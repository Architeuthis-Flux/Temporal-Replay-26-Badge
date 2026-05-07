#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeInput.h"
// BadgeInput.h — Public interface for the BadgeInput module
// See FR-005 in spec.md
//
// Contains: Button struct + buttons[], pollButtons, pollJoystick, pollTilt,
//           tiltFadeTransition.
//           Joystick position state (joySquareX/Y), tilt state flags.
//
// Called exclusively from Core 1 (loop()).

#pragma once
#include <Arduino.h>

// ─── Button ──────────────────────────────────────────────────────────────────

struct Button {
    uint8_t       pin;
    bool          lastReading;
    bool          state;
    unsigned long lastDebounceTime;
    int           indicatorX;
    int           indicatorY;
};

extern Button buttons[];
extern const int NUM_BUTTONS;

// ─── Tilt state (read by BadgeDisplay) ───────────────────────────────────────

extern bool tiltState;
extern bool tiltNametagActive;

// ─── Joystick position (read by BadgeDisplay) ────────────────────────────────

extern int joySquareX;
extern int joySquareY;

// ─── Flags set by button presses, consumed by loop() ─────────────────────────

// BTN_RIGHT: re-check backend pairing / refresh badge data
extern volatile bool pairingCheckRequested;

// BTN_DOWN on "Apps" menu item: show Python app sub-menu
extern volatile bool pythonMenuRequested;

// spec-009: BTN_DOWN on "Messages" menu item: open Messages screen
extern volatile bool messagesRequested;


// ─── Polling functions (Core 1 only) ─────────────────────────────────────────

void pollButtons();
void pollJoystick();
void pollTilt();

// Block until the given pin goes HIGH (button released). Safe to call from
// blocking menu loops. Uses 5 ms polling to avoid busy-wait.
void waitButtonRelease(int pin, int delayMs = 5);

