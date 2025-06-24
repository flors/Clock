#include <WiFi.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// Define a GPIO pin for the reset button
#define RESET_PIN 0  // GPIO0 is often the BOOT button on many ESP32 boards

// Define a GPIO pin for the sensor/button
#define SENSOR_PIN 4  // GPIO pin for your sensor or button (changed from 2)

// Define the internal LED pin (GPIO2 on most ESP32 boards)
#define LED_PIN 2  // GPIO2 is the built-in LED on most ESP32 boards

// For data sending interval
unsigned long lastSendTime = 0;
const long sendInterval = 5000; // Send data every 5 seconds

// For group status polling interval
unsigned long lastGroupPollTime = 0;
const long groupPollInterval = 3000; // Poll group status every 3 seconds

// For saving WiFi credentials to flash memory
Preferences preferences;

// Create web server and DNS server for captive portal
WebServer server(80);
DNSServer dnsServer;

// Credentials for AP mode
const char* apSSID = "ESP32-Setup";
const char* apPassword = "configesp32";

// Variables to store your network credentials
String staSSID;
String staPassword;

// Backend server settings - Change these to match your server
String serverIP = "esp-backend-f4e8.onrender.com"; // Your Render URL (without https://)
int serverPort = 443;                               // HTTPS port for Render
String deviceId = "esp32-001";                      // Unique ID for this device
String groupId = "group-001";                       // Group ID for this device
bool useHTTPS = true;                               // Flag to use HTTPS for Render

// DNS server port (53 is default)
const byte DNS_PORT = 53;

// Flag to track whether we're in AP mode
bool isAPMode = false;

// Variable to store the sensor state
bool sensorState = false;

// Variable to store the LED state (controlled by group status)
bool ledState = false;

// Flag to track if initial status has been sent
bool initialStatusSent = false;

// Initialize the sensor pin and LED pin
void setupSensor() {
  pinMode(SENSOR_PIN, INPUT_PULLUP);  // Use pull-up for a button
  pinMode(LED_PIN, OUTPUT);           // Set LED pin as output
  digitalWrite(LED_PIN, LOW);         // Start with LED off
  
  // Ensure pins are different
  if (SENSOR_PIN == LED_PIN) {
    Serial.println("ERROR: SENSOR_PIN and LED_PIN cannot be the same!");
    return;
  }
  
  // Initialize states to OFF
  sensorState = false;
  ledState = false;
  initialStatusSent = false;
  
  Serial.println("Sensor initialized on pin " + String(SENSOR_PIN) + " (INPUT_PULLUP)");
  Serial.println("LED initialized on pin " + String(LED_PIN) + " (OUTPUT)");
  
  // Test initial states
  Serial.println("Initial sensor state: OFF (forced)");
  Serial.println("Initial LED state: OFF");
  Serial.println("Device will send initial OFF status to server on first connection");
}

// Read the sensor state
bool readSensor() {
  // During startup phase, always return false until initial status is sent
  if (!initialStatusSent) {
    return false;
  }
  
  // Read the pin (invert if using pull-up with a button)
  bool currentSensorState = digitalRead(SENSOR_PIN); // Invert for pull-up logic / saque esto porque estaba invertido
  
  // Add some debug output to track sensor vs LED
  static bool lastSensorState = false;
  if (currentSensorState != lastSensorState) {
    Serial.println("SENSOR state changed: " + String(currentSensorState ? "PRESSED" : "RELEASED") + " (pin " + String(SENSOR_PIN) + ")");
    lastSensorState = currentSensorState;
  }
  
  return currentSensorState;
}

// Control the LED based on group status
void updateLED(bool shouldBeOn) {
  if (ledState != shouldBeOn) {
    ledState = shouldBeOn;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    Serial.println("LED state changed: " + String(ledState ? "ON" : "OFF") + " (pin " + String(LED_PIN) + ") - triggered by group activity");
  }
}

// Read a file from SPIFFS
String readFile(const char* path) {
  if (!SPIFFS.exists(path)) {
    Serial.println("File not found: " + String(path));
    return "File not found";
  }
  
  File file = SPIFFS.open(path, "r");
  if (!file) {
    Serial.println("Failed to open file for reading");
    return String("Error opening file");
  }
  
  String fileContent;
  while (file.available()) {
    fileContent += (char)file.read();
  }
  file.close();
  return fileContent;
}

// Replace placeholders in a template
String processTemplate(String content) {
  content.replace("{{serverIP}}", serverIP);
  content.replace("{{serverPort}}", String(serverPort));
  content.replace("{{deviceId}}", deviceId);
  content.replace("{{groupId}}", groupId);
  content.replace("{{sensorState}}", sensorState ? "ON" : "OFF");
  content.replace("{{ledState}}", ledState ? "ON" : "OFF");  // Add LED state to template
  
  // Handle WiFi status specifically
  if (WiFi.status() == WL_CONNECTED) {
    content.replace("{{wifiStatus}}", "Connected to WiFi");
    String details = "<p><strong>SSID:</strong> " + WiFi.SSID() + "</p>";
    details += "<p><strong>IP Address:</strong> " + WiFi.localIP().toString() + "</p>";
    content.replace("{{wifiDetails}}", details);
  } else {
    content.replace("{{wifiStatus}}", "Not connected to WiFi");
    content.replace("{{wifiDetails}}", "");
  }
  
  return content;
}

// Setup the web server routes
void setupWebServer() {
  // Root route - serve info about the device
  server.on("/", HTTP_GET, []() {
    // DON'T update sensor state from web interface during startup
    // Use the current sensorState variable instead of reading pin directly
    if (initialStatusSent) {
      sensorState = readSensor(); // Only read actual sensor after initial status sent
    }
    // Otherwise use the initialized sensorState (which is false)
    
    String html = readFile("/home.html");
    html = processTemplate(html);
    server.send(200, "text/html", html);
  });
  
  // WiFi configuration page
  server.on("/wifi", HTTP_GET, []() {
    String html = readFile("/index.html");
    server.send(200, "text/html", html);
  });
  
  // Handle WiFi form submission
  server.on("/save", HTTP_POST, []() {
    String newSSID = server.arg("ssid");
    String newPassword = server.arg("password");
    
    // Save the new credentials
    preferences.putString("ssid", newSSID);
    preferences.putString("password", newPassword);
    
    Serial.println("New credentials saved:");
    Serial.println("SSID: " + newSSID);
    Serial.print("Password: ");
    Serial.println(newPassword.length() > 0 ? "[saved]" : "[not set]");
    
    // Send success page
    String html = readFile("/success.html");
    server.send(200, "text/html", html);
    
    // Wait a moment and restart
    delay(3000);
    ESP.restart();
  });
  
  // Add a route to configure the backend server
  server.on("/config", HTTP_GET, []() {
    String html = readFile("/config.html");
    html = processTemplate(html);
    server.send(200, "text/html", html);
  });
  
  // Handle backend config submission
  server.on("/saveconfig", HTTP_POST, []() {
    serverIP = server.arg("serverip");
    serverPort = server.arg("serverport").toInt();
    deviceId = server.arg("deviceid");
    groupId = server.arg("groupid");
    
    // Save to preferences
    preferences.putString("serverip", serverIP);
    preferences.putInt("serverport", serverPort);
    preferences.putString("deviceid", deviceId);
    preferences.putString("groupid", groupId);
    preferences.putBool("usehttps", useHTTPS);
    
    Serial.println("Device config saved:");
    Serial.println("Server IP: " + serverIP);
    Serial.println("Server Port: " + String(serverPort));
    Serial.println("Device ID: " + deviceId);
    Serial.println("Group ID: " + groupId);
    
    server.send(200, "text/html", "<html><body><h1>Configuration Saved</h1><p>Device configuration has been saved.</p><a href='/'>Back to main page</a></body></html>");
  });
  
  // Add a route to reset credentials
  server.on("/reset", HTTP_GET, []() {
    String html = readFile("/reset.html");
    server.send(200, "text/html", html);
  });
  
  server.on("/confirmreset", HTTP_GET, []() {
    preferences.clear();
    server.send(200, "text/plain", "Credentials cleared. Restarting...");
    delay(1000);
    ESP.restart();
  });
  
  // Only set up DNS redirect when in AP mode
  if (isAPMode) {
    // For captive portal - redirect all requests to our server
    server.onNotFound([]() {
      server.sendHeader("Location", "http://" + WiFi.softAPIP().toString(), true);
      server.send(302, "text/plain", "");
    });
  } else {
    // Normal 404 handler when not in AP mode
    server.onNotFound([]() {
      server.send(404, "text/plain", "Not Found");
    });
  }
  
  // Start the server
  server.begin();
  Serial.println("HTTP server started");
}

// Function to start the AP mode
void startAPMode() {
  // Start AP mode
  WiFi.softAP(apSSID, apPassword);
  
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);
  
  // Start the DNS server for captive portal
  dnsServer.start(DNS_PORT, "*", IP);
  Serial.println("DNS server started");
  
  isAPMode = true;
  
  Serial.println("Connect to WiFi network: " + String(apSSID));
  Serial.println("Password: " + String(apPassword));
  Serial.println("Then navigate to http://" + IP.toString() + " in your browser");
}

// Send device data to server via HTTP/HTTPS
void sendDataViaHTTP() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  // On first connection, force send initial OFF state regardless of actual sensor
  if (!initialStatusSent) {
    sensorState = false; // Force OFF on startup
    Serial.println("Sending FORCED initial OFF status to server (startup override)");
  } else {
    // Read current sensor state ONLY from the physical sensor
    bool currentSensorState = readSensor();
    
    // Only send data if sensor state actually changed, or every 30 seconds regardless
    static unsigned long lastForcedSend = 0;
    static bool lastSentSensorState = false;
    
    bool shouldSend = (currentSensorState != lastSentSensorState) || 
                     (millis() - lastForcedSend > 30000);
    
    if (!shouldSend) return;
    
    sensorState = currentSensorState; // Update global variable
    lastSentSensorState = currentSensorState;
    lastForcedSend = millis();
  }
  
  HTTPClient http;
  WiFiClientSecure *client = nullptr;
  
  String url;
  if (useHTTPS) {
    client = new WiFiClientSecure;
    client->setInsecure(); // Skip certificate verification for simplicity
    url = "https://" + serverIP + "/api/data";
    http.begin(*client, url);
  } else {
    url = "http://" + serverIP + ":" + String(serverPort) + "/api/data";
    http.begin(url);
  }
  
  http.addHeader("Content-Type", "application/json");
  
  // Create JSON document
  DynamicJsonDocument doc(256);
  doc["deviceId"] = deviceId;
  doc["groupId"] = groupId;
  doc["sensorState"] = sensorState;  // This should ALWAYS be false on first send
  
  // Serialize and send
  String json;
  serializeJson(doc, json);
  
  if (!initialStatusSent) {
    Serial.println("Sending JSON: " + json);  // Debug the actual JSON being sent
    Serial.println("Sending FORCED OFF sensor data (startup override)");
  } else {
    Serial.println("Sending sensor data: " + String(sensorState ? "ON" : "OFF") + " from pin " + String(SENSOR_PIN));
  }
  
  int httpResponseCode = http.POST(json);
  
  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("Data sent - HTTP Response: " + String(httpResponseCode));
    Serial.println("Server response: " + response);  // See what server actually received
    
    // Mark initial status as sent after successful response
    if (!initialStatusSent) {
      initialStatusSent = true;
      Serial.println("Initial FORCED OFF status successfully sent to server");
    }
  } else {
    Serial.println("Error sending data: " + String(httpResponseCode));
    // Reset flag if sending failed, so we try again
    if (!initialStatusSent) {
      initialStatusSent = false;
    }
  }
  
  http.end();
  if (client) delete client;
}

// NEW: Poll group status and update LED
void pollGroupStatus() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  HTTPClient http;
  WiFiClientSecure *client = nullptr;
  
  String url;
  if (useHTTPS) {
    client = new WiFiClientSecure;
    client->setInsecure(); // Skip certificate verification for simplicity
    url = "https://" + serverIP + "/api/groups/" + groupId + "/status?excludeDevice=" + deviceId;
    http.begin(*client, url);
  } else {
    url = "http://" + serverIP + ":" + String(serverPort) + 
          "/api/groups/" + groupId + "/status?excludeDevice=" + deviceId;
    http.begin(url);
  }
  
  http.addHeader("Content-Type", "application/json");
  
  int httpResponseCode = http.GET();
  
  if (httpResponseCode == 200) {
    String response = http.getString();
    
    // Parse JSON response
    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, response);
    
    if (!error) {
      bool hasActiveDevices = doc["hasActiveDevices"] | false;
      
      // Update LED based on group status
      updateLED(hasActiveDevices);
      
      // Optional: Print active devices for debugging
      if (hasActiveDevices) {
        JsonArray activeDevices = doc["activeDevices"];
        Serial.print("Group " + groupId + " has active devices: ");
        for (JsonVariant device : activeDevices) {
          Serial.print(device.as<String>() + " ");
        }
        Serial.println();
      }
    } else {
      Serial.println("Error parsing group status JSON: " + String(error.c_str()));
    }
  } else if (httpResponseCode > 0) {
    Serial.println("Group status poll error: " + String(httpResponseCode));
  }
  // If httpResponseCode <= 0, it's likely a network error, so we don't spam the serial
  
  http.end();
  if (client) delete client;
}

// Check if reset is needed and clear preferences
bool checkForReset() {
  // Set the reset pin as input with pull-up
  pinMode(RESET_PIN, INPUT_PULLUP);
  
  // Check if the button is pressed (LOW)
  if (digitalRead(RESET_PIN) == LOW) {
    Serial.println("Reset button pressed, waiting for confirmation...");
    
    // Wait to make sure it's not a bounce
    delay(500);
    
    // Check again to make sure button is still pressed
    if (digitalRead(RESET_PIN) == LOW) {
      Serial.println("Reset confirmed! Clearing preferences...");
      
      // Open and clear preferences
      preferences.begin("wifi-config", false);
      preferences.clear();
      preferences.end();
      
      Serial.println("Preferences cleared! Restarting...");
      delay(1000);
      ESP.restart();
      return true;
    }
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== ESP32 IoT Device with Group LED Control ===");
  Serial.println("Hold the BOOT button (GPIO0) during startup to reset configurations");
  Serial.println("DEBUG: Device starting with FORCED OFF states");
  Serial.println("DEBUG: sensorState = " + String(sensorState));
  Serial.println("DEBUG: ledState = " + String(ledState));
  Serial.println("DEBUG: initialStatusSent = " + String(initialStatusSent));
  
  // Check for reset button before doing anything else
  if (checkForReset()) {
    return; // If reset was performed, just return (though we will have restarted)
  }
  
  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("An error occurred while mounting SPIFFS");
    Serial.println("Formatting...");
    SPIFFS.format();
    if (!SPIFFS.begin()) {
      Serial.println("Failed to mount SPIFFS even after formatting");
      return;
    }
  }
  Serial.println("SPIFFS mounted successfully");
  
  // Initialize preferences
  preferences.begin("wifi-config", false);
  
  // Get saved WiFi credentials
  staSSID = preferences.getString("ssid", "");
  staPassword = preferences.getString("password", "");
  
  // Get saved backend server configuration
  serverIP = preferences.getString("serverip", serverIP);
  serverPort = preferences.getInt("serverport", serverPort);
  deviceId = preferences.getString("deviceid", deviceId);
  groupId = preferences.getString("groupid", groupId);
  useHTTPS = preferences.getBool("usehttps", useHTTPS);
  
  Serial.println("Saved credentials:");
  Serial.println("SSID: " + staSSID);
  Serial.print("Password: ");
  Serial.println(staPassword.length() > 0 ? "[saved]" : "[not set]");
  
  Serial.println("Device configuration:");
  Serial.println("Server IP: " + serverIP);
  Serial.println("Server Port: " + String(serverPort));
  Serial.println("Device ID: " + deviceId);
  Serial.println("Group ID: " + groupId);

  // Initialize the sensor and LED
  setupSensor();
  
  // Add option to reset via serial command
  Serial.println("\nTo reset configurations, you can also type 'reset' in the Serial Monitor");
  Serial.println("LED will turn ON when other devices in group " + groupId + " have sensors active");
  
  // Try to connect to WiFi if we have credentials
  bool wifiConnected = false;
  if (staSSID.length() > 0) {
    WiFi.begin(staSSID.c_str(), staPassword.c_str());
    
    Serial.print("Connecting to WiFi");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    Serial.println();
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Connected to WiFi");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());
      Serial.println("You can access the configuration at http://" + WiFi.localIP().toString());
      wifiConnected = true;
    } else {
      Serial.println("Failed to connect to saved network");
    }
  }
  
  // If WiFi connection failed, start AP mode
  if (!wifiConnected) {
    Serial.println("Starting Access Point...");
    startAPMode();
  }
  
  // Setup the web server (for both AP and STA modes)
  setupWebServer();
}

void loop() {
  // Check for serial commands
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    
    if (command == "reset") {
      Serial.println("Resetting configuration...");
      preferences.clear();
      Serial.println("Configuration reset! Restarting...");
      delay(1000);
      ESP.restart();
    }
  }
  
  // Handle DNS requests if in AP mode
  if (isAPMode) {
    dnsServer.processNextRequest();
  }
  
  // Always handle HTTP requests
  server.handleClient();
  
  // Send sensor data and poll group status if connected to WiFi
  if (WiFi.status() == WL_CONNECTED) {
    
    unsigned long currentMillis = millis();
    
    // Send initial status immediately on first connection, then follow normal intervals
    if (!initialStatusSent) {
      sendDataViaHTTP(); // Send initial OFF status
    } else {
      // Send sensor data
      if (currentMillis - lastSendTime >= sendInterval) {
        lastSendTime = currentMillis;
        sendDataViaHTTP();
      }
    }
    
    // Poll group status (only after initial status is sent)
    if (initialStatusSent && currentMillis - lastGroupPollTime >= groupPollInterval) {
      lastGroupPollTime = currentMillis;
      pollGroupStatus();
    }
  }
  
  // Always read sensor (in case we need it for web interface)
  readSensor();
}