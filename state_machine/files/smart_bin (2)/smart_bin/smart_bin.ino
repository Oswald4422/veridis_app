/*
 * ============================================================
 *  SMART WASTE SEGREGATION SYSTEM
 *  Platform : ESP32
 *  Author   : Generated for Smart Bin Project
 * ============================================================
 *  CLASSIFICATION LOGIC
 *  ─────────────────────
 *  Three sensors vote on each bottle:
 *    1. HuskyLens AI Camera (entrance) — primary
 *    2. LDR sensor (chute)             — secondary
 *    3. VL53L0X ToF sensor (chute)     — secondary
 *
 *  Each sensor votes: 1=Plastic, 2=Glass, 3=Reject
 *  Majority (2 of 3) wins. All three disagree = Reject.
 *
 *  ROUTING LOGIC
 *  ─────────────
 *  Servo ch4 & ch5 always open and close together.
 *  Divider servo (ch7) determines which bin the bottle
 *  falls into: neutral (90°) = plastic, 60° CCW = glass.
 *  Reject flap (ch6) opens outward into rejected bin.
 *
 *  CALIBRATION MODE
 *  ─────────────────
 *  Set DEBUG_CALIBRATION to true to print raw LDR + ToF
 *  readings to Serial Monitor without routing bottles.
 *  Servos will NOT move. Place bottles manually at chute
 *  sensor position to collect threshold readings.
 *  Set back to false for normal operation.
 *
 * ============================================================
 *
 *  PIN SUMMARY
 * ─────────────────────────────────────────────────────────────
 *  TFT Display (SPI)
 *    TFT_CS   → GPIO 5
 *    TFT_DC   → GPIO 17
 *    TFT_RST  → GPIO 2
 *    TFT_MOSI → GPIO 23  (shared SPI)
 *    TFT_SCK  → GPIO 18  (shared SPI)
 *    TFT_MISO → GPIO 19  (shared SPI)
 *
 *  Touch Controller XPT2046 (shared SPI)
 *    T_CS     → GPIO 35
 *    T_IRQ    → NOT CONNECTED (polling mode)
 *
 *  I2C Bus (shared: HuskyLens, ToF, Servo Driver)
 *    SDA → GPIO 21  |  SCL → GPIO 22
 *
 *  HuskyLens AI Camera  → I2C
 *    ID 1 = Plastic, ID 2 = Glass
 *    !! UPDATE HUSKY_PLASTIC_ID / HUSKY_GLASS_ID after training !!
 *
 *  VL53L0X ToF Sensor   → I2C (address 0x29)
 *  PCA9685 Servo Driver → I2C
 *    Ch 2 & 3 → Entry gate flaps (open on detection)
 *    Ch 4 & 5 → Bin flaps (always open/close together)
 *    Ch 6     → Rejection flap (opens outward)
 *    Ch 7     → Bin divider (90° neutral=plastic, 60° CCW=glass)
 *
 *  IR Sensor        → GPIO 15  (bottle presence + count trigger)
 *  LDR Sensor       → GPIO 4   (analog, light transmission in chute)
 *  Proximity 1      → GPIO 26  (plastic bin fill level)
 *  Proximity 2      → GPIO 27  (glass bin fill level)
 *
 *  RGB LED 1 & 2 (parallel, common cathode — always same colour)
 *    R → GPIO 13  |  G → GPIO 14  |  B → GPIO 33
 *    White = idle/default, Green = accepted, Red = rejected
 *
 *  Power: 24V → Buck Converter → 6V (servos), 5V (logic), 3.3V (ESP32)
 *
 * ============================================================
 *  LIBRARIES REQUIRED (install via Arduino Library Manager)
 *    - TFT_eSPI
 *    - XPT2046_Touchscreen
 *    - HUSKYLENS              (DFRobot)
 *    - Adafruit_VL53L0X
 *    - Adafruit_PWMServoDriver
 *    - QRCode                 (ricmoo/qrcode)
 * ============================================================
 *
 *  TFT_eSPI User_Setup.h — required defines:
 *    #define ILI9341_DRIVER
 *    #define TFT_CS   5
 *    #define TFT_DC   17
 *    #define TFT_RST  2
 *    #define TFT_MOSI 23
 *    #define TFT_SCLK 18
 *    #define TFT_MISO 19
 *    #define SPI_FREQUENCY  40000000
 *    #define TOUCH_CS 35
 * ============================================================
 */

// ─── Libraries ───────────────────────────────────────────────
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Wire.h>
#include "HUSKYLENS.h"
#include <Adafruit_VL53L0X.h>
#include <Adafruit_PWMServoDriver.h>
#include "qrcode.h"

// ═══════════════════════════════════════════════════════════════
//  CALIBRATION / DEBUG MODE
//  Set to true → prints raw LDR & ToF readings to Serial Monitor.
//  Servos will NOT move. Place bottles manually at chute sensors.
//  Set back to false for normal operation.
// ═══════════════════════════════════════════════════════════════
#define DEBUG_CALIBRATION   false

// ═══════════════════════════════════════════════════════════════
//  LDR THRESHOLDS  (ESP32 ADC: 0–4095, 12-bit)
//  Glass transmits more light  → higher LDR reading
//  Plastic transmits less      → lower LDR reading
//  Non-bottle (opaque)         → very low reading
//
//  How to set:
//    1. Run calibration mode
//    2. Note average reading for glass bottles   → set LDR_GLASS_MIN
//    3. Note average reading for plastic bottles → set LDR_PLASTIC_MIN
//    4. Anything below LDR_PLASTIC_MIN = Reject
// ═══════════════════════════════════════════════════════════════
#define LDR_GLASS_MIN     2800    // !! SET AFTER CALIBRATION !!
#define LDR_PLASTIC_MIN   1200    // !! SET AFTER CALIBRATION !!

// ═══════════════════════════════════════════════════════════════
//  TOF THRESHOLDS  (distance in mm from sensor to bottle surface)
//  Run calibration mode to find your actual ranges.
//
//  How to set:
//    1. Run calibration mode
//    2. Note ToF distance range for glass bottles   → TOF_GLASS_MIN/MAX
//    3. Note ToF distance range for plastic bottles → TOF_PLASTIC_MIN/MAX
//    4. Anything outside both ranges = Reject
// ═══════════════════════════════════════════════════════════════
#define TOF_GLASS_MIN      10     // !! SET AFTER CALIBRATION !!
#define TOF_GLASS_MAX      90     // !! SET AFTER CALIBRATION !!
#define TOF_PLASTIC_MIN    91     // !! SET AFTER CALIBRATION !!
#define TOF_PLASTIC_MAX   160     // !! SET AFTER CALIBRATION !!

// ─── HuskyLens Classification IDs ────────────────────────────
// !! UPDATE AFTER TRAINING IF IDS DIFFER !!
#define HUSKY_PLASTIC_ID    1
#define HUSKY_GLASS_ID      2

// ─── Pin Definitions ─────────────────────────────────────────
#define IR_PIN             15
#define LDR_PIN             4
#define PROX_PLASTIC       26  // plastic bin fill level
#define PROX_GLASS         27  // glass bin fill level
#define RGB_R              13
#define RGB_G              14
#define RGB_B              33
#define TOUCH_CS_PIN       35

// ─── Servo Channels (PCA9685) ────────────────────────────────
#define SERVO_ENTRY_1       2
#define SERVO_ENTRY_2       3
#define SERVO_FLAP_A        4   // bin flaps — always open/close together
#define SERVO_FLAP_B        5
#define SERVO_REJECT        6
#define SERVO_DIVIDER       7

// ─── Servo Pulse Widths (microseconds) ───────────────────────
// Adjust OPEN values if your specific servos need different angles
#define SERVO_FREQ          50
#define ENTRY_OPEN_US     2000
#define ENTRY_CLOSED_US    800
#define FLAP_OPEN_US      2000   // ch4 & ch5 open together
#define FLAP_CLOSED_US     800   // ch4 & ch5 close together
#define REJECT_OPEN_US    2000
#define REJECT_CLOSED_US   800
#define DIVIDER_PLASTIC_US 1500  // ~90° neutral — guides bottle to plastic side
#define DIVIDER_GLASS_US    800  // ~30° (60° anticlockwise) — guides to glass side

// ─── Timing (ms) ─────────────────────────────────────────────
#define GATE_OPEN_DURATION    1500   // how long entry gate stays open
#define FLAP_OPEN_DURATION    1500   // how long bin flaps stay open
#define REJECT_OPEN_DURATION  1500   // how long reject flap stays open
#define DEBOUNCE_MS            300   // IR debounce
#define TOUCH_DEBOUNCE_MS      400   // touch debounce
#define CHUTE_TRAVEL_MS       1000   // time for bottle to reach chute sensors

// ─── QR Code Content ─────────────────────────────────────────
// Replace with your app's actual URL or deep-link
#define QR_CONTENT  "https://yourapp.com/connect"

// ─── TFT Colours ─────────────────────────────────────────────
#define COL_BG       TFT_BLACK
#define COL_WHITE    TFT_WHITE
#define COL_GREEN    0x07E0
#define COL_RED      TFT_RED
#define COL_YELLOW   TFT_YELLOW
#define COL_DARKGREY 0x4208

// ─── Objects ─────────────────────────────────────────────────
TFT_eSPI                tft;
XPT2046_Touchscreen     touch(TOUCH_CS_PIN);
HUSKYLENS               huskyLens;
Adafruit_VL53L0X        tof;
Adafruit_PWMServoDriver pwm;

// ─── State Machine ───────────────────────────────────────────
enum SystemState {
  STATE_IDLE,
  STATE_SESSION_ACTIVE,
  STATE_PROCESSING,
  STATE_DONE
};
SystemState systemState = STATE_IDLE;

// ─── Session Variables ────────────────────────────────────────
int           sessionBottleCount = 0;
unsigned long lastIRTrigger      = 0;
unsigned long lastTouchTime      = 0;
int           lastClassification = 0;
bool          plasticBinFull     = false;
bool          glassBinFull       = false;

// ─── Forward Declarations ────────────────────────────────────
void drawIdleScreen();
void drawSessionScreen(bool showResult, int result);
void drawDoneScreen();
void drawQRCode();
void setLED(bool r, bool g, bool b);
void setLEDWhite();
void setLEDGreen();
void setLEDRed();
void setLEDOff();
void servoWrite(uint8_t ch, uint16_t us);
void openEntryGate();
void closeEntryGate();
void openBinFlaps();
void closeBinFlaps();
void routeBottle(int classification);
void checkBinLevels();
void handleTouch();
void processBottle();
void endSession();
void runCalibrationMode();
int  getHuskyVote();
int  getLDRVote();
int  getToFVote();
int  majorityVote(int a, int b, int c);

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== Smart Bin Booting ===");

  // ── GPIO ──
  pinMode(IR_PIN,       INPUT);
  pinMode(LDR_PIN,      INPUT);
  pinMode(PROX_PLASTIC, INPUT);
  pinMode(PROX_GLASS,   INPUT);
  pinMode(RGB_R,        OUTPUT);
  pinMode(RGB_G,        OUTPUT);
  pinMode(RGB_B,        OUTPUT);
  setLEDOff();

  // ── I2C ──
  Wire.begin(21, 22);

  // ── TFT ──
  tft.init();
  tft.setRotation(0);   // Portrait. Change to 1/2/3 if screen appears rotated
  tft.fillScreen(COL_BG);

  // ── Touch ──
  touch.begin();

  // ── HuskyLens ──
  if (!huskyLens.begin(Wire)) {
    Serial.println("[WARN] HuskyLens not found — check wiring.");
  } else {
    huskyLens.writeAlgorithm(ALGORITHM_OBJECT_CLASSIFICATION);
    Serial.println("[OK] HuskyLens ready.");
  }

  // ── ToF ──
  if (!tof.begin()) {
    Serial.println("[WARN] VL53L0X not found — check wiring.");
  } else {
    Serial.println("[OK] VL53L0X ready.");
  }

  // ── Servo Driver ──
  pwm.begin();
  pwm.setOscillatorFrequency(27000000);
  pwm.setPWMFreq(SERVO_FREQ);
  delay(10);
  // Close all servos at startup
  servoWrite(SERVO_ENTRY_1,  ENTRY_CLOSED_US);
  servoWrite(SERVO_ENTRY_2,  ENTRY_CLOSED_US);
  servoWrite(SERVO_FLAP_A,   FLAP_CLOSED_US);
  servoWrite(SERVO_FLAP_B,   FLAP_CLOSED_US);
  servoWrite(SERVO_REJECT,   REJECT_CLOSED_US);
  servoWrite(SERVO_DIVIDER,  DIVIDER_PLASTIC_US);

  // ── Calibration mode splash ──
  if (DEBUG_CALIBRATION) {
    Serial.println("==============================");
    Serial.println("   CALIBRATION MODE ACTIVE    ");
    Serial.println("Place bottles at chute sensors");
    Serial.println("and read Serial Monitor output.");
    Serial.println("Servos disabled. No routing.  ");
    Serial.println("==============================");
    tft.fillScreen(COL_BG);
    tft.setTextColor(COL_YELLOW, COL_BG);
    tft.setTextSize(2);
    tft.setCursor(10, 120);
    tft.println("CALIBRATION");
    tft.setCursor(25, 145);
    tft.println("MODE ACTIVE");
    tft.setTextSize(1);
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.setCursor(10, 180);
    tft.println("See Serial Monitor");
    setLEDWhite();
    return;
  }

  drawIdleScreen();
  setLEDWhite();
  Serial.println("=== System Ready ===");
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {

  if (DEBUG_CALIBRATION) {
    runCalibrationMode();
    return;
  }

  checkBinLevels();
  handleTouch();

  switch (systemState) {

    case STATE_IDLE:
      // Waiting for QR scan / tap to begin session
      break;

    case STATE_SESSION_ACTIVE:
      if (digitalRead(IR_PIN) == LOW) {
        unsigned long now = millis();
        if (now - lastIRTrigger > DEBOUNCE_MS) {
          lastIRTrigger = now;
          systemState   = STATE_PROCESSING;
          processBottle();
        }
      }
      break;

    case STATE_PROCESSING:
      // Handled inside processBottle()
      break;

    case STATE_DONE:
      break;
  }

  delay(20);
}

// ═══════════════════════════════════════════════════════════════
//  CALIBRATION MODE
//  Prints sensor readings to Serial Monitor. No routing occurs.
//  Place bottles manually at the LDR and ToF position in chute.
// ═══════════════════════════════════════════════════════════════
void runCalibrationMode() {
  if (digitalRead(IR_PIN) == LOW) {
    delay(300); // let bottle settle

    // LDR
    int ldrRaw = analogRead(LDR_PIN);

    // ToF
    VL53L0X_RangingMeasurementData_t measure;
    tof.rangingTest(&measure, false);
    int tofDist = (measure.RangeStatus != 4) ? measure.RangeMilliMeter : -1;

    // HuskyLens
    int huskyID = 0;
    if (huskyLens.request() && huskyLens.isLearned()) {
      HUSKYLENSResult result = huskyLens.read();
      huskyID = result.ID;
    }

    Serial.println("─────────────────────────────────");
    Serial.print  ("LDR Raw Value  : "); Serial.println(ldrRaw);
    Serial.print  ("ToF Distance   : ");
    if (tofDist == -1) Serial.println("Out of range");
    else { Serial.print(tofDist); Serial.println(" mm"); }
    Serial.print  ("HuskyLens ID   : ");
    if      (huskyID == HUSKY_PLASTIC_ID) Serial.println("1 → Plastic");
    else if (huskyID == HUSKY_GLASS_ID)   Serial.println("2 → Glass");
    else if (huskyID == 0)                Serial.println("No result / Not trained");
    else                                  Serial.println("Unknown ID");
    Serial.println("─────────────────────────────────");

    delay(1500); // prevent re-trigger spam
  }
}

// ═══════════════════════════════════════════════════════════════
//  PROCESS BOTTLE
// ═══════════════════════════════════════════════════════════════
void processBottle() {
  Serial.println("\n[BOTTLE] Detected — classifying...");
  setLEDOff();

  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setTextSize(2);
  tft.setCursor(30, 140);
  tft.println("Scanning...");

  // ── Vote 1: HuskyLens at entrance ──
  int huskyVote = getHuskyVote();

  // ── Open entry gate — bottle drops into chute ──
  openEntryGate();
  delay(GATE_OPEN_DURATION);
  closeEntryGate();

  // ── Wait for bottle to travel to chute sensors ──
  delay(CHUTE_TRAVEL_MS);

  // ── Vote 2 & 3: LDR and ToF in chute ──
  int ldrVote = getLDRVote();
  int tofVote = getToFVote();

  // ── Majority vote ──
  int finalResult = majorityVote(huskyVote, ldrVote, tofVote);

  // ── Log all votes ──
  Serial.println("─── VOTE SUMMARY ─────────────────");
  Serial.print  ("HuskyLens : "); Serial.println(huskyVote == 1 ? "Plastic" : huskyVote == 2 ? "Glass" : "Reject");
  Serial.print  ("LDR       : "); Serial.println(ldrVote   == 1 ? "Plastic" : ldrVote   == 2 ? "Glass" : "Reject");
  Serial.print  ("ToF       : "); Serial.println(tofVote   == 1 ? "Plastic" : tofVote   == 2 ? "Glass" : "Reject");
  Serial.print  ("RESULT    : "); Serial.println(finalResult == 1 ? "PLASTIC" : finalResult == 2 ? "GLASS" : "REJECT");
  Serial.println("───────────────────────────────────");

  // ── Override if bin is full ──
  // If the target bin is full, redirect to reject rather than
  // forcing a bottle into a bin that has no space.
  if (finalResult == 1 && plasticBinFull) {
    Serial.println("[INFO] Plastic bin full — redirecting to Reject.");
    finalResult = 3;
  }
  if (finalResult == 2 && glassBinFull) {
    Serial.println("[INFO] Glass bin full — redirecting to Reject.");
    finalResult = 3;
  }

  // ── Route bottle ──
  routeBottle(finalResult);

  // ── Update count and feedback ──
  lastClassification = finalResult;
  if (finalResult == 1 || finalResult == 2) {
    sessionBottleCount++;
    setLEDGreen();
    Serial.print("[ACCEPTED] Session total: ");
    Serial.println(sessionBottleCount);
  } else {
    setLEDRed();
    Serial.println("[REJECTED]");
  }

  drawSessionScreen(true, finalResult);
  delay(2500);
  setLEDWhite();
  drawSessionScreen(false, 0);

  systemState = STATE_SESSION_ACTIVE;
}

// ═══════════════════════════════════════════════════════════════
//  SENSOR VOTES
// ═══════════════════════════════════════════════════════════════

int getHuskyVote() {
  if (huskyLens.request() && huskyLens.isLearned()) {
    HUSKYLENSResult result = huskyLens.read();
    if (result.ID == HUSKY_PLASTIC_ID) return 1;
    if (result.ID == HUSKY_GLASS_ID)   return 2;
  }
  return 3; // unrecognised or no result → reject vote
}

int getLDRVote() {
  int ldrRaw = analogRead(LDR_PIN);
  Serial.print("[LDR] Raw: "); Serial.println(ldrRaw);

  if (ldrRaw >= LDR_GLASS_MIN)                              return 2; // Glass
  if (ldrRaw >= LDR_PLASTIC_MIN && ldrRaw < LDR_GLASS_MIN) return 1; // Plastic
  return 3;                                                            // Reject
}

int getToFVote() {
  VL53L0X_RangingMeasurementData_t measure;
  tof.rangingTest(&measure, false);

  if (measure.RangeStatus == 4) {
    Serial.println("[ToF] Out of range → Reject vote");
    return 3;
  }

  int dist = measure.RangeMilliMeter;
  Serial.print("[ToF] Distance: "); Serial.print(dist); Serial.println(" mm");

  if (dist >= TOF_GLASS_MIN   && dist <= TOF_GLASS_MAX)   return 2; // Glass
  if (dist >= TOF_PLASTIC_MIN && dist <= TOF_PLASTIC_MAX) return 1; // Plastic
  return 3;                                                           // Reject
}

// 2-of-3 majority. All three disagree → reject.
int majorityVote(int a, int b, int c) {
  if (a == b) return a;
  if (a == c) return a;
  if (b == c) return b;
  return 3;
}

// ═══════════════════════════════════════════════════════════════
//  ROUTE BOTTLE
//  ch4 & ch5 always open and close together.
//  Divider (ch7) position determines which bin bottle falls into.
// ═══════════════════════════════════════════════════════════════
void routeBottle(int classification) {
  if (classification == 1) {
    // Plastic — divider stays neutral (90°), bottle guided to plastic side
    servoWrite(SERVO_DIVIDER, DIVIDER_PLASTIC_US);
    delay(300);
    openBinFlaps();
    delay(FLAP_OPEN_DURATION);
    closeBinFlaps();

  } else if (classification == 2) {
    // Glass — divider rotates 60° CCW, bottle guided to glass side
    servoWrite(SERVO_DIVIDER, DIVIDER_GLASS_US);
    delay(300);
    openBinFlaps();
    delay(FLAP_OPEN_DURATION);
    closeBinFlaps();
    delay(300);
    servoWrite(SERVO_DIVIDER, DIVIDER_PLASTIC_US); // return divider to neutral

  } else {
    // Reject — reject flap opens outward into rejected bin
    servoWrite(SERVO_REJECT, REJECT_OPEN_US);
    delay(REJECT_OPEN_DURATION);
    servoWrite(SERVO_REJECT, REJECT_CLOSED_US);
  }
}

// ═══════════════════════════════════════════════════════════════
//  ENTRY GATE (ch2 & ch3)
// ═══════════════════════════════════════════════════════════════
void openEntryGate() {
  servoWrite(SERVO_ENTRY_1, ENTRY_OPEN_US);
  servoWrite(SERVO_ENTRY_2, ENTRY_OPEN_US);
}

void closeEntryGate() {
  servoWrite(SERVO_ENTRY_1, ENTRY_CLOSED_US);
  servoWrite(SERVO_ENTRY_2, ENTRY_CLOSED_US);
}

// ═══════════════════════════════════════════════════════════════
//  BIN FLAPS (ch4 & ch5 — always together)
// ═══════════════════════════════════════════════════════════════
void openBinFlaps() {
  servoWrite(SERVO_FLAP_A, FLAP_OPEN_US);
  servoWrite(SERVO_FLAP_B, FLAP_OPEN_US);
}

void closeBinFlaps() {
  servoWrite(SERVO_FLAP_A, FLAP_CLOSED_US);
  servoWrite(SERVO_FLAP_B, FLAP_CLOSED_US);
}

// ═══════════════════════════════════════════════════════════════
//  BIN LEVEL CHECK
//  LOW = bin full (flip to HIGH if your sensors output inverted)
// ═══════════════════════════════════════════════════════════════
void checkBinLevels() {
  plasticBinFull = (digitalRead(PROX_PLASTIC) == LOW);
  glassBinFull   = (digitalRead(PROX_GLASS)   == LOW);
  if (plasticBinFull) Serial.println("[WARN] Plastic bin full!");
  if (glassBinFull)   Serial.println("[WARN] Glass bin full!");
}

// ═══════════════════════════════════════════════════════════════
//  TOUCH HANDLER
// ═══════════════════════════════════════════════════════════════
void handleTouch() {
  if (!touch.touched()) return;

  unsigned long now = millis();
  if (now - lastTouchTime < TOUCH_DEBOUNCE_MS) return;
  lastTouchTime = now;

  TS_Point p = touch.getPoint();

  // Map raw touch coordinates to screen pixels (240x320 portrait)
  // Calibrate these map() ranges if touch position is inaccurate
  int x = map(p.x, 200, 3800, 0, 240);
  int y = map(p.y, 200, 3800, 0, 320);

  Serial.print("[TOUCH] x="); Serial.print(x);
  Serial.print(" y=");        Serial.println(y);

  if (systemState == STATE_IDLE) {
    // Any tap on idle screen starts a session
    // Production: trigger from app via WiFi/BLE instead
    sessionBottleCount = 0;
    systemState = STATE_SESSION_ACTIVE;
    drawSessionScreen(false, 0);
    Serial.println("[SESSION] Started.");

  } else if (systemState == STATE_SESSION_ACTIVE) {
    // DONE button occupies y > 220 (bottom ~100px of screen)
    if (y > 220) {
      endSession();
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  END SESSION
// ═══════════════════════════════════════════════════════════════
void endSession() {
  systemState = STATE_DONE;
  Serial.print("[SESSION] Ended. Accepted bottles: ");
  Serial.println(sessionBottleCount);

  drawDoneScreen();
  setLEDWhite();

  // ── App Integration Point ──────────────────────────────────
  // When WiFi/BLE is implemented, send session data here:
  //   POST /api/session/end
  //   { "userId": currentUserId, "bottles": sessionBottleCount }
  // The backend calculates and credits the simulated reward.
  // ──────────────────────────────────────────────────────────

  delay(6000);

  // Reset for next user
  sessionBottleCount = 0;
  lastClassification = 0;
  systemState = STATE_IDLE;
  drawIdleScreen();
}

// ═══════════════════════════════════════════════════════════════
//  SERVO HELPER  — converts microseconds to PCA9685 ticks
// ═══════════════════════════════════════════════════════════════
void servoWrite(uint8_t channel, uint16_t us) {
  uint16_t ticks = (uint16_t)((us / 1000000.0) * SERVO_FREQ * 4096);
  pwm.setPWM(channel, 0, ticks);
}

// ═══════════════════════════════════════════════════════════════
//  RGB LED  — Common Cathode (HIGH = colour ON)
//  Both LEDs wired in parallel — always same colour
// ═══════════════════════════════════════════════════════════════
void setLED(bool r, bool g, bool b) {
  digitalWrite(RGB_R, r ? HIGH : LOW);
  digitalWrite(RGB_G, g ? HIGH : LOW);
  digitalWrite(RGB_B, b ? HIGH : LOW);
}
void setLEDWhite() { setLED(true,  true,  true);  }   // idle / default
void setLEDGreen() { setLED(false, true,  false); }   // accepted
void setLEDRed()   { setLED(true,  false, false); }   // rejected
void setLEDOff()   { setLED(false, false, false); }   // processing

// ═══════════════════════════════════════════════════════════════
//  TFT SCREENS
// ═══════════════════════════════════════════════════════════════

void drawIdleScreen() {
  tft.fillScreen(COL_BG);

  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setTextSize(1);
  tft.setCursor(30, 10);
  tft.println("SMART WASTE BIN");
  tft.setCursor(20, 22);
  tft.println("Scan to start session");

  drawQRCode();

  tft.setTextSize(1);
  tft.setTextColor(COL_DARKGREY, COL_BG);
  tft.setCursor(40, 305);
  tft.println("or tap to begin");
}

void drawSessionScreen(bool showResult, int result) {
  tft.fillScreen(COL_BG);

  // Header
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setTextSize(1);
  tft.setCursor(20, 10);
  tft.println("SESSION ACTIVE");

  // Bottle count
  tft.setTextSize(2);
  tft.setTextColor(COL_YELLOW, COL_BG);
  tft.setCursor(10, 30);
  tft.print("Bottles: ");
  tft.println(sessionBottleCount);

  // Bin levels
  tft.setTextSize(1);
  tft.setTextColor(plasticBinFull ? COL_RED : COL_GREEN, COL_BG);
  tft.setCursor(10, 65);
  tft.print("Plastic bin: ");
  tft.println(plasticBinFull ? "FULL" : "OK");

  tft.setTextColor(glassBinFull ? COL_RED : COL_GREEN, COL_BG);
  tft.setCursor(10, 80);
  tft.print("Glass bin:   ");
  tft.println(glassBinFull ? "FULL" : "OK");

  // Result symbol
  if (showResult) {
    if (result == 1 || result == 2) {
      // Large green tick
      tft.setTextSize(8);
      tft.setTextColor(COL_GREEN, COL_BG);
      tft.setCursor(70, 110);
      tft.println("v");
      tft.setTextSize(1);
      tft.setTextColor(COL_GREEN, COL_BG);
      tft.setCursor(75, 195);
      tft.println(result == 1 ? "PLASTIC" : "GLASS");
      tft.setCursor(70, 207);
      tft.println("ACCEPTED");
    } else {
      // Large red X
      tft.setTextSize(8);
      tft.setTextColor(COL_RED, COL_BG);
      tft.setCursor(70, 110);
      tft.println("X");
      tft.setTextSize(1);
      tft.setTextColor(COL_RED, COL_BG);
      tft.setCursor(70, 195);
      tft.println("REJECTED");
    }
  } else {
    tft.setTextSize(1);
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.setCursor(25, 140);
    tft.println("Place bottle in slot");
  }

  // DONE button
  tft.fillRoundRect(20, 225, 200, 45, 8, COL_GREEN);
  tft.setTextColor(COL_BG, COL_GREEN);
  tft.setTextSize(2);
  tft.setCursor(65, 240);
  tft.println("DONE");
}

void drawDoneScreen() {
  tft.fillScreen(COL_BG);

  tft.setTextColor(COL_GREEN, COL_BG);
  tft.setTextSize(2);
  tft.setCursor(30, 80);
  tft.println("SESSION DONE");

  tft.setTextSize(1);
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setCursor(25, 120);
  tft.println("Bottles accepted:");

  tft.setTextSize(4);
  tft.setTextColor(COL_YELLOW, COL_BG);
  tft.setCursor(95, 145);
  tft.println(sessionBottleCount);

  tft.setTextSize(1);
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setCursor(10, 210);
  tft.println("Check your app for");
  tft.setCursor(10, 222);
  tft.println("reward details.");

  tft.setTextColor(COL_DARKGREY, COL_BG);
  tft.setCursor(20, 300);
  tft.println("Returning to home...");
}

void drawQRCode() {
  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(3)];
  qrcode_initText(&qrcode, qrcodeData, 3, ECC_LOW, QR_CONTENT);

  int moduleSize = 4;
  int qrSize     = qrcode.size * moduleSize;
  int offsetX    = (240 - qrSize) / 2;
  int offsetY    = 40;

  // White border behind QR for scannability
  tft.fillRect(offsetX - 4, offsetY - 4, qrSize + 8, qrSize + 8, TFT_WHITE);

  for (uint8_t y = 0; y < qrcode.size; y++) {
    for (uint8_t x = 0; x < qrcode.size; x++) {
      uint16_t colour = qrcode_getModule(&qrcode, x, y) ? TFT_BLACK : TFT_WHITE;
      tft.fillRect(offsetX + x * moduleSize,
                   offsetY + y * moduleSize,
                   moduleSize, moduleSize, colour);
    }
  }
}
