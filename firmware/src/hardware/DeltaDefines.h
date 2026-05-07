#ifndef DELTA_DEFINES_H
#define DELTA_DEFINES_H

// Temporal Badge — DELTA revision
// ESP32-S3-MINI-1, XIAO form factor
//
// To add a new hardware target:
//   1. Copy this file, rename it (e.g. MyBoardDefines.h)
//   2. Update the pin numbers below to match your schematic
//   3. Add BADGE_HAS_* flags for any optional peripherals your board has
//      (see CharlieDefines.h for the full list of available flags)
//   4. Add a new [env:myboard] section to platformio.ini with -DHARDWARE_MYBOARD
//   5. Add your board to HardwareConfig.h

#define JOY_X 1
#define JOY_Y 2

#define IR_TX_PIN 3
#define IR_RX_PIN 4

#define SDA_PIN 5
#define SCL_PIN 6

#define BUTTON_DOWN  7
#define BUTTON_LEFT  8
#define BUTTON_RIGHT 9

#define TILT_PIN 43
#define BUTTON_UP    44

#define OLED_I2C_ADDRESS 0x3C
#define OLED_WIDTH 128
#define OLED_HEIGHT 64

#define BADGE_DELTA

#endif
