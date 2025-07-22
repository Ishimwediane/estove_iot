#include "max6675.h"

int thermoCLK = 18;
int thermoCS = 5;
int thermoDO = 19;

int relayPin = 23;  // GPIO23 for relay

MAX6675 thermocouple(thermoCLK, thermoCS, thermoDO);

void setup() {
  Serial.begin(115200);
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);  // initially OFF
}

void loop() {
  float temp = thermocouple.readCelsius();
  Serial.print("Temperature: ");
  Serial.println(temp);

  if (temp < 90) { // You can change to your cooking target
    digitalWrite(relayPin, HIGH);  // Turn heater ON
    Serial.println("Cooking… Heater ON");
  } else {
    digitalWrite(relayPin, LOW);   // Turn heater OFF
    Serial.println("Cooking done! Heater OFF");
  }

  delay(1000);
}
