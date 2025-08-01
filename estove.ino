#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include "max6675.h"

// === WiFi Credentials ===
const char* ssid = "IDiane";
const char* password = "123456789";

// === Static IP Configuration ===
IPAddress local_IP(192, 168, 243, 184);
IPAddress gateway(192, 168, 243, 1);
IPAddress subnet(255, 255, 255, 0);

// === Pins ===
const int relayPin = 23;
const int thermoCLK = 18;
const int thermoCS = 5;
const int thermoDO = 19;
const int manualSwitchPin = 14; // Manual override switch

// === Thermocouple ===
MAX6675 thermocouple(thermoCLK, thermoCS, thermoDO);

// === LCD Setup ===
LiquidCrystal_I2C lcd(0x27, 16, 2); // I2C address 0x27, 16x2 display

// === Web Server ===
AsyncWebServer server(80);

// === Cooking Timer & Flags ===
unsigned long cookingEndTime = 0;
bool cooking = false;
bool manualMode = false;
unsigned long manualStartTime = 0;
bool manualOverrideDisabled = false; // NEW: disable manual override temporarily

unsigned long lastLCDUpdate = 0;

void setup() {
  Serial.begin(115200);

  // Pins setup
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);
  pinMode(manualSwitchPin, INPUT_PULLUP); // Switch: pressed = LOW

  // LCD init
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi");

  // Configure static IP
  if (!WiFi.config(local_IP, gateway, subnet)) {
    Serial.println("❌ STA Failed to configure");
  }

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

  // Start mDNS responder with hostname "estove"
  if (MDNS.begin("estove")) {
    Serial.println("mDNS responder started: estove.local");
  } else {
    Serial.println("Error setting up mDNS responder!");
  }

  // Enable CORS
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

  // === Start Cooking Endpoint ===
  server.on("/start-cooking", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("seconds", true)) {
      if (!manualMode) {
        int seconds = request->getParam("seconds", true)->value().toInt();
        cookingEndTime = millis() + (seconds * 1000UL);
        cooking = true;
        manualStartTime = 0; // Reset manual start timer

        digitalWrite(relayPin, HIGH);

        // Show confirmation on LCD
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Cooking Started");
        lcd.setCursor(0, 1);
        lcd.print("Time: ");
        lcd.print(seconds / 60);
        lcd.print("m");
        lcd.print(seconds % 60);
        lcd.print("s");
        delay(2000);

        Serial.println("🍳 Cooking started for " + String(seconds) + " seconds");
        request->send(200, "text/plain", "✅ Cooking started");
      } else {
        request->send(403, "text/plain", "⚠️ Manual mode active");
      }
    } else {
      request->send(400, "text/plain", "❌ Missing 'seconds' parameter");
    }
  });

  // === Stop Cooking Endpoint ===
  server.on("/stop-cooking", HTTP_GET, [](AsyncWebServerRequest *request) {
    cooking = false;
    cookingEndTime = 0;
    manualMode = false;
    manualStartTime = 0;
    digitalWrite(relayPin, LOW);

    // Disable manual override temporarily to allow manual switch to be ignored until released
    manualOverrideDisabled = true;

    Serial.println("❌ Cooking stopped manually (API), manual override disabled");
    request->send(200, "text/plain", "✅ Cooking stopped");
  });

  // === Temperature Endpoint ===
  server.on("/temperature", HTTP_GET, [](AsyncWebServerRequest *request) {
    float temp = thermocouple.readCelsius();
    request->send(200, "text/plain", String(temp));
  });

  // === Status Endpoint ===
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    float temp = thermocouple.readCelsius();
    unsigned long timeLeft = 0;
    bool isCooking = cooking;

    if (cooking && millis() < cookingEndTime) {
      timeLeft = (cookingEndTime - millis()) / 1000;
    } else if (manualMode) {
      // In manual mode, timeLeft = elapsed time since manual started
      timeLeft = (millis() - manualStartTime) / 1000;
      isCooking = true;
    } else {
      isCooking = false;
      timeLeft = 0;
    }

    String json = "{";
    json += "\"relay\":" + String(digitalRead(relayPin) == HIGH ? "true" : "false") + ",";
    json += "\"manualMode\":" + String(manualMode ? "true" : "false") + ",";
    json += "\"cooking\":" + String(isCooking ? "true" : "false") + ",";
    json += "\"timeLeft\":" + String(timeLeft) + ",";
    json += "\"temperature\":" + String(temp, 1);
    json += "}";

    request->send(200, "application/json", json);
  });

  server.begin();
}

void loop() {
  bool manualSwitchPressed = digitalRead(manualSwitchPin) == LOW;

  if (manualOverrideDisabled) {
    // Ignore switch presses while manual override disabled (after web stop)
    if (!manualSwitchPressed) {
      manualOverrideDisabled = false; // Reset when switch released
      Serial.println("Manual override disabled cleared");
    }
  } else {
    if (manualSwitchPressed && !manualMode) {
      manualMode = true;
      cooking = false;          // stop any timed cooking
      cookingEndTime = 0;
      manualStartTime = millis();
      digitalWrite(relayPin, HIGH);
      Serial.println("🟢 Manual switch ON: Relay ON");
    } else if (!manualSwitchPressed && manualMode) {
      manualMode = false;
      cooking = false;
      cookingEndTime = 0;
      manualStartTime = 0;
      digitalWrite(relayPin, LOW);
      Serial.println("🔴 Manual switch OFF: Relay OFF");
    }
  }

  // Auto stop timer cooking if expired and not in manual mode
  if (cooking && !manualMode && millis() > cookingEndTime) {
    cooking = false;
    cookingEndTime = 0;
    digitalWrite(relayPin, LOW);
    Serial.println("✅ Cooking timer ended, relay OFF");
  }

  // LCD & Serial output update every second
  if (millis() - lastLCDUpdate > 1000) {
    lastLCDUpdate = millis();
    float temp = thermocouple.readCelsius();

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Temp: ");
    lcd.print(temp, 1);
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
      Serial.print(temp);
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
      Serial.print(temp);
      Serial.print(" °C | Time Left: ");
      Serial.print(minutes);
      Serial.print("m ");
      Serial.print(seconds);
      Serial.println("s");
    } else {
      lcd.print("Cooking: OFF");
      Serial.print("🧊 Temp: ");
      Serial.print(temp);
      Serial.println(" °C | Cooking OFF");
    }
  }
}
