# Implementation Plan: Firmware Second-Pass Modularization

**Branch**: `005-switch-button-ui` | **Date**: 2026-03-12 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/005-switch-button-ui/spec.md`

## Summary

Pure refactor of `firmware/Firmware-0308-modular/`. No behavioral change. Three new source files are created (`BadgeMenu.h/.cpp`, `BadgePairing.h/.cpp`), the joystick/tilt tuning constants migrate to `BadgeConfig.h`, and `submitPairing()` is decomposed into focused helpers. The main `.ino` sketch is reduced to `setup()` + `loop()` + global state declarations. All changes validated by `./build.sh` success and full manual hardware verification.

## Technical Context

**Language/Version**: Arduino C++ (ESP32 Arduino core 3.x)
**Primary Dependencies**: U8G2, IRremote, ArduinoJson 6.x, Preferences, HTTPClient, WiFi (ESP32 Arduino core)
**Storage**: ESP32 NVS via `Preferences` library (unchanged)
**Testing**: Manual hardware verification per checklist
**Target Platform**: ESP32-S3-MINI-1 (XIAO form factor), 128×64 SSD1309 OLED
**Project Type**: Embedded firmware
**Performance Goals**: Identical to pre-refactor — display refresh ≤ 50 ms, IR RX jitter unchanged
**Constraints**: No OTA; binary must fit in 4 MB app partition; no new heap allocations
**Scale/Scope**: Single firmware binary, ~7 source modules after refactor

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Arduino C++ First | ✅ PASS | All new code is Arduino C++. No new runtimes. |
| II. Firmware-0308 Behavioral Parity | ✅ PASS | Pure refactor — zero intentional behavior change. Boot sequence, IR protocol, state machine, dual-core assignment, modal UI, tilt, NVS all preserved. |
| III. Credentials-at-Build | ✅ PASS | New constants added to `BadgeConfig.h.example`. `BadgeConfig.h` stays gitignored. |
| IV. Backend Contract Compliance | ✅ PASS | No HTTP endpoint changes. `BadgePairing` calls existing `BadgeAPI` functions only. |
| V. Reproducible Build | ✅ PASS | `./build.sh` is the validation gate. No toolchain changes. |
| VI. Hardware Safety | ✅ PASS | No GPIO, eFuse, or peripheral changes. Pin constants remain in `BadgeConfig.h`. |
| VII. API Contract Library | ✅ PASS | `BadgeAPI` interface unchanged. `BadgePairing` calls `BadgeAPI::createBoop`, `getBoopStatus`, `cancelBoop`. |

**Complexity Tracking**: No violations.

## Project Structure

### Documentation (this feature)

```text
specs/005-switch-button-ui/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output — module map and data flow
├── quickstart.md        # Phase 1 output — manual verification guide
├── contracts/           # Phase 1 output — module public interfaces
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (after refactor)

```text
firmware/Firmware-0308-modular/
├── Firmware-0308-modular.ino   # setup() + loop() + extern global state only
├── BadgeConfig.h               # (gitignored) — gains 6 tuning constants
├── BadgeConfig.h.example       # gains 6 tuning constants with comments
│
├── BadgeMenu.h                 # NEW — menu public interface
├── BadgeMenu.cpp               # NEW — renderMenu(), menuIndex, MENU_ITEMS, joystick nav
│
├── BadgePairing.h              # NEW — WiFi/pairing flow public interface
├── BadgePairing.cpp            # NEW — wifiConnect(), osRun(), osUnpairedFlow(), osPairedFlow(), rePair()
│
├── BadgeDisplay.h              # unchanged interface
├── BadgeDisplay.cpp            # renderMenu() call → BadgeMenu; menu state removed
│
├── BadgeInput.h                # unchanged interface
├── BadgeInput.cpp              # menuIndex/MENU_COUNT via BadgeMenu.h; joystick nav constants from BadgeConfig.h
│
├── BadgeIR.h                   # unchanged interface
├── BadgeIR.cpp                 # submitPairing() decomposed into private helpers
│
├── BadgeAPI.h/.cpp             # unchanged
├── BadgeStorage.h/.cpp         # unchanged
├── BadgeUID.h/.cpp             # unchanged
├── graphics.h                  # unchanged
│
├── build.sh / flash.sh / setup.sh  # unchanged
└── build/                      # build artifacts (gitignored)
```

**Structure Decision**: Extends existing single-module pattern. Each new `.h/.cpp` pair follows the established convention: `.h` declares `extern` globals and function signatures; `.cpp` defines them.
