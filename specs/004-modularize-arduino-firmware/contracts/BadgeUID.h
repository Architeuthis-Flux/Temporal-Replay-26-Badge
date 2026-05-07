// BadgeUID.h — Public interface for the BadgeUID module
// See FR-006 in spec.md
//
// Reads the ESP32-S3 eFuse OPTIONAL_UNIQUE_ID (16 bytes) and converts the first
// 6 bytes to a 12-character hex string.
//
// HALT behavior: read_uid() displays an error on the OLED and enters an infinite loop
// if the eFuse read fails. It must be called before any other module runs.

#pragma once
#include <Arduino.h>

#define UID_SIZE 16

extern uint8_t uid[UID_SIZE];
extern char    uid_hex[UID_SIZE * 2 + 1];

// Read eFuse UID. Halts with display error if read fails.
// Must be called in setup() before u8g2.begin() for the error display to work;
// u8g2 is initialized inside read_uid() on the error path.
void read_uid();

// Convert first 6 bytes of uid[] to uid_hex[]. Null-terminates at [12].
void uid_to_hex();
