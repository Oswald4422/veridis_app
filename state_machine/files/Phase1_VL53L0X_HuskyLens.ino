/*
 * ============================================================
 *  VERIDIS — Phase 1: VL53L0X + HuskyLens (I2C pair test)
 * ============================================================
 *  PURPOSE:
 *    Read distance from the VL53L0X and object ID from the
 *    HuskyLens in the same loop to confirm both I2C devices
 *    coexist without bus contention or timeouts.
 *
 *  LIBRARIES REQUIRED (install via Library Manager):
 *    - Adafruit_VL53L0X   (by Adafruit)
 *    - HUSKYLENS          (by DFRobot — or use the HuskyLens
 *                          Arduino library from GitHub)
 *
 *  WIRING:
 *    VL53L0X  VIN → 5V    GND → GND    SDA → 21   SCL → 22
 *    HuskyLens  T → 21    R → 22       + → 5V     − → GND
 *       (set HuskyLens protocol to I2C in its own menu:
 *        General Settings → Protocol → I2C)
 *
 *  WHAT TO LOOK FOR:
 *    - Distance readings update every loop (~200ms).
 *    - When you hold a trained object in front of the
 *      HuskyLens, the ID and position appear in Serial.
 *    - Neither sensor causes the other to freeze or return
 *      error values after 10+ minutes of running.
 * ============================================================
 */

#include <Wire.h>
#include <Adafruit_VL53L0X.h>
#include "HUSKYLENS.h"

// ---------- objects ----------
Adafruit_VL53L0X tof = Adafruit_VL53L0X();
HUSKYLENS        huskylens;

// ---------- timing ----------
unsigned long lastPrint = 0;
const unsigned long PRINT_INTERVAL = 500;  // ms

// ---------- counters for reliability tracking ----------
unsigned long loopCount    = 0;
unsigned long tofErrors    = 0;
unsigned long huskyErrors  = 0;

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }
  Wire.begin(21, 22);

  Serial.println("\n=== Phase 1: VL53L0X + HuskyLens ===\n");

  // --- init VL53L0X ---
  if (!tof.begin()) {
    Serial.println("[FAIL] VL53L0X not found at 0x29");
    Serial.println("       Check wiring and rerun Phase 0 scanner.");
    while (1) { delay(100); }
  }
  Serial.println("[OK]   VL53L0X initialised");

  // --- init HuskyLens ---
  if (!huskylens.begin(Wire)) {
    Serial.println("[FAIL] HuskyLens not found at 0x32");
    Serial.println("       Make sure protocol is set to I2C on the device.");
    while (1) { delay(100); }
  }
  Serial.println("[OK]   HuskyLens initialised");

  Serial.println("\nReading sensors — hold an object in front of HuskyLens...\n");
  Serial.println("Loop# | ToF(mm)  | Husky ID | Husky X,Y  | Errors(T/H)");
  Serial.println("------|----------|----------|------------|------------");
}

// ---------- loop ----------
void loop() {
  loopCount++;

  // --- VL53L0X read ---
  VL53L0X_RangingMeasurementData_t measure;
  tof.rangingTest(&measure, false);

  int distanceMM = -1;
  if (measure.RangeStatus != 4) {   // 4 = phase failure / out of range
    distanceMM = measure.RangeMilliMeter;
  } else {
    tofErrors++;
  }

  // --- HuskyLens read ---
  int huskyID = -1;
  int huskyX  = -1;
  int huskyY  = -1;

  if (huskylens.request()) {
    if (huskylens.isLearned()) {
      if (huskylens.available()) {
        HUSKYLENSResult result = huskylens.read();
        huskyID = result.ID;
        huskyX  = result.xCenter;
        huskyY  = result.yCenter;
      }
    }
  } else {
    huskyErrors++;
  }

  // --- print at interval ---
  if (millis() - lastPrint >= PRINT_INTERVAL) {
    lastPrint = millis();

    // loop count
    char buf[128];
    snprintf(buf, sizeof(buf),
      "%5lu | %6s | %6s | %10s | T:%lu H:%lu",
      loopCount,
      (distanceMM >= 0) ? String(distanceMM).c_str() : "OOR",
      (huskyID >= 0)     ? String(huskyID).c_str()    : "none",
      (huskyID >= 0)     ? (String(huskyX) + "," + String(huskyY)).c_str() : "—",
      tofErrors,
      huskyErrors
    );
    Serial.println(buf);
  }

  delay(100);  // keep loop ~10 Hz
}
