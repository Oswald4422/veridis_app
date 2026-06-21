#include "Adafruit_VL53L0X.h"

// Create an instance of the sensor
Adafruit_VL53L0X lox = Adafruit_VL53L0X();

void setup() {
  Serial.begin(9600);

  // Wait for serial to initialize
  while (!Serial) { delay(1); }

  Serial.println("Adafruit VL53L0X test");
  
  // Initialize sensor at default address 0x29
  if (!lox.begin()) {
    Serial.println(F("Failed to boot VL53L0X. Check wiring!"));
    while (1);
  }
  
  Serial.println(F("VL53L0X Ready!\n"));
}

void loop() {
  VL53L0X_RangingMeasurementData_t measure;

  Serial.print("Reading measurement... ");
  // Request data from sensor (false = no debug info)
  lox.rangingTest(&measure, false); 

  // Phase failure status 4 means object is out of range
  if (measure.RangeStatus != 4) {  
    Serial.print("Distance (mm): "); 
    Serial.println(measure.RangeMilliMeter);
  } else {
    Serial.println(" Out of range ");
  }

  delay(100);
}
