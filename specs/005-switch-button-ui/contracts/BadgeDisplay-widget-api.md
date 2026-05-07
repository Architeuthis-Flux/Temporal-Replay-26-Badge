# Contract: New Module Public Interfaces

**Feature**: 005-switch-button-ui (firmware modularization)

---

## BadgeMenu.h — New Module

```cpp
#pragma once
#include <Arduino.h>

// ─── Menu state (extern globals) ──────────────────────────────────────────────
#define MENU_COUNT 3
extern int menuIndex;

// ─── Public functions ─────────────────────────────────────────────────────────

// Render the menu screen. Called by BadgeDisplay::renderScreen() for MODE_MENU.
// Acquires/releases display mutex internally via the caller (renderScreen holds it).
void renderMenu();

// Process joystick Y axis for menu navigation. Call from BadgeInput::pollJoystick()
// when renderMode == MODE_MENU.
// ny: normalized joystick Y value (-1.0 to +1.0, deadband already applied)
// Sets menuIndex and screenDirty as needed.
void menuHandleJoystick(float ny);
```

**Owns**: `menuIndex` (int global), `MENU_ITEMS[]` (static const), `menuNavLocked` (static local), `MENU_NAV_THRESHOLD` (from BadgeConfig.h), `MENU_COUNT` (define).

**Dependencies**: `BadgeDisplay.h` (u8g2, renderMode, screenDirty), `BadgeConfig.h` (MENU_NAV_THRESHOLD).

---

## BadgePairing.h — New Module

```cpp
#pragma once
#include <Arduino.h>

// ─── Boot-time entry point ────────────────────────────────────────────────────

// Full startup sequence: connect WiFi, fetch QR/badge assets, set renderMode=MODE_MENU.
// Call from setup() after NVS load and IR task creation.
void osRun();

// ─── Runtime re-pair (called from loop() when pairingCheckRequested) ──────────

// Reconnect WiFi if needed, then refresh badge or re-run association poll.
// Sets renderMode=MODE_MENU on completion.
void rePair();
```

**Owns**: `wifiConnect()` (static), `osConnectWiFi()` (static), `osUnpairedFlow()` (static), `osPairedFlow()` (static).

**Accesses via extern**: `badgeState`, `assignedRole`, `qrBits`, `qrByteCount`, `badgeBits`, `badgeByteCount`, `nvsWasPaired` (all defined in main .ino).

**Dependencies**: `BadgeAPI.h`, `BadgeDisplay.h`, `BadgeStorage.h`, `BadgeConfig.h`, `BadgeMenu.h`.

---

## BadgeConfig.h — New Constants

Six tuning constants added (also in `BadgeConfig.h.example`):

```cpp
// ─── Joystick tuning ──────────────────────────────────────────────────────────
const float JOY_DEADBAND        = 0.08f;  // normalized units; below this → 0
const float MENU_NAV_THRESHOLD  = 0.5f;   // normalized units; above this → nav step
const int   JOY_CIRCLE_R        = 6;      // joystick indicator circle radius (px)
const int   JOY_CIRCLE_CX       = 100;    // joystick indicator center X (px)
const int   JOY_CIRCLE_CY       = 53;     // joystick indicator center Y (px)

// ─── Tilt tuning ──────────────────────────────────────────────────────────────
const unsigned long TILT_HOLD_MS = 1500;  // hold duration to activate nametag view
```

---

## BadgeIR.cpp — submitPairing() Decomposition (internal only)

The public `submitPairing(const char* theirUID, uint8_t theirAddr)` signature is **unchanged**. Three private (file-scope, not exported) helpers are extracted:

```cpp
// POST /api/v1/boops. Returns false if failed (phase already set).
// On immediate confirm: sets IR_PAIRED_OK phase and returns false (done).
// On pending: populates workflowId and returns true (proceed to poll).
static bool pairingPost(const char* theirUID, uint8_t theirAddr, String& workflowId);

// 30-second poll loop. Handles cancel, confirm, timeout.
// theirUID/theirAddr used for peer display and cancel call.
static void pairingPollLoop(const String& workflowId,
                            const char* theirUID, uint8_t theirAddr);

// Send DELETE /api/v1/boops/pending and set IR_PAIR_CANCELLED phase.
static void pairingCancel(const char* theirUID);
```

**Invariants**:
- Protocol behavior (NEC bytes, address encoding, timing) is unchanged.
- `irSetPhase()` and `irSetDebug()` calls produce identical serial output and phase transitions as before.
- No new network endpoints or HTTP methods.
