/*
 * ============================================================
 *  VERIDIS — Phase 0: I2C Bus Scanner
 * ============================================================
 *  PURPOSE:
 *    Scan the I2C bus and report every device found.
 *    Run this FIRST to confirm all three I2C devices
 *    respond at their expected addresses:
 *      - VL53L0X   → 0x29
 *      - HuskyLens → 0x32
 *      - PCA9685   → 0x40
 *
 *  WIRING:
 *    SDA → GPIO 21   (shared bus)
 *    SCL → GPIO 22   (shared bus)
 *    4.7 kΩ pull-up resistors from SDA → 3.3V
 *    4.7 kΩ pull-up resistors from SCL → 3.3V
 *    All device VCC → 5V rail
 *    All device GND → common GND
 *
 *  WHAT TO LOOK FOR:
 *    - All three addresses appear every scan cycle.
 *    - No "unknown" addresses (could mean a wiring short).
 *    - If a device drops out intermittently, suspect loose
 *      wiring or insufficient pull-up strength.
 * ============================================================
 */

#include <Wire.h>

// ---------- known VERIDIS I2C addresses ----------
struct I2CDevice {
  uint8_t address;
  const char* name;
};

const I2CDevice KNOWN_DEVICES[] = {
  { 0x29, "VL53L0X (ToF)" },
  { 0x32, "HuskyLens"     },
  { 0x40, "PCA9685"       },
};
const uint8_t NUM_KNOWN = sizeof(KNOWN_DEVICES) / sizeof(KNOWN_DEVICES[0]);

// ---------- helpers ----------
const char* lookupName(uint8_t addr) {
  for (uint8_t i = 0; i < NUM_KNOWN; i++) {
    if (KNOWN_DEVICES[i].address == addr) return KNOWN_DEVICES[i].name;
  }
  return "UNKNOWN";
}

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }
  Wire.begin(21, 22);  // SDA, SCL
  Serial.println();
  Serial.println("=== VERIDIS I2C Bus Scanner ===");
  Serial.println("Scanning every 3 seconds...\n");
}

// ---------- loop ----------
void loop() {
  uint8_t found = 0;

  Serial.println("--- scan start ---");

  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();

    if (err == 0) {
      found++;
      Serial.print("  0x");
      if (addr < 16) Serial.print("0");
      Serial.print(addr, HEX);
      Serial.print("  →  ");
      Serial.println(lookupName(addr));
    }
  }

  if (found == 0) {
    Serial.println("  *** No devices found! Check wiring & pull-ups ***");
  } else {
    Serial.print("  Total: ");
    Serial.print(found);
    Serial.println(" device(s)");
  }

  // flag any expected devices that are missing
  for (uint8_t i = 0; i < NUM_KNOWN; i++) {
    Wire.beginTransmission(KNOWN_DEVICES[i].address);
    if (Wire.endTransmission() != 0) {
      Serial.print("  ⚠  MISSING: ");
      Serial.print(KNOWN_DEVICES[i].name);
      Serial.print(" (0x");
      if (KNOWN_DEVICES[i].address < 16) Serial.print("0");
      Serial.print(KNOWN_DEVICES[i].address, HEX);
      Serial.println(")");
    }
  }

  Serial.println("--- scan end ---\n");
  delay(3000);
}
