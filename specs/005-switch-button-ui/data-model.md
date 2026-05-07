# Data Model: Firmware Second-Pass Modularization — Module Map

**Branch**: `005-switch-button-ui` | **Date**: 2026-03-12

This is a firmware refactor. There is no traditional data model. This document maps the module structure, shared state ownership, and data flow after the refactor.

---

## Module Ownership Map

| Module | Owns (globals defined here) | Externs from |
|--------|----------------------------|--------------|
| `Firmware-0308-modular.ino` | `badgeState`, `assignedRole`, `qrBits`, `badgeBits`, `qrByteCount`, `badgeByteCount`, `nvsWasPaired` | — |
| `BadgeMenu.cpp` | `menuIndex`, `MENU_ITEMS[]`, `menuNavLocked` | `BadgeDisplay.h` (`renderMode`, `screenDirty`, `u8g2`) |
| `BadgePairing.cpp` | `wifiConnect()`, `osRun()`, `rePair()` | `.ino` globals via extern; `BadgeAPI.h`, `BadgeDisplay.h`, `BadgeStorage.h`, `BadgeMenu.h` |
| `BadgeDisplay.cpp` | `u8g2`, `displayMutex`, `screenLine1/2`, `screenDirty`, `modalActive`, `renderMode` | `BadgeIR.h` (irStatus), `BadgeInput.h` (buttons, tiltState), `BadgeMenu.h` (renderMenu), `.ino` (qrBits, badgeBits, badgeState, assignedRole) |
| `BadgeInput.cpp` | `buttons[]`, `tiltState`, `pairingCheckRequested`, `joySquareX/Y` | `BadgeDisplay.h` (renderMode, screenDirty), `BadgeIR.h` (irStatus, flags), `BadgeMenu.h` (menuIndex, MENU_COUNT) |
| `BadgeIR.cpp` | `irStatus`, `boopListening`, `irPairingRequested`, `pairingCancelRequested` | `BadgeDisplay.h` (screenDirty, showModal), `BadgeAPI.h`, `BadgeUID.h` (uid, uid_hex) |
| `BadgeAPI.cpp` | HTTP result structs | `BadgeConfig.h` |
| `BadgeStorage.cpp` | NVS read/write | `Preferences` library |
| `BadgeUID.cpp` | `uid[]`, `uid_hex[]` | ESP32 eFuse API |

---

## Shared State: .ino Global Declarations

These globals are defined in the main sketch and accessed via `extern` by any module that needs them. This is the established pattern in this codebase.

| Variable | Type | Owner (.ino) | Readers |
|----------|------|-------------|---------|
| `badgeState` | `BadgeState` enum | .ino | BadgeDisplay.cpp, BadgePairing.cpp |
| `assignedRole` | `int` | .ino | BadgeDisplay.cpp, BadgePairing.cpp, BadgeIR.cpp |
| `qrBits` | `uint8_t*` | .ino | BadgeDisplay.cpp, BadgePairing.cpp |
| `badgeBits` | `uint8_t*` | .ino | BadgeDisplay.cpp, BadgePairing.cpp |
| `qrByteCount` | `int` | .ino | BadgePairing.cpp |
| `badgeByteCount` | `int` | .ino | BadgePairing.cpp |
| `nvsWasPaired` | `bool` | .ino | BadgePairing.cpp |

---

## BadgeConfig.h: New Constants (this refactor)

Six constants move from `BadgeInput.cpp` static locals to `BadgeConfig.h`:

| Constant | Type | Default | Used by |
|----------|------|---------|---------|
| `JOY_DEADBAND` | `float` | 0.08f | `BadgeInput.cpp` |
| `MENU_NAV_THRESHOLD` | `float` | 0.5f | `BadgeMenu.cpp` |
| `JOY_CIRCLE_R` | `int` | 6 | `BadgeInput.cpp` |
| `JOY_CIRCLE_CX` | `int` | 100 | `BadgeInput.cpp` |
| `JOY_CIRCLE_CY` | `int` | 53 | `BadgeInput.cpp` |
| `TILT_HOLD_MS` | `unsigned long` | 1500 | `BadgeInput.cpp` |

---

## Data Flow: Menu Navigation

```
pollJoystick() [BadgeInput.cpp]
  reads rawX/rawY from ADC
  normalizes to nx/ny
  if renderMode==MODE_MENU and |ny| > MENU_NAV_THRESHOLD:
    writes menuIndex [BadgeMenu.cpp global]
    sets screenDirty=true

renderScreen() [BadgeDisplay.cpp]
  case MODE_MENU → renderMenu() [BadgeMenu.cpp]
    reads menuIndex, MENU_ITEMS[], MENU_COUNT
    draws inverted-box highlight + item strings
```

---

## Data Flow: Pairing

```
onButtonPressed(BTN_DOWN, menuIndex==WiFi/Pair) [BadgeInput.cpp]
  sets pairingCheckRequested=true

loop() [Firmware-0308-modular.ino]
  if pairingCheckRequested:
    calls rePair() [BadgePairing.cpp]
      wifiConnect() if needed
      osUnpairedFlow() or osPairedFlow()
      renderMode=MODE_MENU, renderScreen()
```

---

## submitPairing() Decomposition

```
submitPairing(theirUID, theirAddr) [BadgeIR.cpp — public]
  ↓ calls pairingPost(theirUID, &boopResult)  [private helper]
      → BadgeAPI::createBoop()
      → if confirmed: irSetPhase(IR_PAIRED_OK), return
      → if no workflowId: irSetPhase(IR_PAIR_FAILED), return
  ↓ calls pairingPollLoop(workflowId, theirUID, theirAddr)  [private helper]
      → 30s loop: BadgeAPI::getBoopStatus()
      → if pairingCancelRequested: calls pairingCancel(theirUID), return
      → if confirmed: irSetPhase(IR_PAIRED_OK), return
      → if timeout: irSetPhase(IR_PAIR_FAILED)
  pairingCancel(theirUID)  [private helper]
      → BadgeAPI::cancelBoop()
      → irSetPhase(IR_PAIR_CANCELLED)
```

---

## No Persistence Changes

This refactor does not change any NVS keys, read/write patterns, or the NVS namespace layout. `BadgeStorage` is unchanged.

---

## (Archived) Prior data model for button-hint widget

### Entities

### ButtonHintLabels

A 4-element array of C-string pointers, one per button in order `[UP, DOWN, LEFT, RIGHT]`, matching the `buttons[]` index order in `BadgeInput.cpp`.

| Field | Type | Constraints |
|-------|------|-------------|
| `label[0]` — UP | `const char*` | Static lifetime; single char recommended; `""` = inactive |
| `label[1]` — DOWN | `const char*` | Same |
| `label[2]` — LEFT | `const char*` | Same |
| `label[3]` — RIGHT | `const char*` | Same |

**Lifecycle**: Module-level static in `BadgeDisplay.cpp`. Initialized to defaults at boot. Updated by `setButtonHints()` on screen-context transitions. Not persisted.

**Validation**: No runtime validation. Labels longer than one character render but overflow the circle boundary — callers are responsible for passing single-character strings.

---

### ButtonHintPositions (compile-time constant)

Circle center coordinates and radius, embedded in `BadgeDisplay.cpp` as `const int` arrays.

| Field | Value | Source |
|-------|-------|--------|
| `cx[0]` — UP center X    | 118 | Former `buttons[0].indicatorX` |
| `cx[1]` — DOWN center X  | 118 | Former `buttons[1].indicatorX` |
| `cx[2]` — LEFT center X  | 113 | Former `buttons[2].indicatorX` |
| `cx[3]` — RIGHT center X | 123 | Former `buttons[3].indicatorX` |
| `cy[0]` — UP center Y    | 48  | Former `buttons[0].indicatorY` |
| `cy[1]` — DOWN center Y  | 58  | Former `buttons[1].indicatorY` |
| `cy[2]` — LEFT center Y  | 53  | Former `buttons[2].indicatorY` |
| `cy[3]` — RIGHT center Y | 53  | Former `buttons[3].indicatorY` |
| `r` — circle radius      | 4   | Derived: 8px diameter fits 4x6 font |

These values do not change at runtime. They replace the `indicatorX`/`indicatorY` fields removed from the `Button` struct.

---

### Button (modified)

Existing struct in `BadgeInput.h`. Fields `indicatorX` and `indicatorY` are removed; all other fields unchanged.

| Field | Type | Notes |
|-------|------|-------|
| `pin` | `uint8_t` | GPIO pin number |
| `lastReading` | `bool` | Previous raw reading for debounce |
| `state` | `bool` | Current debounced state (`LOW` = pressed) |
| `lastDebounceTime` | `unsigned long` | Debounce timestamp |
| ~~`indicatorX`~~ | ~~`int`~~ | Removed — moved to `BadgeDisplay.cpp` |
| ~~`indicatorY`~~ | ~~`int`~~ | Removed — moved to `BadgeDisplay.cpp` |

---

## State Transitions

```
Button physical state    →   Button.state    →   Circle render
─────────────────────────────────────────────────────────────
Not pressed              →   HIGH            →   drawCircle (outlined) + label
Pressed and held         →   LOW             →   drawDisc (filled) + inverse label
Released                 →   HIGH            →   drawCircle (outlined) + label
```

Transition triggers `screenDirty = true` via `pollButtons()` → `renderScreen()` on next loop iteration.

---

## No Persistence

The widget state is entirely ephemeral:
- Circle positions: compile-time constants
- Label strings: runtime pointers to string literals, reset to defaults at boot
- Button press state: live from `buttons[i].state` each frame

No NVS reads or writes. No WiFi or HTTP interaction.
