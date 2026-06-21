# VERIDIS — Incremental Integration Test Sketches

## Overview

Six phases of Arduino sketches for integrating the VERIDIS smart waste
segregation system. Each phase adds components incrementally so that
failures are easy to isolate.

**Run each phase for at least 10–15 minutes before moving on.**


## Final Pin Assignments

| GPIO | Function            | Bus/Type | Notes                          |
|------|---------------------|----------|--------------------------------|
| 2    | TFT RST             | Digital  | Moved from 16 to avoid HX711  |
| 4    | LDR module          | ADC      | Analog transparency reading    |
| 5    | TFT CS              | SPI      |                                |
| 15   | IR break-beam       | Digital  | Item detection trigger         |
| 16   | HX711 DT            | Digital  | Load cell data                 |
| 17   | TFT DC              | SPI ctrl |                                |
| 18   | TFT CLK             | SPI      | Freed from proximity sensor    |
| 19   | TFT MISO            | SPI      |                                |
| 21   | I2C SDA             | I2C      | Shared: VL53L0X, HuskyLens, PCA9685 |
| 22   | I2C SCL             | I2C      | Shared: 4.7 kΩ pull-ups to 3.3V |
| 23   | TFT MOSI            | SPI      |                                |
| 25   | HX711 SCK           | Digital  | Load cell clock                |
| 27   | Proximity sensor    | Digital  | Moved from 18 to avoid SPI CLK |


## Pin Conflict Resolutions

Two conflicts from the original mapping have been resolved:

1. **GPIO 16** was shared by HX711 DT and TFT RST.
   → TFT RST moved to **GPIO 2**.

2. **GPIO 18** was shared by the proximity sensor and SPI CLK.
   → Proximity sensor moved to **GPIO 27**.

Update your TFT_eSPI `User_Setup.h` accordingly:
```
#define TFT_RST  2    // was 16
```


## Required Libraries

Install all via Arduino Library Manager:

| Library                        | Author     | Used in       |
|--------------------------------|------------|---------------|
| Adafruit_VL53L0X               | Adafruit   | Phase 1+      |
| HUSKYLENS                      | DFRobot    | Phase 1+      |
| Adafruit PWM Servo Driver      | Adafruit   | Phase 2+      |
| TFT_eSPI                       | Bodmer     | Phase 3+      |
| HX711                          | bogde      | Phase 5+      |


## Phases

### Phase 0 — I2C Bus Scan
Confirms all three I2C devices respond at expected addresses.
No sensor libraries needed — just Wire.h.

### Phase 1 — VL53L0X + HuskyLens
First I2C pairing. Reads distance and object ID in the same loop.
Tracks error counters to detect bus contention.

### Phase 2 — Add PCA9685
All three I2C devices active. Sweeps a servo while reading sensors.
Requires 6V external supply connected to PCA9685 V+ terminal.

### Phase 3 — Add TFT Display
SPI + I2C running simultaneously. Sensor data rendered on screen.
Validates the GPIO 2/16 and GPIO 18/27 pin reassignments.

### Phase 4 — Add GPIO Sensors
IR break-beam, LDR, and proximity sensor added. Tests that
additional analogRead/digitalRead calls don't disrupt timing.

### Phase 5 — Add Load Cell + HX711
Two modes:
- `MODE_STANDALONE` — calibrate the load cell in isolation first.
- `MODE_INTEGRATED` — add HX711 to the full stack.

### Phase 6 — Full Pipeline
State machine implementing the complete VERIDIS workflow:
IDLE → WEIGH → SCAN → CLASSIFY → SORT → DISPLAY → COOLDOWN.
Includes a simple weighted-vote classifier. Tune thresholds
with your real bottles.


## Testing Methodology

For each phase:

1. Upload the sketch and open Serial Monitor at 115200 baud.
2. Verify all components initialise without errors.
3. Exercise each sensor (wave hand in front of IR, block LDR,
   hold trained object in front of HuskyLens, place weight on
   load cell, etc.).
4. Let it run for 10–15 minutes and watch error counters.
5. If error counters climb, investigate before proceeding.
6. Only move to the next phase when the current one is stable.

For Phase 6, test with actual plastic and glass bottles and
record the classification scores to tune your thresholds.
