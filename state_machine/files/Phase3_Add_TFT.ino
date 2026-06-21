/*
 * ============================================================
 *  VERIDIS — Phase 3: I2C devices + TFT Display (SPI)
 * ============================================================
 *  PURPOSE:
 *    Add the TFT display over SPI while all three I2C devices
 *    are active. Sensor readings are rendered on-screen instead
 *    of (in addition to) Serial. This validates:
 *      - SPI and I2C buses operating simultaneously.
 *      - Pin conflict resolution for GPIO 16 & 18.
 *
 *  LIBRARIES REQUIRED:
 *    - Adafruit_VL53L0X
 *    - HUSKYLENS
 *    - Adafruit_PWMServoDriver
 *    - TFT_eSPI   (configure User_Setup.h — see notes below)
 *
 *  ⚠  PIN CONFLICT RESOLUTION:
 *    Your original mapping had GPIO 16 shared between HX711 DT
 *    and TFT RST, and GPIO 18 shared between proximity sensor
 *    and SPI CLK. This sketch applies the recommended fixes:
 *
 *    TFT RST  → GPIO 2   (moved off 16, freeing it for HX711)
 *    Prox sensor will be tested in Phase 4 on GPIO 27
 *    (freeing GPIO 18 exclusively for SPI CLK)
 *
 *  TFT_eSPI User_Setup.h configuration:
 *    #define ILI9341_DRIVER        // or ST7789_DRIVER
 *    #define TFT_CS    5
 *    #define TFT_DC   17
 *    #define TFT_RST   2          // ← changed from 16
 *    #define TFT_MOSI 23
 *    #define TFT_SCLK 18
 *    #define TFT_MISO 19
 *
 *  WIRING:
 *    TFT CS   → GPIO 5
 *    TFT DC   → GPIO 17
 *    TFT RST  → GPIO 2   (CHANGED)
 *    TFT CLK  → GPIO 18
 *    TFT MOSI → GPIO 23
 *    TFT MISO → GPIO 19
 *    TFT VCC  → 5V
 *    TFT LED  → 3.3V
 *    TFT GND  → GND
 *
 *  WHAT TO LOOK FOR:
 *    - TFT shows live sensor values and servo angle.
 *    - No screen glitches when I2C traffic is heavy.
 *    - Servo sweep stays smooth (SPI DMA shouldn't block I2C).
 *    - Run for 10+ minutes.
 * ============================================================
 */

#include <Wire.h>
#include <SPI.h>
#include <TFT_eSPI.h>              // configured via User_Setup.h
#include <Adafruit_VL53L0X.h>
#include <Adafruit_PWMServoDriver.h>
#include "HUSKYLENS.h"

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

// ---------- display refresh ----------
unsigned long lastDraw = 0;
const unsigned long DRAW_INTERVAL = 300;  // ms — don't redraw too fast

// ---------- state ----------
unsigned long loopCount   = 0;
unsigned long tofErrors   = 0;
unsigned long huskyErrors = 0;
int lastDist  = -1;
int lastHID   = -1;

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  Serial.println("\n=== Phase 3: I2C + TFT (SPI) ===\n");

  // --- TFT ---
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("VERIDIS Phase 3");
  tft.setTextSize(1);
  tft.setCursor(10, 40);
  tft.println("Initialising sensors...");
  Serial.println("[OK]   TFT display");

  // --- VL53L0X ---
  if (!tof.begin()) {
    tft.setTextColor(TFT_RED);
    tft.println("VL53L0X FAIL");
    Serial.println("[FAIL] VL53L0X");
    while (1) delay(100);
  }
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.println("VL53L0X OK");
  Serial.println("[OK]   VL53L0X");

  // --- HuskyLens ---
  if (!huskylens.begin(Wire)) {
    tft.setTextColor(TFT_RED);
    tft.println("HuskyLens FAIL");
    Serial.println("[FAIL] HuskyLens");
    while (1) delay(100);
  }
  tft.println("HuskyLens OK");
  Serial.println("[OK]   HuskyLens");

  // --- PCA9685 ---
  pwm.begin();
  pwm.setOscillatorFrequency(27000000);
  pwm.setPWMFreq(50);
  pwm.setPWM(0, 0, (SERVO_MIN + SERVO_MAX) / 2);
  tft.println("PCA9685 OK");
  Serial.println("[OK]   PCA9685");

  delay(1000);
  tft.fillScreen(TFT_BLACK);
}

// ---------- helpers ----------
// Map servo pulse to approximate angle for display
int pulseToAngle(int pulse) {
  return map(pulse, SERVO_MIN, SERVO_MAX, 0, 180);
}

void drawDashboard(int dist, int hID, int angle) {
  // header
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 5);
  tft.print("VERIDIS  ");

  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setCursor(200, 12);
  tft.printf("loop %lu  ", loopCount);

  // divider
  tft.drawFastHLine(0, 28, tft.width(), TFT_DARKGREY);

  // ToF distance
  tft.setTextSize(2);
  tft.setCursor(10, 40);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.print("ToF: ");
  if (dist >= 0) {
    tft.printf("%4d mm   ", dist);
  } else {
    tft.print(" OOR      ");
  }

  // HuskyLens
  tft.setCursor(10, 70);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.print("Cam: ");
  if (hID >= 0) {
    tft.printf("ID %d      ", hID);
  } else {
    tft.print("none      ");
  }

  // Servo angle
  tft.setCursor(10, 100);
  tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
  tft.printf("Servo: %3d deg   ", angle);

  // error counters
  tft.setTextSize(1);
  tft.setCursor(10, 130);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.printf("Errors  ToF:%lu  Husky:%lu    ", tofErrors, huskyErrors);
}

// ---------- loop ----------
void loop() {
  loopCount++;

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
  lastDist = (m.RangeStatus != 4) ? m.RangeMilliMeter : -1;
  if (lastDist < 0) tofErrors++;

  // --- HuskyLens ---
  lastHID = -1;
  if (huskylens.request()) {
    if (huskylens.isLearned() && huskylens.available()) {
      lastHID = huskylens.read().ID;
    }
  } else {
    huskyErrors++;
  }

  // --- update display ---
  if (millis() - lastDraw >= DRAW_INTERVAL) {
    lastDraw = millis();
    drawDashboard(lastDist, lastHID, pulseToAngle(sweepPulse));

    // also mirror to serial
    Serial.printf("[%lu] ToF:%d  Husky:%d  Servo:%d°\n",
                  loopCount, lastDist, lastHID, pulseToAngle(sweepPulse));
  }

  delay(30);
}
