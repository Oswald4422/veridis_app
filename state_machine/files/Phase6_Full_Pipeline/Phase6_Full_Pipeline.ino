/*
 * ============================================================
 *  VERIDIS — Phase 6: Full Classification Pipeline
 * ============================================================
 *  PURPOSE:
 *    Tie everything into the actual VERIDIS workflow:
 *      1. IDLE   — wait for IR beam break (item inserted)
 *      2. WEIGH  — read load cell, reject if below threshold
 *      3. SCAN   — capture HuskyLens ID, ToF signal, LDR value
 *      4. CLASSIFY — fuse sensor data into a material decision
 *      5. SORT   — rotate servo gate to correct bin
 *      6. DISPLAY — show result on TFT
 *      7. Return to IDLE
 *
 *    This is NOT the final production firmware — it's a test
 *    harness that proves the full pipeline works end-to-end.
 *    Networking, reward logic, and QR auth come later.
 *
 *  PIN SUMMARY (final assignments):
 *    GPIO 4  — LDR (analog)
 *    GPIO 5  — TFT CS
 *    GPIO 15 — IR break-beam
 *    GPIO 16 — HX711 DT
 *    GPIO 17 — TFT DC
 *    GPIO 18 — TFT CLK (SPI)
 *    GPIO 19 — TFT MISO
 *    GPIO 2  — TFT RST (moved from 16)
 *    GPIO 21 — I2C SDA
 *    GPIO 22 — I2C SCL
 *    GPIO 23 — TFT MOSI
 *    GPIO 25 — HX711 SCK
 *    GPIO 27 — Proximity sensor (moved from 18)
 *
 *  SERVO CHANNELS (PCA9685):
 *    CH 0 — sorting gate (0° = plastic bin, 90° = reject, 180° = glass bin)
 *
 *  CLASSIFICATION LOGIC (simplified weighted vote):
 *    HuskyLens ID → primary classifier
 *    LDR value    → high = transparent (glass), low = opaque (plastic)
 *    ToF signal   → material reflectance signature
 *    Weight       → secondary validation (within expected range?)
 *
 *    This is intentionally simple. Tune thresholds with real
 *    data from your own bottles during testing.
 * ============================================================
 */

#include <Wire.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Adafruit_VL53L0X.h>
#include <Adafruit_PWMServoDriver.h>
#include <HX711.h>
#include "HUSKYLENS.h"

// ============================================================
//  PIN DEFINITIONS
// ============================================================
const uint8_t PIN_IR_BEAM   = 15;
const uint8_t PIN_LDR       =  4;
const uint8_t PIN_PROXIMITY = 27;
const uint8_t HX711_DT      = 16;
const uint8_t HX711_SCK     = 25;

// ============================================================
//  OBJECTS
// ============================================================
TFT_eSPI                 tft = TFT_eSPI();
Adafruit_VL53L0X         tof = Adafruit_VL53L0X();
HUSKYLENS                huskylens;
Adafruit_PWMServoDriver  pwm = Adafruit_PWMServoDriver(0x40);
HX711                    scale;

// ============================================================
//  SERVO POSITIONS  (tune for your mechanical design)
// ============================================================
const uint16_t SERVO_MIN     = 150;
const uint16_t SERVO_MAX     = 600;
const uint16_t SERVO_PLASTIC = 150;   // ~0°   → plastic bin
const uint16_t SERVO_REJECT  = 375;   // ~90°  → reject chute
const uint16_t SERVO_GLASS   = 600;   // ~180° → glass bin

// ============================================================
//  CLASSIFICATION THRESHOLDS  (★ TUNE WITH REAL DATA ★)
// ============================================================

// HuskyLens trained object IDs — set these to match your training
const int HUSKY_ID_PLASTIC = 1;
const int HUSKY_ID_GLASS   = 2;

// LDR threshold: above = transparent/glass, below = opaque/plastic
const uint16_t LDR_TRANSPARENCY_THRESHOLD = 2000;

// ToF reflectance: glass tends to give weaker return (longer range reading
// in close quarters) — this is highly dependent on chamber geometry
const int TOF_GLASS_MIN = 20;    // mm — expected range when glass is close
const int TOF_GLASS_MAX = 80;
const int TOF_PLASTIC_MIN = 10;
const int TOF_PLASTIC_MAX = 60;

// Weight bounds (grams) — reject anything outside these
const float WEIGHT_MIN     = 5.0;    // lighter = probably not a real bottle
const float WEIGHT_MAX     = 800.0;  // heavier = filled with liquid or debris
const float WEIGHT_GLASS_MIN = 100.0; // glass bottles are typically heavier

// HX711 calibration factor (from Phase 5a)
const float CALIBRATION_FACTOR = 1.0;  // ★ REPLACE WITH YOUR VALUE ★

// ============================================================
//  STATE MACHINE
// ============================================================
enum State {
  STATE_IDLE,
  STATE_WEIGH,
  STATE_SCAN,
  STATE_CLASSIFY,
  STATE_SORT,
  STATE_DISPLAY_RESULT,
  STATE_COOLDOWN
};

enum Material {
  MAT_UNKNOWN,
  MAT_PLASTIC,
  MAT_GLASS,
  MAT_REJECT
};

State    currentState = STATE_IDLE;
Material classifiedAs = MAT_UNKNOWN;

// ---------- sensor readings for current item ----------
float    itemWeight   = 0.0;
int      itemHuskyID  = -1;
uint16_t itemLDR      = 0;
int      itemToF      = -1;

// ---------- session stats ----------
unsigned long totalItems    = 0;
unsigned long plasticCount  = 0;
unsigned long glassCount    = 0;
unsigned long rejectCount   = 0;
float         totalWeightG  = 0.0;

// ---------- timing ----------
unsigned long stateEnterTime = 0;
const unsigned long WEIGH_SETTLE_MS    = 1000;  // let load cell settle
const unsigned long SCAN_DURATION_MS   = 1500;  // time to collect sensor data
const unsigned long SORT_DURATION_MS   = 1500;  // time for servo to move
const unsigned long DISPLAY_DURATION_MS = 2000; // show result before reset
const unsigned long COOLDOWN_MS        = 500;   // debounce before next item

// ============================================================
//  HELPERS
// ============================================================
const char* materialName(Material m) {
  switch (m) {
    case MAT_PLASTIC: return "PLASTIC";
    case MAT_GLASS:   return "GLASS";
    case MAT_REJECT:  return "REJECT";
    default:          return "???";
  }
}

uint16_t materialColour(Material m) {
  switch (m) {
    case MAT_PLASTIC: return TFT_BLUE;
    case MAT_GLASS:   return TFT_GREEN;
    case MAT_REJECT:  return TFT_RED;
    default:          return TFT_WHITE;
  }
}

void moveGate(uint16_t pulse) {
  pwm.setPWM(0, 0, pulse);
}

void enterState(State s) {
  currentState   = s;
  stateEnterTime = millis();
}

unsigned long stateElapsed() {
  return millis() - stateEnterTime;
}

// ============================================================
//  CLASSIFICATION  (weighted vote)
// ============================================================
Material classifyItem() {
  // --- reject invalid weight first ---
  if (itemWeight < WEIGHT_MIN || itemWeight > WEIGHT_MAX) {
    Serial.println("  → REJECT: weight out of bounds");
    return MAT_REJECT;
  }

  // --- scoring ---
  int plasticScore = 0;
  int glassScore   = 0;

  // HuskyLens (strongest signal — worth 3 points)
  if (itemHuskyID == HUSKY_ID_PLASTIC) plasticScore += 3;
  else if (itemHuskyID == HUSKY_ID_GLASS) glassScore += 3;
  // if unknown ID, no points — rely on other sensors

  // LDR transparency (worth 2 points)
  if (itemLDR >= LDR_TRANSPARENCY_THRESHOLD) {
    glassScore += 2;   // transparent → likely glass
  } else {
    plasticScore += 2;  // opaque → likely plastic
  }

  // ToF reflectance (worth 1 point — least reliable alone)
  if (itemToF >= TOF_GLASS_MIN && itemToF <= TOF_GLASS_MAX) {
    glassScore += 1;
  }
  if (itemToF >= TOF_PLASTIC_MIN && itemToF <= TOF_PLASTIC_MAX) {
    plasticScore += 1;
  }

  // Weight as tiebreaker — glass bottles are heavier
  if (itemWeight >= WEIGHT_GLASS_MIN) {
    glassScore += 1;
  } else {
    plasticScore += 1;
  }

  Serial.printf("  Scores → plastic:%d  glass:%d\n", plasticScore, glassScore);

  // --- decision ---
  if (plasticScore > glassScore)      return MAT_PLASTIC;
  else if (glassScore > plasticScore) return MAT_GLASS;
  else {
    // tie — not confident enough, reject for safety
    Serial.println("  → REJECT: tie score, not confident");
    return MAT_REJECT;
  }
}

// ============================================================
//  TFT SCREENS
// ============================================================
void showIdleScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(30, 20);
  tft.println("VERIDIS");
  tft.setTextSize(1);
  tft.setCursor(30, 50);
  tft.println("Insert a bottle to begin");

  tft.setCursor(10, 90);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.printf("Session: %lu items  (P:%lu G:%lu R:%lu)",
             totalItems, plasticCount, glassCount, rejectCount);
  tft.setCursor(10, 105);
  tft.printf("Total weight: %.1f g", totalWeightG);

  // bin status
  bool binFull = (digitalRead(PIN_PROXIMITY) == LOW);
  tft.setCursor(10, 125);
  if (binFull) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.println("!! BIN FULL — please empty !!");
  } else {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.println("Bin status: OK");
  }
}

void showProcessingScreen(const char* step) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(30, 40);
  tft.println("Processing...");
  tft.setTextSize(1);
  tft.setCursor(30, 70);
  tft.println(step);
}

void showResultScreen(Material mat, float weight) {
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(materialColour(mat), TFT_BLACK);
  tft.setTextSize(3);
  tft.setCursor(20, 20);
  tft.println(materialName(mat));

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(20, 60);
  tft.printf("%.1f g", weight);

  if (mat != MAT_REJECT) {
    tft.setTextSize(1);
    tft.setCursor(20, 90);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.println("Item accepted — thank you!");
  } else {
    tft.setTextSize(1);
    tft.setCursor(20, 90);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.println("Item rejected — please retrieve");
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  pinMode(PIN_IR_BEAM,   INPUT);
  pinMode(PIN_PROXIMITY, INPUT);

  Serial.println("\n=== Phase 6: Full VERIDIS Pipeline ===\n");

  // --- TFT ---
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  // --- I2C devices ---
  if (!tof.begin())              { Serial.println("VL53L0X FAIL"); while(1) delay(100); }
  if (!huskylens.begin(Wire))    { Serial.println("HuskyLens FAIL"); while(1) delay(100); }
  pwm.begin();
  pwm.setOscillatorFrequency(27000000);
  pwm.setPWMFreq(50);

  // --- HX711 ---
  scale.begin(HX711_DT, HX711_SCK);
  delay(500);
  if (scale.is_ready()) {
    scale.set_scale(CALIBRATION_FACTOR);
    scale.tare();
    Serial.println("[OK]   HX711 tared");
  } else {
    Serial.println("[WARN] HX711 not ready");
  }

  // --- start in idle, gate to reject position ---
  moveGate(SERVO_REJECT);
  enterState(STATE_IDLE);
  showIdleScreen();
  Serial.println("[OK]   System ready — insert a bottle\n");
}

// ============================================================
//  MAIN LOOP — STATE MACHINE
// ============================================================
void loop() {

  switch (currentState) {

    // -------------------------------------------------------
    case STATE_IDLE: {
      // wait for IR beam to break
      if (digitalRead(PIN_IR_BEAM) == LOW) {
        Serial.println("\n>> Item detected!");
        showProcessingScreen("Weighing...");
        enterState(STATE_WEIGH);
      }
      break;
    }

    // -------------------------------------------------------
    case STATE_WEIGH: {
      // let load cell settle, then read
      if (stateElapsed() >= WEIGH_SETTLE_MS) {
        if (scale.is_ready()) {
          itemWeight = scale.get_units(5);  // average of 5
        } else {
          itemWeight = -1;
        }
        Serial.printf("  Weight: %.1f g\n", itemWeight);
        showProcessingScreen("Scanning...");
        enterState(STATE_SCAN);
      }
      break;
    }

    // -------------------------------------------------------
    case STATE_SCAN: {
      // collect sensor data over SCAN_DURATION_MS for stability
      // take readings multiple times and use last stable value

      // HuskyLens
      if (huskylens.request()) {
        if (huskylens.isLearned() && huskylens.available()) {
          itemHuskyID = huskylens.read().ID;
        }
      }

      // LDR (take most recent)
      itemLDR = analogRead(PIN_LDR);

      // VL53L0X
      VL53L0X_RangingMeasurementData_t m;
      tof.rangingTest(&m, false);
      if (m.RangeStatus != 4) {
        itemToF = m.RangeMilliMeter;
      }

      if (stateElapsed() >= SCAN_DURATION_MS) {
        Serial.printf("  Husky ID: %d  LDR: %d  ToF: %d mm\n",
                      itemHuskyID, itemLDR, itemToF);
        showProcessingScreen("Classifying...");
        enterState(STATE_CLASSIFY);
      }
      break;
    }

    // -------------------------------------------------------
    case STATE_CLASSIFY: {
      classifiedAs = classifyItem();
      Serial.printf("  >> CLASSIFIED: %s\n", materialName(classifiedAs));
      enterState(STATE_SORT);
      break;
    }

    // -------------------------------------------------------
    case STATE_SORT: {
      // move gate on entry
      if (stateElapsed() < 50) {
        switch (classifiedAs) {
          case MAT_PLASTIC: moveGate(SERVO_PLASTIC); break;
          case MAT_GLASS:   moveGate(SERVO_GLASS);   break;
          default:          moveGate(SERVO_REJECT);   break;
        }
      }

      // wait for item to fall through
      if (stateElapsed() >= SORT_DURATION_MS) {
        // return gate to neutral
        moveGate(SERVO_REJECT);

        // update stats
        totalItems++;
        if (classifiedAs == MAT_PLASTIC) { plasticCount++; totalWeightG += itemWeight; }
        if (classifiedAs == MAT_GLASS)   { glassCount++;   totalWeightG += itemWeight; }
        if (classifiedAs == MAT_REJECT)  { rejectCount++; }

        enterState(STATE_DISPLAY_RESULT);
      }
      break;
    }

    // -------------------------------------------------------
    case STATE_DISPLAY_RESULT: {
      if (stateElapsed() < 50) {
        showResultScreen(classifiedAs, itemWeight);
        Serial.printf("  Stats → P:%lu G:%lu R:%lu  TotalWt:%.1f g\n",
                      plasticCount, glassCount, rejectCount, totalWeightG);
      }

      if (stateElapsed() >= DISPLAY_DURATION_MS) {
        enterState(STATE_COOLDOWN);
      }
      break;
    }

    // -------------------------------------------------------
    case STATE_COOLDOWN: {
      // reset per-item readings
      itemWeight  = 0;
      itemHuskyID = -1;
      itemLDR     = 0;
      itemToF     = -1;
      classifiedAs = MAT_UNKNOWN;

      if (stateElapsed() >= COOLDOWN_MS) {
        showIdleScreen();
        enterState(STATE_IDLE);
      }
      break;
    }
  }

  delay(10);
}
