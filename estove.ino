#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "max6675.h"
#include <ArduinoJson.h>

// === WiFi Credentials ===
const char* ssid = "IDiane";
const char* password = "123456789";

// === Database Server Configuration ===
const char* serverUrl = "https://estove-server-1.onrender.com";
const char* dataEndpoint = "/api/stove-data";
const char* commandsEndpoint = "/api/commands/pending";
const char* commandProcessedEndpoint = "/api/commands/processed";

// === Pins ===
const int relayPin = 23;
const int thermoCLK = 18;
const int thermoCS = 5;
const int thermoDO = 19;
const int manualSwitchPin = 14; // Manual override switch

// === Thermocouple ===
MAX6675 thermocouple(thermoCLK, thermoCS, thermoDO);

// === Temperature Filtering ===
float lastValidTemp = 0;
const float MAX_TEMP_CHANGE = 50.0;
const int TEMP_READINGS = 5;
float tempReadings[TEMP_READINGS];
int tempIndex = 0;

// === LCD Setup ===
LiquidCrystal_I2C lcd(0x27, 16, 2);

// === Temperature Control ===
const float MAX_TEMP = 500.0;
const float MIN_TEMP = 80.0;
bool tempControlEnabled = false;

// === Cooking Timer & Flags ===
unsigned long cookingEndTime = 0;
bool cooking = false;
bool manualMode = false;
unsigned long manualStartTime = 0;
bool manualOverrideDisabled = false;
bool switchWasPressed = false; // Track previous switch state

// === Web Control Integration ===
unsigned long lastCommandCheck = 0;
const unsigned long COMMAND_CHECK_INTERVAL = 1000;
String lastProcessedCommandId = "";

// === Safety & Monitoring ===
unsigned long lastSuccessfulConnection = 0;
const unsigned long CONNECTION_TIMEOUT = 30000;
bool connectionWarningShown = false;

unsigned long lastLCDUpdate = 0;
unsigned long lastDataSend = 0;
const unsigned long DATA_SEND_INTERVAL = 1000;

void setup() {
  Serial.begin(115200);

  // Pins setup
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, HIGH); // Start with relay OFF (HIGH = OFF, LOW = ON)
  pinMode(manualSwitchPin, INPUT_PULLUP);

  // LCD init
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi");

  Serial.println("Connecting to WiFi...");

  // Connect to WiFi
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 60) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Connected to WiFi!");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Connected");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
    Serial.println("ESP32 IP address: " + WiFi.localIP().toString());
    lastSuccessfulConnection = millis();
  } else {
    Serial.println("\n❌ WiFi connection failed!");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Failed!");
    lcd.setCursor(0, 1);
    lcd.print("Check Credentials");
  }
  
  Serial.println("Server URL: " + String(serverUrl) + String(dataEndpoint));
  
  // Initialize temperature readings array
  for (int i = 0; i < TEMP_READINGS; i++) {
    tempReadings[i] = 0;
  }
  
  // Test switch pin
  Serial.println("🔘 Testing switch pin " + String(manualSwitchPin));
  Serial.println("🔘 Initial switch state: " + String(digitalRead(manualSwitchPin) == LOW ? "PRESSED" : "NOT PRESSED"));
  
  delay(2000);
}

// Function to send data to database server
void sendDataToServer(float temperature, bool relay, bool manualMode, bool cooking, unsigned long timeLeft) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected, cannot send data");
    return;
  }

  HTTPClient http;
  String url = String(serverUrl) + String(dataEndpoint);
  
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);

  // Create JSON payload
  String jsonPayload = "{";
  jsonPayload += "\"temperature\":" + String(temperature, 1) + ",";
  jsonPayload += "\"relay\":" + String(relay ? "true" : "false") + ",";
  jsonPayload += "\"manualMode\":" + String(manualMode ? "true" : "false") + ",";
  jsonPayload += "\"cooking\":" + String(cooking ? "true" : "false") + ",";
  jsonPayload += "\"timeLeft\":" + String(timeLeft);
  jsonPayload += "}";

  Serial.println("📤 Sending data to server: " + jsonPayload);

  int httpResponseCode = http.POST(jsonPayload);
  
  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("✅ Data sent successfully. Response: " + response);
    lastSuccessfulConnection = millis();
    connectionWarningShown = false;
  } else {
    Serial.println("❌ Error sending data. HTTP code: " + String(httpResponseCode));
  }
  
  http.end();
}

// Function to check for web commands
void checkWebCommands() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected, cannot check commands");
    return;
  }

  HTTPClient http;
  String url = String(serverUrl) + String(commandsEndpoint);
  
  http.begin(url);
  http.setTimeout(10000);
  int httpResponseCode = http.GET();
  
  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("📥 Received command: " + response);
    
    // Parse JSON response
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, response);
    
    if (!error && doc.containsKey("_id") && doc.containsKey("command")) {
      String commandId = doc["_id"].as<String>();
      String command = doc["command"].as<String>();
      int seconds = doc["seconds"] | 0;
      
      // Check if this is a new command
      if (commandId != lastProcessedCommandId) {
        Serial.println("🔄 Processing command: " + command + " (ID: " + commandId + ")");
        
        if (command == "start") {
          // Start cooking with timer (only if not already cooking)
          if (!cooking && !manualMode) {
            Serial.println("🍳 Web command: Start cooking for " + String(seconds) + " seconds");
            cooking = true;
            manualMode = false;
            manualStartTime = 0;
            cookingEndTime = millis() + (seconds * 1000);
            digitalWrite(relayPin, LOW); // Turn ON relay
            tempControlEnabled = true;
            manualOverrideDisabled = false; // Allow manual switch to stop
          } else {
            Serial.println("⚠️ Web command: Cannot start - already cooking");
          }
          
        } else if (command == "stop") {
          // Stop cooking (can stop anytime)
          Serial.println("⏹️ Web command: Stop cooking - Relay OFF");
          cooking = false;
          manualMode = false;
          cookingEndTime = 0;
          manualStartTime = 0;
          digitalWrite(relayPin, HIGH); // Turn OFF relay
          tempControlEnabled = false;
          manualOverrideDisabled = false; // Allow manual switch to work normally
          
        } else if (command == "manual_on") {
          // Activate manual mode (only if not already cooking)
          if (!cooking && !manualMode) {
            Serial.println("🔌 Web command: Manual mode ON");
            manualMode = true;
            cooking = false;
            cookingEndTime = 0;
            manualStartTime = millis();
            digitalWrite(relayPin, LOW); // Turn ON relay
            tempControlEnabled = false;
            manualOverrideDisabled = false; // Allow manual switch to stop
          } else {
            Serial.println("⚠️ Web command: Cannot start manual mode - already cooking");
          }
          
        } else if (command == "manual_off") {
          // Deactivate manual mode (can stop anytime)
          Serial.println("🔌 Web command: Manual mode OFF");
          manualMode = false;
          cooking = false;
          cookingEndTime = 0;
          manualStartTime = 0;
          digitalWrite(relayPin, HIGH); // Turn OFF relay
          tempControlEnabled = false;
          manualOverrideDisabled = false;
        }
        
        // Mark command as processed
        markCommandAsProcessed(commandId);
        lastProcessedCommandId = commandId;
      }
    }
    
  } else {
    Serial.println("❌ Error checking commands. HTTP code: " + String(httpResponseCode));
  }
  
  http.end();
}

// Function to mark command as processed
void markCommandAsProcessed(String commandId) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected, cannot mark command as processed");
    return;
  }

  HTTPClient http;
  String url = String(serverUrl) + String(commandProcessedEndpoint);
  
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  String jsonPayload = "{\"commandId\":\"" + commandId + "\"}";
  
  int httpResponseCode = http.POST(jsonPayload);
  
  if (httpResponseCode > 0) {
    Serial.println("✅ Command marked as processed");
  } else {
    Serial.println("❌ Error marking command as processed. HTTP code: " + String(httpResponseCode));
  }
  
  http.end();
}

// Function to control relay based on temperature
void controlTemperature(float temperature) {
  if (!tempControlEnabled) return;
  
  bool currentRelayState = digitalRead(relayPin) == LOW;
  
  if (temperature >= MAX_TEMP && currentRelayState) {
    digitalWrite(relayPin, HIGH);
    Serial.println("🌡️ Temperature too high (" + String(temperature, 1) + "°C), relay OFF");
  } else if (temperature <= MIN_TEMP && !currentRelayState) {
    digitalWrite(relayPin, LOW);
    Serial.println("🌡️ Temperature too low (" + String(temperature, 1) + "°C), relay ON");
  }
}

// Function to get filtered temperature reading
float getFilteredTemperature() {
  float rawTemp = thermocouple.readCelsius();
  
  if (rawTemp < 0 || rawTemp > 1000) {
    Serial.println("⚠️ Invalid temperature reading: " + String(rawTemp));
    return lastValidTemp;
  }
  
  if (lastValidTemp > 0 && abs(rawTemp - lastValidTemp) > MAX_TEMP_CHANGE) {
    Serial.println("⚠️ Sudden temperature change: " + String(lastValidTemp) + " → " + String(rawTemp));
    return lastValidTemp;
  }
  
  tempReadings[tempIndex] = rawTemp;
  tempIndex = (tempIndex + 1) % TEMP_READINGS;
  
  float sum = 0;
  int validReadings = 0;
  for (int i = 0; i < TEMP_READINGS; i++) {
    if (tempReadings[i] > 0) {
      sum += tempReadings[i];
      validReadings++;
    }
  }
  
  float avgTemp = (validReadings > 0) ? sum / validReadings : rawTemp;
  lastValidTemp = avgTemp;
  
  return avgTemp;
}

// Function to check connection status and show warnings
void checkConnectionStatus() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!connectionWarningShown) {
      Serial.println("⚠️ WiFi connection lost!");
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("WiFi Disconnected");
      lcd.setCursor(0, 1);
      lcd.print("Reconnecting...");
      connectionWarningShown = true;
    }
    
    WiFi.reconnect();
  } else if (connectionWarningShown) {
    Serial.println("✅ WiFi reconnected!");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Reconnected");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
    connectionWarningShown = false;
    lastSuccessfulConnection = millis();
    delay(2000);
  }
}

void loop() {
  bool manualSwitchPressed = digitalRead(manualSwitchPin) == LOW;
  float temperature = getFilteredTemperature();

  // Track switch state changes
  bool switchJustReleased = switchWasPressed && !manualSwitchPressed;
  
  // Debug switch state (only print when state changes)
  static bool lastSwitchState = false;
  if (manualSwitchPressed != lastSwitchState) {
    Serial.println("🔘 Switch state changed: " + String(lastSwitchState) + " → " + String(manualSwitchPressed));
    lastSwitchState = manualSwitchPressed;
  }
  
  // Check for switch press (rising edge detection)
  bool switchJustPressed = manualSwitchPressed && !switchWasPressed;
  
  // Update switch state for next iteration
  switchWasPressed = manualSwitchPressed;

  // Check connection status
  checkConnectionStatus();

  // Check for web commands
  if (millis() - lastCommandCheck > COMMAND_CHECK_INTERVAL) {
    lastCommandCheck = millis();
    checkWebCommands();
  }

  // Manual switch control - can start when OFF, can stop when ON
  if (switchJustPressed) { // Switch just pressed (rising edge)
    Serial.println("🔘 Switch pressed - Current state: cooking=" + String(cooking) + ", manualMode=" + String(manualMode));
    
    if (!cooking && !manualMode) {
      // Start cooking in manual mode when everything is OFF
      manualMode = true;
      cooking = false;
      cookingEndTime = 0;
      manualStartTime = millis();
      digitalWrite(relayPin, LOW); // Turn ON relay
      tempControlEnabled = false;
      manualOverrideDisabled = false; // Clear any web override
      Serial.println("🟢 Manual switch pressed: Starting manual mode");
    } else if (cooking || manualMode) {
      // Stop cooking when it's ON (either timer or manual mode)
      cooking = false;
      manualMode = false;
      cookingEndTime = 0;
      manualStartTime = 0;
      digitalWrite(relayPin, HIGH); // Turn OFF relay
      tempControlEnabled = false;
      manualOverrideDisabled = false; // Clear any web override
      Serial.println("🔴 Manual switch pressed: Stopping cooking");
    }
  }

  // Auto stop timer cooking if expired and not in manual mode
  if (cooking && !manualMode && millis() > cookingEndTime) {
    cooking = false;
    cookingEndTime = 0;
    digitalWrite(relayPin, HIGH); // Turn OFF relay
    tempControlEnabled = false;
    // Don't immediately re-enable manual override - let switch release handle it
    Serial.println("✅ Cooking timer ended, relay OFF");
    
    // Update LCD immediately when timer ends
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Temp: ");
    lcd.print(temperature, 1);
    lcd.print((char)223);
    lcd.print("C");
    lcd.setCursor(0, 1);
    lcd.print("Cooking: OFF");
  }

  // Temperature control during cooking
  if (cooking && !manualMode) {
    tempControlEnabled = true;
    controlTemperature(temperature);
  }

  // LCD & Serial output update every second
  if (millis() - lastLCDUpdate > 1000) {
    lastLCDUpdate = millis();

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Temp: ");
    lcd.print(temperature, 1);
    lcd.print((char)223);
    lcd.print("C");

    lcd.setCursor(0, 1);
    if (manualMode) {
      unsigned long elapsed = (millis() - manualStartTime) / 1000;
      int minutes = elapsed / 60;
      int seconds = elapsed % 60;

      lcd.print("Manual: ON ");
      if (minutes < 10) lcd.print("0");
      lcd.print(minutes);
      lcd.print(":");
      if (seconds < 10) lcd.print("0");
      lcd.print(seconds);

      Serial.print("🟢 Temp: ");
      Serial.print(temperature);
      Serial.print(" °C | Manual Mode ON | Elapsed: ");
      Serial.print(minutes);
      Serial.print("m ");
      Serial.print(seconds);
      Serial.println("s");
    } else if (cooking) {
      unsigned long timeLeft = (cookingEndTime > millis()) ? (cookingEndTime - millis()) / 1000 : 0;
      int minutes = timeLeft / 60;
      int seconds = timeLeft % 60;

      lcd.print("Time: ");
      if (minutes < 10) lcd.print("0");
      lcd.print(minutes);
      lcd.print(":");
      if (seconds < 10) lcd.print("0");
      lcd.print(seconds);

      Serial.print("🔥 Cooking - Temp: ");
      Serial.print(temperature);
      Serial.print(" °C | Time Left: ");
      Serial.print(minutes);
      Serial.print("m ");
      Serial.print(seconds);
      Serial.println("s");
         } else {
       lcd.print("Cooking: OFF");
       Serial.print("🧊 Temp: ");
       Serial.print(temperature);
       Serial.println(" °C | Cooking OFF | Ready to start");
     }
  }

        // Send data to server every 1 second
  if (millis() - lastDataSend > DATA_SEND_INTERVAL) {
    lastDataSend = millis();
    
    unsigned long timeLeft = 0;
    if (cooking && millis() < cookingEndTime) {
      timeLeft = (cookingEndTime - millis()) / 1000;
    } else if (manualMode) {
      timeLeft = (millis() - manualStartTime) / 1000;
    }
    
    sendDataToServer(temperature, digitalRead(relayPin) == LOW, manualMode, cooking, timeLeft);
    
    // Debug: Print switch state every 5 seconds
    static unsigned long lastSwitchDebug = 0;
    if (millis() - lastSwitchDebug > 5000) {
      lastSwitchDebug = millis();
      Serial.println("🔘 Switch debug - Pin " + String(manualSwitchPin) + " state: " + String(digitalRead(manualSwitchPin) == LOW ? "PRESSED" : "NOT PRESSED"));
      Serial.println("🔘 Switch variables - manualSwitchPressed: " + String(manualSwitchPressed) + ", switchWasPressed: " + String(switchWasPressed));
    }
  }
}