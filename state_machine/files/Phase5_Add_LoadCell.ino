/*
 * ============================================================
 *  VERIDIS — Phase 5: Load Cell + HX711 Integration
 * ============================================================
 *  PURPOSE:
 *    Add weight measurement to the full sensor stack.
 *    This sketch has TWO MODES controlled by a #define:
 *
 *      MODE_STANDALONE  — HX711 + load cell only, for initial
 *                         calibration and validation.
 *      MODE_INTEGRATED  — Full stack with HX711 added.
 *
 *    Start with MODE_STANDALONE to calibrate, then switch to
 *    MODE_INTEGRATED to confirm timing works with everything.
 *
 *  LIBRARIES REQUIRED:
 *    - HX711 (by bogde — install via Library Manager)
 *    - All previous libraries for integrated mode
 *
 *  WIRING:
 *    HX711 DT   → GPIO 16
 *    HX711 SCK  → GPIO 25
 *    HX711 VCC  → 5V
 *    HX711 GND  → GND
 *
 *    Load Cell (4-wire Wheatstone bridge):
 *      RED   → HX711 E+   (excitation +)
 *      BLACK → HX711 E−   (excitation −)
 *      GREEN → HX711 A+   (signal +)
 *      WHITE → HX711 A−   (signal −)
 *
 *    ⚠  Verify wire colours against YOUR load cell datasheet.
 *       Some manufacturers use different colour schemes.
 *
 *  CALIBRATION PROCEDURE (standalone mode):
 *    1. Upload and open serial monitor at 115200.
 *    2. With nothing on the load cell, note the "raw" reading.
 *    3. Place a known weight (e.g. 100g calibration weight).
 *    4. Note the new "raw" reading.
 *    5. calibration_factor = (raw_with_weight − raw_empty) / known_grams
 *    6. Enter this value in CALIBRATION_FACTOR below.
 *    7. Verify: place the known weight again — should read ~100g.
 * ============================================================
 */

// *** TOGGLE THIS TO SWITCH MODES ***
#define MODE_STANDALONE    // comment out for MODE_INTEGRATED
// #define MODE_INTEGRATED

#include <HX711.h>

// ---------- HX711 pins ----------
const uint8_t HX711_DT  = 16;
const uint8_t HX711_SCK = 25;

// ---------- calibration ----------
// Set this to 1.0 initially, then update after calibration
float CALIBRATION_FACTOR = 1.0;

HX711 scale;

// ================================================================
//  STANDALONE MODE — calibrate the load cell in isolation
// ================================================================
#ifdef MODE_STANDALONE

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }

  Serial.println("\n=== Phase 5a: HX711 Standalone Calibration ===\n");

  scale.begin(HX711_DT, HX711_SCK);

  Serial.println("Remove all weight from the load cell...");
  Serial.println("Taring in 3 seconds...");
  delay(3000);

  scale.set_scale();            // raw mode
  scale.tare();                 // zero the scale
  Serial.println("Tare complete.\n");

  if (CALIBRATION_FACTOR != 1.0) {
    scale.set_scale(CALIBRATION_FACTOR);
    Serial.printf("Using calibration factor: %.2f\n\n", CALIBRATION_FACTOR);
    Serial.println("Place known weight to verify...\n");
    Serial.println("  Raw       | Units (g) | Avg(10)");
    Serial.println("------------|-----------|--------");
  } else {
    Serial.println("CALIBRATION_FACTOR is 1.0 — running in raw mode.");
    Serial.println("Place a known weight and note the raw reading.\n");
    Serial.println("  Raw         | Avg(10)");
    Serial.println("--------------|--------");
  }
}

void loop() {
  if (scale.is_ready()) {
    long raw = scale.read();
    float avg = scale.get_units(10);  // average of 10 readings

    if (CALIBRATION_FACTOR != 1.0) {
      Serial.printf("  %10ld | %7.1f g | %7.1f g\n", raw, scale.get_units(1), avg);
    } else {
      Serial.printf("  %12ld | %10.1f\n", raw, avg);
    }
  } else {
    Serial.println("  HX711 not ready — check wiring");
  }

  delay(500);
}

#endif  // MODE_STANDALONE


// ================================================================
//  INTEGRATED MODE — HX711 added to full sensor stack
// ================================================================
#ifdef MODE_INTEGRATED

#include <Wire.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Adafruit_VL53L0X.h>
#include <Adafruit_PWMServoDriver.h>
#include "HUSKYLENS.h"

// ---------- GPIO pins ----------
const uint8_t PIN_IR_BEAM   = 15;
const uint8_t PIN_LDR       =  4;
const uint8_t PIN_PROXIMITY = 27;

// ---------- objects ----------
TFT_eSPI                 tft = TFT_eSPI();
Adafruit_VL53L0X         tof = Adafruit_VL53L0X();
HUSKYLENS                huskylens;
Adafruit_PWMServoDriver  pwm = Adafruit_PWMServoDriver(0x40);

// ---------- servo ----------
const uint16_t SERVO_MIN = 150;
const uint16_t SERVO_MAX = 600;
int   sweepPulse = SERVO_MIN;
int   sweepDir   = +4;
unsigned long lastSweep = 0;

// ---------- timing ----------
unsigned long lastDraw     = 0;
unsigned long lastWeigh    = 0;
unsigned long loopCount    = 0;
const unsigned long WEIGH_INTERVAL = 200;  // ms between HX711 reads

// ---------- sensor state ----------
int      tofDist      = -1;
int      huskyID      = -1;
uint16_t ldrValue     = 0;
bool     irBlocked    = false;
bool     binFull      = false;
float    weightGrams  = 0.0;
bool     hx711Ready   = false;

// ---------- error counters ----------
unsigned long tofErrors   = 0;
unsigned long huskyErrors = 0;
unsigned long hx711Errors = 0;

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  pinMode(PIN_IR_BEAM,   INPUT);
  pinMode(PIN_PROXIMITY, INPUT);

  Serial.println("\n=== Phase 5b: Full stack + HX711 ===\n");

  // --- TFT ---
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("VERIDIS Phase 5");
  tft.setTextSize(1);
  tft.setCursor(10, 40);

  // --- I2C devices ---
  if (!tof.begin()) { tft.println("VL53L0X FAIL"); while(1) delay(100); }
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.println("VL53L0X OK");

  if (!huskylens.begin(Wire)) { tft.println("HuskyLens FAIL"); while(1) delay(100); }
  tft.println("HuskyLens OK");

  pwm.begin();
  pwm.setOscillatorFrequency(27000000);
  pwm.setPWMFreq(50);
  pwm.setPWM(0, 0, (SERVO_MIN + SERVO_MAX) / 2);
  tft.println("PCA9685 OK");

  // --- HX711 ---
  scale.begin(HX711_DT, HX711_SCK);
  delay(500);
  if (scale.is_ready()) {
    scale.set_scale(CALIBRATION_FACTOR);
    scale.tare();
    hx711Ready = true;
    tft.println("HX711 OK (tared)");
    Serial.println("[OK]   HX711 tared");
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.println("HX711 NOT READY");
    Serial.println("[WARN] HX711 not ready — will retry in loop");
  }

  tft.println("GPIO sensors OK");
  delay(1000);
  tft.fillScreen(TFT_BLACK);
}

void drawFullDashboard() {
  int y = 5;
  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(10, y);
  tft.print("VERIDIS");
  y += 25;
  tft.drawFastHLine(0, y, tft.width(), TFT_DARKGREY);
  y += 5;

  // Weight — the new addition, shown prominently
  tft.setCursor(10, y);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  if (hx711Ready) {
    tft.printf("Wt: %6.1f g   ", weightGrams);
  } else {
    tft.print("Wt: -- err --  ");
  }
  y += 22;

  // IR
  tft.setCursor(10, y);
  tft.setTextColor(irBlocked ? TFT_RED : TFT_GREEN, TFT_BLACK);
  tft.printf("IR:  %s      ", irBlocked ? "BLOCKED" : "clear  ");
  y += 22;

  // LDR
  tft.setCursor(10, y);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.printf("LDR: %4d      ", ldrValue);
  y += 22;

  // ToF
  tft.setCursor(10, y);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.printf("ToF: %s      ", (tofDist >= 0) ? String(String(tofDist) + " mm").c_str() : "OOR");
  y += 22;

  // Husky
  tft.setCursor(10, y);
  tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
  tft.printf("Cam: %s        ", (huskyID >= 0) ? String(huskyID).c_str() : "none");
  y += 22;

  // Bin
  tft.setCursor(10, y);
  tft.setTextColor(binFull ? TFT_ORANGE : TFT_GREEN, TFT_BLACK);
  tft.printf("Bin: %s      ", binFull ? "FULL" : "OK  ");
}

void loop() {
  loopCount++;

  // --- GPIO ---
  irBlocked = (digitalRead(PIN_IR_BEAM) == LOW);
  ldrValue  = analogRead(PIN_LDR);
  binFull   = (digitalRead(PIN_PROXIMITY) == LOW);

  // --- HX711 (rate-limited — it's slow) ---
  if (millis() - lastWeigh >= WEIGH_INTERVAL) {
    lastWeigh = millis();
    if (scale.is_ready()) {
      weightGrams = scale.get_units(3);  // avg of 3 for speed
      hx711Ready  = true;
    } else {
      hx711Errors++;
      hx711Ready = false;
    }
  }

  // --- servo sweep ---
  if (millis() - lastSweep >= 20) {
    lastSweep = millis();
    sweepPulse += sweepDir;
    if (sweepPulse >= SERVO_MAX || sweepPulse <= SERVO_MIN)
      sweepDir = -sweepDir;
    pwm.setPWM(0, 0, sweepPulse);
  }

  // --- VL53L0X ---
  VL53L0X_RangingMeasurementData_t m;
  tof.rangingTest(&m, false);
  tofDist = (m.RangeStatus != 4) ? m.RangeMilliMeter : -1;
  if (tofDist < 0) tofErrors++;

  // --- HuskyLens ---
  huskyID = -1;
  if (huskylens.request()) {
    if (huskylens.isLearned() && huskylens.available())
      huskyID = huskylens.read().ID;
  } else {
    huskyErrors++;
  }

  // --- TFT ---
  if (millis() - lastDraw >= 300) {
    lastDraw = millis();
    drawFullDashboard();
    Serial.printf("[%lu] Wt:%.1fg IR:%d LDR:%d ToF:%d Cam:%d Bin:%d\n",
                  loopCount, weightGrams, irBlocked, ldrValue,
                  tofDist, huskyID, binFull);
  }

  delay(20);
}

#endif  // MODE_INTEGRATED
