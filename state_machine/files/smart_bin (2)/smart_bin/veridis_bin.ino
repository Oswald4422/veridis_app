/*
 * ============================================================
 *  VERIDIS — SMART WASTE SEGREGATION SYSTEM
 *  Platform : ESP32
 * ============================================================
 *
 *  SEQUENCE OF OPERATION
 *  ─────────────────────
 *  1.  Power on  → All servos CLOSED, Both LEDs OFF
 *  2.  IR beam broken → Both LEDs ON WHITE (light for HuskyLens)
 *  3.  HuskyLens votes (plastic / glass / reject)
 *  4.  Ch2 & Ch3 OPEN  → bottle enters chute
 *  5.  Ch2 & Ch3 CLOSE
 *  6.  Wait CHUTE_TRAVEL_MS for bottle to reach chute sensors
 *  7.  Both LEDs stay WHITE (light for LDR)
 *  8.  LDR votes + ToF votes
 *  9.  Majority vote (2-of-3) → final classification
 * 10.  Ch7 divider moves to correct position
 * 11.  Ch4 & Ch5 OPEN → bottle falls into bin
 * 12.  Ch4 & Ch5 CLOSE → Ch7 returns to neutral (glass only)
 * 13.  Accepted → both LEDs GREEN  |  Rejected → both LEDs RED
 * 14.  Feedback delay → both LEDs OFF, TFT updates, count++
 * 15.  Return to idle (LEDs OFF, all servos CLOSED)
 *
 *  CALIBRATION MODE
 *  ─────────────────
 *  Set DEBUG_CALIBRATION true → prints raw LDR & ToF readings
 *  to Serial Monitor. Servos do NOT move. Safe for testing.
 *  Set back to false for normal operation.
 *
 * ============================================================
 *
 *  PIN SUMMARY
 * ─────────────────────────────────────────────────────────────
 *  TFT Display (SPI)
 *    TFT_CS   → GPIO 5      TFT_DC   → GPIO 17
 *    TFT_RST  → GPIO 2      TFT_MOSI → GPIO 23 (shared SPI)
 *    TFT_SCK  → GPIO 18 (shared SPI)
 *    TFT_MISO → GPIO 19 (shared SPI)
 *
 *  Touch XPT2046 (shared SPI)
 *    T_CS     → GPIO 35     T_IRQ    → NOT CONNECTED
 *
 *  I2C Bus  (HuskyLens + ToF + Servo Driver share this)
 *    SDA → GPIO 21          SCL → GPIO 22
 *
 *  HuskyLens  → I2C   (ID 1 = Plastic, ID 2 = Glass)
 *  VL53L0X    → I2C   address 0x29
 *  PCA9685    → I2C
 *    Ch 2 & 3 → Entry gate flaps
 *    Ch 4 & 5 → Bin flaps (always open/close together)
 *    Ch 6     → Rejection flap
 *    Ch 7     → Bin divider (neutral=plastic, 60°CCW=glass)
 *
 *  IR Sensor        → GPIO 15  (bottle presence / count trigger)
 *  LDR Sensor       → GPIO 4   (analog, chute light reading)
 *  Proximity 1      → GPIO 26  (plastic bin fill level)
 *  Proximity 2      → GPIO 27  (glass bin fill level)
 *
 *  RGB LED 1 & 2 (parallel, common cathode)
 *    R → GPIO 13   G → GPIO 14   B → GPIO 33
 *
 * ============================================================
 *  LIBRARIES
 *  ─────────────────────────────────────────────────────────────
 *  Install via Library Manager (or copy .h/.cpp to sketch folder):
 *    TFT_eSPI, XPT2046_Touchscreen, Adafruit_VL53L0X,
 *    Adafruit_PWMServoDriver, QRCode (ricmoo)
 *
 *  Copied to sketch folder (use "" not <>):
 *    HUSKYLENS.h, qrcode.h
 *
 *  TFT_eSPI User_Setup.h must define:
 *    #define ILI9341_DRIVER
 *    #define TFT_CS   5    #define TFT_DC   17   #define TFT_RST  2
 *    #define TFT_MOSI 23   #define TFT_SCLK 18   #define TFT_MISO 19
 *    #define TOUCH_CS 35   #define SPI_FREQUENCY 40000000
 * ============================================================
 */

// ─── Libraries ───────────────────────────────────────────────
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Wire.h>
#include "HUSKYLENS.h"                // copied to sketch folder
#include <Adafruit_VL53L0X.h>
#include <Adafruit_PWMServoDriver.h>
#include "qrcode.h"                   // copied to sketch folder

// ═══════════════════════════════════════════════════════════════
//  CALIBRATION MODE
//  true  → prints LDR & ToF readings, servos do NOT move
//  false → normal operation
// ═══════════════════════════════════════════════════════════════
#define DEBUG_CALIBRATION   false

// ═══════════════════════════════════════════════════════════════
//  LDR THRESHOLDS  (0–4095, 12-bit ADC)
//  Glass transmits more light  → higher reading
//  Plastic transmits less      → lower reading
//  Non-bottle                  → very low reading
//  !! SET AFTER RUNNING CALIBRATION MODE !!
// ═══════════════════════════════════════════════════════════════
#define LDR_GLASS_MIN     2800
#define LDR_PLASTIC_MIN   1200

// ═══════════════════════════════════════════════════════════════
//  TOF THRESHOLDS  (mm)
//  !! SET AFTER RUNNING CALIBRATION MODE !!
// ═══════════════════════════════════════════════════════════════
#define TOF_GLASS_MIN      10
#define TOF_GLASS_MAX      90
#define TOF_PLASTIC_MIN    91
#define TOF_PLASTIC_MAX   160

// ─── HuskyLens IDs ───────────────────────────────────────────
// !! UPDATE AFTER TRAINING IF IDS DIFFER !!
#define HUSKY_PLASTIC_ID    1
#define HUSKY_GLASS_ID      2

// ─── Pin Definitions ─────────────────────────────────────────
#define IR_PIN             15
#define LDR_PIN             4
#define PROX_PLASTIC       26
#define PROX_GLASS         27
#define RGB_R              13
#define RGB_G              14
#define RGB_B              33
#define TOUCH_CS_PIN       35

// ─── Servo Channels (PCA9685) ────────────────────────────────
#define SERVO_ENTRY_1       2
#define SERVO_ENTRY_2       3
#define SERVO_FLAP_A        4
#define SERVO_FLAP_B        5
#define SERVO_REJECT        6
#define SERVO_DIVIDER       7

// ─── Servo Pulse Widths (microseconds) ───────────────────────
// If a servo moves in the wrong direction swap its OPEN and CLOSED values
#define SERVO_FREQ            50

#define ENTRY_OPEN_US       2000      // !! SWAP WITH CLOSED IF DIRECTION WRONG !!
#define ENTRY_CLOSED_US      800

#define FLAP_OPEN_US        2000
#define FLAP_CLOSED_US       800

#define REJECT_OPEN_US      2000
#define REJECT_CLOSED_US     800

#define DIVIDER_PLASTIC_US  1500      // ~90° neutral
#define DIVIDER_GLASS_US     800      // ~30°  (60° anticlockwise)

// ─── Timing (ms) ─────────────────────────────────────────────
#define GATE_OPEN_DURATION    1500    // entry gate stays open
#define FLAP_OPEN_DURATION    1500    // bin flaps stay open
#define REJECT_OPEN_DURATION  1500    // reject flap stays open
#define CHUTE_TRAVEL_MS       1000    // bottle travel to chute sensors
#define DIVIDER_SETTLE_MS      400    // divider settle before flaps open
#define FEEDBACK_DURATION     2500    // green/red LED display duration
#define DEBOUNCE_MS            300    // IR debounce
#define TOUCH_DEBOUNCE_MS      400    // touch debounce
#define SERVO_INIT_DELAY_MS    600    // PCA9685 stabilise at boot

// ─── QR Code ─────────────────────────────────────────────────
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
void initServosImmediately();
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
  delay(200);
  Serial.println("=== VERIDIS Booting ===");

  // ── GPIO ──────────────────────────────────────────────────
  pinMode(IR_PIN,       INPUT);
  pinMode(LDR_PIN,      INPUT);
  pinMode(PROX_PLASTIC, INPUT);
  pinMode(PROX_GLASS,   INPUT);
  pinMode(RGB_R,        OUTPUT);
  pinMode(RGB_G,        OUTPUT);
  pinMode(RGB_B,        OUTPUT);

  // LEDs OFF immediately at boot
  setLEDOff();

  // ── I2C ──────────────────────────────────────────────────
  Wire.begin(21, 22);

  // ── Servo Driver — initialise FIRST and send CLOSED ──────
  // This must happen before anything else to prevent power-on
  // drift where gates open before the ESP32 sends commands.
  pwm.begin();
  pwm.setOscillatorFrequency(27000000);
  pwm.setPWMFreq(SERVO_FREQ);
  delay(SERVO_INIT_DELAY_MS);     // let PCA9685 fully stabilise
  initServosImmediately();         // send CLOSED to all channels NOW
  Serial.println("[OK] Servos initialised closed.");

  // ── TFT ──────────────────────────────────────────────────
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(COL_BG);

  // ── Touch ────────────────────────────────────────────────
  touch.begin();

  // ── HuskyLens ────────────────────────────────────────────
  if (!huskyLens.begin(Wire)) {
    Serial.println("[WARN] HuskyLens not found — check wiring.");
  } else {
    huskyLens.writeAlgorithm(ALGORITHM_OBJECT_CLASSIFICATION);
    Serial.println("[OK] HuskyLens ready.");
  }

  // ── ToF ──────────────────────────────────────────────────
  if (!tof.begin()) {
    Serial.println("[WARN] VL53L0X not found — check wiring.");
  } else {
    Serial.println("[OK] VL53L0X ready.");
  }

  // ── Calibration splash or normal idle ────────────────────
  if (DEBUG_CALIBRATION) {
    Serial.println("==============================");
    Serial.println("   CALIBRATION MODE ACTIVE    ");
    Serial.println("Place bottles at chute sensors");
    Serial.println("and read Serial Monitor output.");
    Serial.println("Servos will NOT move.         ");
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
    // LEDs stay OFF in calibration mode
    return;
  }

  drawIdleScreen();
  // LEDs stay OFF at idle
  Serial.println("=== System Ready — Idle ===");
}

// ═══════════════════════════════════════════════════════════════
//  INIT SERVOS IMMEDIATELY
//  Called at the very start of setup() before anything else.
//  Sends CLOSED position to every channel so gates cannot drift.
// ═══════════════════════════════════════════════════════════════
void initServosImmediately() {
  servoWrite(SERVO_ENTRY_1,  ENTRY_CLOSED_US);
  servoWrite(SERVO_ENTRY_2,  ENTRY_CLOSED_US);
  servoWrite(SERVO_FLAP_A,   FLAP_CLOSED_US);
  servoWrite(SERVO_FLAP_B,   FLAP_CLOSED_US);
  servoWrite(SERVO_REJECT,   REJECT_CLOSED_US);
  servoWrite(SERVO_DIVIDER,  DIVIDER_PLASTIC_US);
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {

  // Calibration mode — only print readings, nothing else
  if (DEBUG_CALIBRATION) {
    runCalibrationMode();
    return;
  }

  checkBinLevels();
  handleTouch();

  switch (systemState) {

    case STATE_IDLE:
      // Waiting for QR scan or tap to start session
      // LEDs are OFF here
      break;

    case STATE_SESSION_ACTIVE:
      // Watch for bottle at IR sensor
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
      // Handled entirely inside processBottle()
      break;

    case STATE_DONE:
      break;
  }

  delay(20);
}

// ═══════════════════════════════════════════════════════════════
//  CALIBRATION MODE
//  Prints raw LDR + ToF + HuskyLens readings.
//  Servos do NOT move. Place bottles manually at chute sensors.
// ═══════════════════════════════════════════════════════════════
void runCalibrationMode() {
  if (digitalRead(IR_PIN) == LOW) {
    delay(300);

    int ldrRaw = analogRead(LDR_PIN);

    VL53L0X_RangingMeasurementData_t measure;
    tof.rangingTest(&measure, false);
    int tofDist = (measure.RangeStatus != 4) ? measure.RangeMilliMeter : -1;

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

    delay(1500);
  }
}

// ═══════════════════════════════════════════════════════════════
//  PROCESS BOTTLE — full sequence from IR trigger to routing
// ═══════════════════════════════════════════════════════════════
void processBottle() {
  Serial.println("\n[IR] Bottle detected at entrance.");

  // ── STEP 1: Turn LEDs on WHITE for HuskyLens ─────────────
  setLEDWhite();
  Serial.println("[LED] White ON — HuskyLens illuminated.");

  // Show scanning on TFT
  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setTextSize(2);
  tft.setCursor(30, 140);
  tft.println("Scanning...");

  // Brief settle for HuskyLens to see the bottle clearly
  delay(300);

  // ── STEP 2: HuskyLens votes at entrance ──────────────────
  int huskyVote = getHuskyVote();
  Serial.print("[HUSKY] Vote: ");
  Serial.println(huskyVote == 1 ? "Plastic" : huskyVote == 2 ? "Glass" : "Reject");

  // ── STEP 3: Open entry gate — bottle drops into chute ────
  Serial.println("[GATE] Entry gate opening...");
  openEntryGate();
  delay(GATE_OPEN_DURATION);
  closeEntryGate();
  Serial.println("[GATE] Entry gate closed.");

  // ── STEP 4: Wait for bottle to travel to chute sensors ───
  // LEDs stay WHITE during travel — illuminate for LDR
  Serial.println("[CHUTE] Waiting for bottle to reach chute sensors...");
  delay(CHUTE_TRAVEL_MS);

  // ── STEP 5: LDR and ToF vote in chute ────────────────────
  int ldrVote = getLDRVote();
  int tofVote = getToFVote();

  // ── STEP 6: Majority vote ─────────────────────────────────
  int finalResult = majorityVote(huskyVote, ldrVote, tofVote);

  // ── Log all votes ─────────────────────────────────────────
  Serial.println("─── VOTE SUMMARY ──────────────────");
  Serial.print  ("HuskyLens : "); Serial.println(huskyVote == 1 ? "Plastic" : huskyVote == 2 ? "Glass" : "Reject");
  Serial.print  ("LDR       : "); Serial.println(ldrVote   == 1 ? "Plastic" : ldrVote   == 2 ? "Glass" : "Reject");
  Serial.print  ("ToF       : "); Serial.println(tofVote   == 1 ? "Plastic" : tofVote   == 2 ? "Glass" : "Reject");
  Serial.print  ("RESULT    : "); Serial.println(finalResult == 1 ? "PLASTIC" : finalResult == 2 ? "GLASS" : "REJECT");
  Serial.println("───────────────────────────────────");

  // ── STEP 7: Bin full override ─────────────────────────────
  if (finalResult == 1 && plasticBinFull) {
    Serial.println("[INFO] Plastic bin full — redirecting to Reject.");
    finalResult = 3;
  }
  if (finalResult == 2 && glassBinFull) {
    Serial.println("[INFO] Glass bin full — redirecting to Reject.");
    finalResult = 3;
  }

  // ── STEP 8: Route bottle ──────────────────────────────────
  // Ch7, Ch4, Ch5 only move HERE — after full classification
  routeBottle(finalResult);

  // ── STEP 9: LED feedback and session update ───────────────
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

  // Update TFT with result
  drawSessionScreen(true, finalResult);

  // Hold feedback colour for user to see
  delay(FEEDBACK_DURATION);

  // ── STEP 10: Reset LEDs OFF, return to idle session state ─
  setLEDOff();
  drawSessionScreen(false, 0);

  systemState = STATE_SESSION_ACTIVE;
  Serial.println("[READY] Waiting for next bottle.");
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
  return 3; // unrecognised → reject vote
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
//  Ch7, Ch4, Ch5 are ONLY called from here — never before
//  final classification is complete.
// ═══════════════════════════════════════════════════════════════
void routeBottle(int classification) {
  if (classification == 1) {
    // ── Plastic ──────────────────────────────────────────────
    // Divider stays neutral (bottle guided to plastic side)
    servoWrite(SERVO_DIVIDER, DIVIDER_PLASTIC_US);
    delay(DIVIDER_SETTLE_MS);
    openBinFlaps();
    delay(FLAP_OPEN_DURATION);
    closeBinFlaps();
    Serial.println("[ROUTE] Plastic bin.");

  } else if (classification == 2) {
    // ── Glass ────────────────────────────────────────────────
    // Divider rotates 60° anticlockwise (bottle guided to glass side)
    servoWrite(SERVO_DIVIDER, DIVIDER_GLASS_US);
    delay(DIVIDER_SETTLE_MS);
    openBinFlaps();
    delay(FLAP_OPEN_DURATION);
    closeBinFlaps();
    delay(DIVIDER_SETTLE_MS);
    servoWrite(SERVO_DIVIDER, DIVIDER_PLASTIC_US); // return to neutral
    Serial.println("[ROUTE] Glass bin.");

  } else {
    // ── Reject ───────────────────────────────────────────────
    // Reject flap opens outward — Ch4, Ch5, Ch7 do NOT move
    servoWrite(SERVO_REJECT, REJECT_OPEN_US);
    delay(REJECT_OPEN_DURATION);
    servoWrite(SERVO_REJECT, REJECT_CLOSED_US);
    Serial.println("[ROUTE] Rejected bin.");
  }
}

// ═══════════════════════════════════════════════════════════════
//  ENTRY GATE (Ch2 & Ch3)
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
//  BIN FLAPS (Ch4 & Ch5 — always together)
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
//  LOW = bin full. Flip to HIGH if your sensors output inverted.
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

  // Calibrate map() values if touch position is inaccurate
  int x = map(p.x, 200, 3800, 0, 240);
  int y = map(p.y, 200, 3800, 0, 320);

  Serial.print("[TOUCH] x="); Serial.print(x);
  Serial.print(" y=");        Serial.println(y);

  if (systemState == STATE_IDLE) {
    // Any tap starts a session
    // Production: replace with WiFi/BLE trigger from app
    sessionBottleCount = 0;
    systemState = STATE_SESSION_ACTIVE;
    drawSessionScreen(false, 0);
    Serial.println("[SESSION] Started.");

  } else if (systemState == STATE_SESSION_ACTIVE) {
    // DONE button occupies bottom of screen (y > 220)
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
  Serial.print("[SESSION] Ended. Bottles accepted: ");
  Serial.println(sessionBottleCount);

  drawDoneScreen();
  setLEDOff(); // LEDs off at session end

  // ── App Integration Point ──────────────────────────────────
  // Send sessionBottleCount to backend via WiFi/BLE here:
  //   POST /api/session/end
  //   { "userId": currentUserId, "bottles": sessionBottleCount }
  // ──────────────────────────────────────────────────────────

  delay(6000);

  // Reset for next user
  sessionBottleCount = 0;
  lastClassification = 0;
  systemState = STATE_IDLE;
  setLEDOff();
  drawIdleScreen();
}

// ═══════════════════════════════════════════════════════════════
//  SERVO HELPER — microseconds to PCA9685 ticks
//  Also acts as the continuous hold signal — calling this
//  repeatedly keeps the servo locked in position.
// ═══════════════════════════════════════════════════════════════
void servoWrite(uint8_t channel, uint16_t us) {
  uint16_t ticks = (uint16_t)((us / 1000000.0) * SERVO_FREQ * 4096);
  pwm.setPWM(channel, 0, ticks);
}

// ═══════════════════════════════════════════════════════════════
//  RGB LED — Common Cathode (HIGH = colour ON)
//  Both LEDs wired in parallel — always same colour
// ═══════════════════════════════════════════════════════════════
void setLED(bool r, bool g, bool b) {
  digitalWrite(RGB_R, r ? HIGH : LOW);
  digitalWrite(RGB_G, g ? HIGH : LOW);
  digitalWrite(RGB_B, b ? HIGH : LOW);
}
void setLEDWhite() { setLED(true,  true,  true);  }  // HuskyLens / LDR illumination
void setLEDGreen() { setLED(false, true,  false); }  // accepted
void setLEDRed()   { setLED(true,  false, false); }  // rejected
void setLEDOff()   { setLED(false, false, false); }  // idle / saving power

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
