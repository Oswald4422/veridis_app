/*
 * ============================================================
 *  VERIDIS — Phase 4: Add GPIO Sensors (IR, LDR, Proximity)
 * ============================================================
 *  PURPOSE:
 *    Layer the three simple GPIO sensors onto the already-working
 *    I2C + SPI stack. These are low-risk additions but confirm
 *    that main-loop timing remains stable with extra reads.
 *
 *  LIBRARIES REQUIRED:
 *    Same as Phase 3 (Adafruit_VL53L0X, HUSKYLENS,
 *    Adafruit_PWMServoDriver, TFT_eSPI)
 *
 *  WIRING (additions / changes):
 *    IR break-beam   OUT → GPIO 15   VCC → 5V   GND → GND
 *    LDR module       AO → GPIO 4    VCC → 5V   GND → GND
 *    Proximity sensor OUT → GPIO 27  VCC → 5V   GND → GND
 *       ↑ moved from GPIO 18 to avoid SPI CLK conflict
 *
 *  WHAT TO LOOK FOR:
 *    - IR beam reads HIGH normally, drops LOW when broken
 *      (or inverted depending on your module — note which).
 *    - LDR analog value rises when light hits the sensor,
 *      drops when an opaque object blocks the path.
 *    - Proximity reads LOW/HIGH based on bin fill level.
 *    - All I2C and TFT functions continue working.
 *    - Test the IR break-beam rapidly (simulating fast bottle
 *      insertion) to check for missed triggers.
 * ============================================================
 */

#include <Wire.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Adafruit_VL53L0X.h>
#include <Adafruit_PWMServoDriver.h>
#include "HUSKYLENS.h"

// ---------- pin definitions ----------
const uint8_t PIN_IR_BEAM   = 15;    // digital — item detection
const uint8_t PIN_LDR       =  4;    // analog  — transparency
const uint8_t PIN_PROXIMITY = 27;    // digital — bin fill level

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
unsigned long lastDraw  = 0;
unsigned long loopCount = 0;

// ---------- GPIO trigger tracking ----------
volatile unsigned long irTriggerCount = 0;
bool     lastIRState   = HIGH;
uint16_t ldrValue      = 0;
bool     proxTriggered = false;

// ---------- I2C data ----------
int tofDist = -1;
int huskyID = -1;
unsigned long tofErrors   = 0;
unsigned long huskyErrors = 0;

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  // GPIO pins
  pinMode(PIN_IR_BEAM,   INPUT);
  pinMode(PIN_PROXIMITY, INPUT);
  // GPIO 4 is analog — no pinMode needed on ESP32

  Serial.println("\n=== Phase 4: Full sensor stack ===\n");

  // --- TFT ---
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("VERIDIS Phase 4");
  tft.setTextSize(1);
  tft.setCursor(10, 40);

  // --- VL53L0X ---
  if (!tof.begin()) {
    tft.println("VL53L0X FAIL"); while (1) delay(100);
  }
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.println("VL53L0X OK");

  // --- HuskyLens ---
  if (!huskylens.begin(Wire)) {
    tft.println("HuskyLens FAIL"); while (1) delay(100);
  }
  tft.println("HuskyLens OK");

  // --- PCA9685 ---
  pwm.begin();
  pwm.setOscillatorFrequency(27000000);
  pwm.setPWMFreq(50);
  pwm.setPWM(0, 0, (SERVO_MIN + SERVO_MAX) / 2);
  tft.println("PCA9685 OK");

  tft.println("GPIO sensors OK");
  Serial.println("[OK]   All components initialised\n");
  delay(1000);
  tft.fillScreen(TFT_BLACK);
}

// ---------- dashboard ----------
void drawDashboard() {
  int y = 5;
  tft.setTextSize(2);

  // header
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(10, y);
  tft.print("VERIDIS  ");
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.printf("loop %lu    ", loopCount);
  y += 25;
  tft.drawFastHLine(0, y, tft.width(), TFT_DARKGREY);
  y += 5;

  tft.setTextSize(2);

  // IR break-beam
  tft.setCursor(10, y);
  bool beamBroken = (digitalRead(PIN_IR_BEAM) == LOW);
  if (beamBroken) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.print("IR:  BLOCKED   ");
  } else {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.print("IR:  clear     ");
  }
  y += 22;

  // LDR
  tft.setCursor(10, y);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.printf("LDR: %4d      ", ldrValue);
  // simple bar graph
  uint16_t barW = map(ldrValue, 0, 4095, 0, 100);
  tft.fillRect(220, y + 2, 100, 14, TFT_BLACK);
  tft.fillRect(220, y + 2, barW, 14, TFT_YELLOW);
  y += 22;

  // Proximity (bin level)
  tft.setCursor(10, y);
  if (proxTriggered) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.print("Bin: FULL      ");
  } else {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.print("Bin: OK        ");
  }
  y += 22;

  // ToF distance
  tft.setCursor(10, y);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  if (tofDist >= 0) {
    tft.printf("ToF: %4d mm   ", tofDist);
  } else {
    tft.print("ToF: OOR       ");
  }
  y += 22;

  // HuskyLens
  tft.setCursor(10, y);
  tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
  if (huskyID >= 0) {
    tft.printf("Cam: ID %d      ", huskyID);
  } else {
    tft.print("Cam: none      ");
  }
  y += 22;

  // Servo
  tft.setCursor(10, y);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.printf("Servo: %3d deg ", map(sweepPulse, SERVO_MIN, SERVO_MAX, 0, 180));
  y += 25;

  // IR trigger count
  tft.setTextSize(1);
  tft.setCursor(10, y);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.printf("IR triggers: %lu   Errors T:%lu H:%lu    ",
             irTriggerCount, tofErrors, huskyErrors);
}

// ---------- loop ----------
void loop() {
  loopCount++;

  // --- GPIO reads ---
  bool irState = digitalRead(PIN_IR_BEAM);
  // detect falling edge (beam just broken)
  if (lastIRState == HIGH && irState == LOW) {
    irTriggerCount++;
    Serial.printf("[TRIGGER] IR beam broken — count: %lu\n", irTriggerCount);
  }
  lastIRState = irState;

  ldrValue      = analogRead(PIN_LDR);
  proxTriggered = (digitalRead(PIN_PROXIMITY) == LOW);  // active-low typical

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
    if (huskylens.isLearned() && huskylens.available()) {
      huskyID = huskylens.read().ID;
    }
  } else {
    huskyErrors++;
  }

  // --- TFT update ---
  if (millis() - lastDraw >= 300) {
    lastDraw = millis();
    drawDashboard();
  }

  delay(20);
}
