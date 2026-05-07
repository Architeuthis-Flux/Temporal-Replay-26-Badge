// BadgeInput.h — Public interface for the BadgeInput module
// See FR-005 in spec.md
//
// Contains: Button struct + buttons[], pollButtons, pollJoystick, pollTilt,
//           tiltFadeTransition, onButtonPressed.
//           Joystick position state (joySquareX/Y), tilt state flags.
//
// Called exclusively from Core 1 (loop()).
// onButtonPressed() sets volatile flags in BadgeIR (irPairingRequested, boopListening,
// pairingCancelRequested) and calls irSetPhase() — the only BadgeIR calls from Core 1.

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

// ─── Tilt state (read by BadgeDisplay::renderMain) ───────────────────────────

extern bool tiltState;
extern bool tiltNametagActive;
extern bool tiltHoldPending;

// ─── Joystick position (read by BadgeDisplay::renderMain) ────────────────────

extern int joySquareX;
extern int joySquareY;

// ─── Polling functions (Core 1 only) ─────────────────────────────────────────

void pollButtons();
void pollJoystick();
void pollTilt();

// ─── Action dispatch ──────────────────────────────────────────────────────────

// Called from pollButtons() on button press. Dispatches to BadgeIR flags.
void onButtonPressed(int index);
