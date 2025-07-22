#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "max6675.h"

// === WiFi Credentials ===
const char* ssid = "IDiane";
const char* password = "123456789";

// === Pins ===
const int relayPin = 23;
const int thermoCLK = 18;
const int thermoCS = 5;
const int thermoDO = 19;

// === Thermocouple ===
MAX6675 thermocouple(thermoCLK, thermoCS, thermoDO);

// === Web Server ===
AsyncWebServer server(80);

// === Cooking Timer ===
unsigned long cookingEndTime = 0;
bool cooking = false;

void setup() {
  Serial.begin(115200);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);

  WiFi.begin(ssid, password);
  Serial.println("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ Connected to WiFi!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Enable CORS for browser apps
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

  // === Start Cooking ===
  server.on("/start-cooking", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("seconds", true)) {
      int seconds = request->getParam("seconds", true)->value().toInt();
      cookingEndTime = millis() + (seconds * 1000UL);
      digitalWrite(relayPin, HIGH);
      cooking = true;
      Serial.println("🍳 Cooking started for " + String(seconds) + " seconds");
      request->send(200, "text/plain", "✅ Cooking started");
    } else {
      request->send(400, "text/plain", "❌ Missing 'seconds' parameter");
    }
  });

  // === Stop Cooking ===
  server.on("/stop-cooking", HTTP_GET, [](AsyncWebServerRequest *request){
    cooking = false;
    cookingEndTime = 0;
    digitalWrite(relayPin, LOW);
    Serial.println("❌ Cooking stopped manually");
    request->send(200, "text/plain", "✅ Cooking stopped");
  });

  // === Read Temperature ===
  server.on("/temperature", HTTP_GET, [](AsyncWebServerRequest *request){
    float temp = thermocouple.readCelsius();
    request->send(200, "text/plain", String(temp));
  });

  server.begin();
}

void loop() {
  if (cooking && millis() > cookingEndTime) {
    cooking = false;
    digitalWrite(relayPin, LOW);
    Serial.println("✅ Cooking timer ended, relay OFF");
  }
}
