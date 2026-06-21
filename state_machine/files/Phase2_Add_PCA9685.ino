/*
 * ============================================================
 *  VERIDIS — Phase 2: VL53L0X + HuskyLens + PCA9685
 * ============================================================
 *  PURPOSE:
 *    All three I2C devices on the same bus. Read both sensors
 *    while simultaneously sweeping a servo on PCA9685 channel 0.
 *    Confirms that servo PWM traffic doesn't disrupt sensor reads.
 *
 *  LIBRARIES REQUIRED:
 *    - Adafruit_VL53L0X
 *    - HUSKYLENS
 *    - Adafruit_PWMServoDriver  (Adafruit PWM Servo Driver Library)
 *
 *  WIRING (additions to Phase 1):
 *    PCA9685  VCC → 5V   GND → GND   SDA → 21   SCL → 22
 *    PCA9685  V+  → 6V external supply  (servo power)
 *    Servo signal → PCA9685 channel 0
 *    Servo power  → fed from PCA9685 V+ terminal
 *
 *  ⚠  Do NOT power servos from the ESP32 — use the external
 *     6V supply through the PCA9685 V+ screw terminal.
 *
 *  WHAT TO LOOK FOR:
 *    - Servo sweeps smoothly from 0° to 180° and back.
 *    - ToF and HuskyLens readings remain stable during sweep.
 *    - No I2C errors or resets appear in serial output.
 *    - Run for 10+ minutes to catch intermittent issues.
 * ============================================================
 */

#include <Wire.h>
#include <Adafruit_VL53L0X.h>
#include <Adafruit_PWMServoDriver.h>
#include "HUSKYLENS.h"

// ---------- objects ----------
Adafruit_VL53L0X        tof = Adafruit_VL53L0X();
HUSKYLENS                huskylens;
Adafruit_PWMServoDriver  pwm = Adafruit_PWMServoDriver(0x40);

// ---------- servo tuning ----------
// Typical SG90 pulse range — adjust for your servos
const uint16_t SERVO_MIN  = 150;   // ~0°   (~0.6 ms pulse)
const uint16_t SERVO_MAX  = 600;   // ~180° (~2.4 ms pulse)
const uint8_t  SERVO_CH   = 0;     // PCA9685 channel

// ---------- sweep state ----------
int     sweepPulse = SERVO_MIN;
int     sweepDir   = +4;           // increment per step
unsigned long lastSweep = 0;
const unsigned long SWEEP_INTERVAL = 20;  // ms between steps

// ---------- print timing ----------
unsigned long lastPrint = 0;
const unsigned long PRINT_INTERVAL = 500;

// ---------- error counters ----------
unsigned long loopCount   = 0;
unsigned long tofErrors   = 0;
unsigned long huskyErrors = 0;
unsigned long servoErrors = 0;

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }
  Wire.begin(21, 22);

  Serial.println("\n=== Phase 2: VL53L0X + HuskyLens + PCA9685 ===\n");

  // --- VL53L0X ---
  if (!tof.begin()) {
    Serial.println("[FAIL] VL53L0X not found");
    while (1) delay(100);
  }
  Serial.println("[OK]   VL53L0X");

  // --- HuskyLens ---
  if (!huskylens.begin(Wire)) {
    Serial.println("[FAIL] HuskyLens not found");
    while (1) delay(100);
  }
  Serial.println("[OK]   HuskyLens");

  // --- PCA9685 ---
  pwm.begin();
  pwm.setOscillatorFrequency(27000000);
  pwm.setPWMFreq(50);  // 50 Hz for standard servos
  delay(10);

  // quick test: move servo to centre
  uint16_t centre = (SERVO_MIN + SERVO_MAX) / 2;
  pwm.setPWM(SERVO_CH, 0, centre);
  Serial.println("[OK]   PCA9685 — servo centred");

  Serial.println("\nSweeping servo while reading sensors...\n");
  Serial.println("Loop# | ToF(mm) | Husky ID | Servo pulse | Errors(T/H/S)");
  Serial.println("------|---------|----------|-------------|---------------");
}

// ---------- loop ----------
void loop() {
  loopCount++;

  // --- servo sweep (non-blocking) ---
  if (millis() - lastSweep >= SWEEP_INTERVAL) {
    lastSweep = millis();
    sweepPulse += sweepDir;
    if (sweepPulse >= SERVO_MAX || sweepPulse <= SERVO_MIN) {
      sweepDir = -sweepDir;
      sweepPulse += sweepDir;  // stay in bounds
    }
    pwm.setPWM(SERVO_CH, 0, sweepPulse);
  }

  // --- VL53L0X ---
  VL53L0X_RangingMeasurementData_t measure;
  tof.rangingTest(&measure, false);
  int distMM = -1;
  if (measure.RangeStatus != 4) {
    distMM = measure.RangeMilliMeter;
  } else {
    tofErrors++;
  }

  // --- HuskyLens ---
  int hID = -1;
  if (huskylens.request()) {
    if (huskylens.isLearned() && huskylens.available()) {
      HUSKYLENSResult r = huskylens.read();
      hID = r.ID;
    }
  } else {
    huskyErrors++;
  }

  // --- print ---
  if (millis() - lastPrint >= PRINT_INTERVAL) {
    lastPrint = millis();
    char buf[128];
    snprintf(buf, sizeof(buf),
      "%5lu | %5s   | %6s   | %5d       | T:%lu H:%lu S:%lu",
      loopCount,
      (distMM >= 0)  ? String(distMM).c_str() : "OOR",
      (hID >= 0)     ? String(hID).c_str()     : "none",
      sweepPulse,
      tofErrors, huskyErrors, servoErrors
    );
    Serial.println(buf);
  }

  delay(50);
}
