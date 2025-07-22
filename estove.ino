#include "max6675.h"

// Define GPIO pins
int thermoCLK = 18;   // SCK
int thermoCS  = 5;    // CS
int thermoDO  = 19;   // SO

// Create thermocouple instance
MAX6675 thermocouple(thermoCLK, thermoCS, thermoDO);

void setup() {
  Serial.begin(115200);
  delay(500);  // wait for MAX6675 to stabilize
  Serial.println("Thermocouple ready!");
}

void loop() {
  // Read temperature in Celsius
  float temperatureC = thermocouple.readCelsius();
  
  // Read temperature in Fahrenheit (optional)
  float temperatureF = thermocouple.readFahrenheit();

  Serial.print("Temperature: ");
  Serial.print(temperatureC);
  Serial.print(" °C  |  ");
  Serial.print(temperatureF);
  Serial.println(" °F");

  delay(1000); // 1 second delay
}
