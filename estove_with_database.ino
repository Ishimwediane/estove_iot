#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "max6675.h"

// === WiFi Credentials ===
const char* ssid = "IDiane";
const char* password = "123456789";

// === Database Server Configuration ===
const char* serverUrl = "https://estove-server-1.onrender.com";
const char* dataEndpoint = "/api/stove-data";

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
const float MAX_TEMP_CHANGE = 50.0; // Maximum allowed temperature change per reading
const int TEMP_READINGS = 5; // Number of readings to average
float tempReadings[TEMP_READINGS];
int tempIndex = 0;

// === LCD Setup ===
LiquidCrystal_I2C lcd(0x27, 16, 2); // I2C address 0x27, 16x2 display

// === Temperature Control ===
const float MAX_TEMP = 500.0; // Maximum temperature (°C)
const float MIN_TEMP = 80.0;  // Minimum temperature (°C)
bool tempControlEnabled = false; // Only enable temp control during cooking

// === Cooking Timer & Flags ===
unsigned long cookingEndTime = 0;
bool cooking = false;
bool manualMode = false;
unsigned long manualStartTime = 0;
bool manualOverrideDisabled = false;

unsigned long lastLCDUpdate = 0;
unsigned long lastDataSend = 0;
const unsigned long DATA_SEND_INTERVAL = 5000; // Send data every 5 seconds

void setup() {
  Serial.begin(115200);

  // Pins setup
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, HIGH); // Start with relay OFF (HIGH = OFF, LOW = ON)
  pinMode(manualSwitchPin, INPUT_PULLUP); // Switch: pressed = LOW

  // LCD init
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi");

  Serial.println("Connecting to WiFi...");

  // Connect to WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n✅ Connected to WiFi!");
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi Connected");
  lcd.setCursor(0, 1);
  lcd.print(WiFi.localIP());
  Serial.println("ESP32 IP address: " + WiFi.localIP().toString());
  
  Serial.println("Server URL: " + String(serverUrl) + String(dataEndpoint));
  
  // Initialize temperature readings array
  for (int i = 0; i < TEMP_READINGS; i++) {
    tempReadings[i] = 0;
  }
  
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
  } else {
    Serial.println("❌ Error sending data. HTTP code: " + String(httpResponseCode));
  }
  
  http.end();
}

// Function to control relay based on temperature
void controlTemperature(float temperature) {
  if (!tempControlEnabled) return; // Only control temp during cooking
  
  bool currentRelayState = digitalRead(relayPin) == LOW; // LOW = ON, HIGH = OFF
  
  if (temperature >= MAX_TEMP && currentRelayState) {
    // Temperature too high, turn OFF relay
    digitalWrite(relayPin, HIGH);
    Serial.println("🌡️ Temperature too high (" + String(temperature, 1) + "°C), relay OFF");
  } else if (temperature <= MIN_TEMP && !currentRelayState) {
    // Temperature too low, turn ON relay
    digitalWrite(relayPin, LOW);
    Serial.println("🌡️ Temperature too low (" + String(temperature, 1) + "°C), relay ON");
  }
}

// Function to get filtered temperature reading
float getFilteredTemperature() {
  float rawTemp = thermocouple.readCelsius();
  
  // Check for invalid readings (negative or extremely high)
  if (rawTemp < 0 || rawTemp > 1000) {
    Serial.println("⚠️ Invalid temperature reading: " + String(rawTemp));
    return lastValidTemp; // Return last valid reading
  }
  
  // Check for sudden temperature changes
  if (lastValidTemp > 0 && abs(rawTemp - lastValidTemp) > MAX_TEMP_CHANGE) {
    Serial.println("⚠️ Sudden temperature change: " + String(lastValidTemp) + " → " + String(rawTemp));
    return lastValidTemp; // Return last valid reading
  }
  
  // Add to rolling average
  tempReadings[tempIndex] = rawTemp;
  tempIndex = (tempIndex + 1) % TEMP_READINGS;
  
  // Calculate average
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

void loop() {
  bool manualSwitchPressed = digitalRead(manualSwitchPin) == LOW;
  float temperature = getFilteredTemperature();

  // Manual switch control
  if (manualOverrideDisabled) {
    // Ignore switch presses while manual override disabled
    if (!manualSwitchPressed) {
      manualOverrideDisabled = false;
      Serial.println("Manual override disabled cleared");
    }
  } else {
    if (manualSwitchPressed && !manualMode) {
      manualMode = true;
      cooking = false;
      cookingEndTime = 0;
      manualStartTime = millis();
      digitalWrite(relayPin, LOW); // Turn ON relay (LOW = ON)
      tempControlEnabled = false; // Disable temp control in manual mode
      Serial.println("🟢 Manual switch ON: Relay ON");
    } else if (!manualSwitchPressed && manualMode) {
      manualMode = false;
      cooking = false;
      cookingEndTime = 0;
      manualStartTime = 0;
      digitalWrite(relayPin, HIGH); // Turn OFF relay (HIGH = OFF)
      tempControlEnabled = false;
      Serial.println("🔴 Manual switch OFF: Relay OFF");
    }
  }

  // Auto stop timer cooking if expired and not in manual mode
  if (cooking && !manualMode && millis() > cookingEndTime) {
    cooking = false;
    cookingEndTime = 0;
    digitalWrite(relayPin, HIGH); // Turn OFF relay (HIGH = OFF)
    tempControlEnabled = false;
    Serial.println("✅ Cooking timer ended, relay OFF");
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
    lcd.print((char)223); // ° symbol
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
      Serial.println(" °C | Cooking OFF");
    }
  }

  // Send data to server every 5 seconds
  if (millis() - lastDataSend > DATA_SEND_INTERVAL) {
    lastDataSend = millis();
    
    unsigned long timeLeft = 0;
    if (cooking && millis() < cookingEndTime) {
      timeLeft = (cookingEndTime - millis()) / 1000;
    } else if (manualMode) {
      timeLeft = (millis() - manualStartTime) / 1000;
    }
    
    sendDataToServer(temperature, digitalRead(relayPin) == LOW, manualMode, cooking, timeLeft);
  }
} 