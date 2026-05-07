# Implementation Plan: Modularize Arduino Badge Firmware + API SDK

**Branch**: `004-modularize-arduino-firmware` | **Date**: 2026-03-11 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/004-modularize-arduino-firmware/spec.md`

## Summary

Split the 1,626-line `firmware/Firmware-0308/Firmware-0308.ino` monolith into seven focused
Arduino `.h`/`.cpp` modules plus a thin orchestration sketch, without changing any runtime
behavior. Extract the inline HTTP logic from `submitPairing` and `fetchBadgeXBM` into a
typed `BadgeAPI` module. Output goes into a new `firmware/Firmware-0308-modular/` sketch
directory. No new features.

## Technical Context

**Language/Version**: Arduino C++ (ESP32 Arduino core 3.x); targeting ESP32-S3-MINI-1
**Primary Dependencies**: U8G2, IRremote, ArduinoJson 6.x, Preferences (NVS), HTTPClient, WiFi (all from ESP32 Arduino core)
**Storage**: ESP32 NVS via `Preferences` library
**Testing**: Manual flash-and-verify (no automated test runner per constitution); Arduino IDE or `arduino-cli` compile check is the build gate
**Target Platform**: ESP32-S3-MINI-1 (XIAO form factor); FreeRTOS dual-core
**Project Type**: Embedded firmware refactor (Arduino sketch modularization)
**Performance Goals**: Behavior-identical to Firmware-0308; Core 0 IR task runs within 8 KB stack budget
**Constraints**: No new features; behavior parity only; BadgeAPI HTTP functions must fit within 8 KB FreeRTOS stack; `setup()` + `loop()` + boot stages ≤ 150 lines in main `.ino`
**Scale/Scope**: Single sketch directory, ~7 modules, ~1,626 lines split across ~8 files

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

> **Scope note**: The constitution governs the MicroPython firmware in `firmware/micropython-build/`.
> `Firmware-0308/` is the **Arduino reference implementation** — it sits outside the
> constitution's jurisdictional scope. The checks below are applied in advisory capacity
> and flagged where they still apply materially.

| Principle | Status | Notes |
|-----------|--------|-------|
| **I. MicroPython-First** | N/A | This feature modifies the Arduino reference sketch, not the MicroPython firmware. No new C is being added to micropython-build/. |
| **II. Firmware-0306 Parity** | ADVISORY PASS | Firmware-0308 is a documented superset of 0306 (U8G2, dual-core, ArduinoJson, NVS). The spec explicitly states 0308 supersedes 0306. The modular output must be behavior-identical to 0308. |
| **III. Credentials-at-Build** | PASS | `BadgeConfig.h` will contain WiFi credentials. The file must be listed in `.gitignore`. No production credentials committed. |
| **IV. Backend Contract Compliance** | PASS | `BadgeAPI` functions (FR-011–FR-016) align exactly with the documented endpoints. `createBoop`, `getBoopStatus`, `cancelBoop` replace the inline HTTP in `submitPairing`. |
| **V. Reproducible Build** | ADVISORY PASS | Arduino library versions should be noted in a `README.md` alongside the sketch. No `arduino-cli` pinning mechanism exists in this project yet — tracked as a follow-up. |
| **VI. Hardware Safety** | PASS | Pin definitions move to `BadgeConfig.h` verbatim from Firmware-0308. No GPIO changes. No eFuse writes in scope. |

No gate violations. Proceed to Phase 0.

## Project Structure

### Documentation (this feature)

```text
specs/004-modularize-arduino-firmware/
├── plan.md              ← this file
├── research.md          ← Phase 0 output
├── data-model.md        ← Phase 1 output
├── quickstart.md        ← Phase 1 output
├── contracts/
│   ├── BadgeAPI.h       ← Phase 1 output — BadgeAPI public interface
│   ├── BadgeDisplay.h   ← Phase 1 output — Display public interface
│   ├── BadgeIR.h        ← Phase 1 output — IR public interface
│   ├── BadgeInput.h     ← Phase 1 output — Input public interface
│   ├── BadgeStorage.h   ← Phase 1 output — Storage public interface
│   └── BadgeUID.h       ← Phase 1 output — UID public interface
└── tasks.md             ← Phase 2 output (/speckit.tasks — NOT created by /speckit.plan)
```

### Source Code (repository root)

```text
firmware/Firmware-0308-modular/
├── Firmware-0308-modular.ino   # setup(), loop(), osRun + sub-stages, wifiConnect
│                                # Target: ≤ 150 lines
├── BadgeConfig.h               # All deployment constants — WiFi, SERVER_URL, endpoints,
│                                # timeouts, pin defs, BYPASS, THIS_BADGE_ROLE, TILT_SHOWS_BADGE
├── BadgeUID.h                  # uid[], uid_hex[], read_uid(), uid_to_hex()
├── BadgeUID.cpp
├── BadgeDisplay.h              # U8G2 instance, displayMutex, DISPLAY_TAKE/GIVE,
│                                # renderBoot/QR/Main/Screen, renderModal, showModal,
│                                # bootPrint, drawXBM, drawStringCharWrap, setDisplayFlip
├── BadgeDisplay.cpp
├── BadgeStorage.h              # nvsSaveQR, nvsLoadQR, nvsSavePaired, nvsLoadState
├── BadgeStorage.cpp
├── BadgeAPI.h                  # Result structs + public function declarations
├── BadgeAPI.cpp                # createBoop, getBoopStatus, cancelBoop,
│                                # fetchQR, fetchBadgeInfo, fetchBadgeXBM + transport helper
├── BadgeIR.h                   # IrPhase enum, IrStatus struct, irStatus, volatile flags,
│                                # irSetPhase/Peer/Debug, irTask declaration
├── BadgeIR.cpp                 # irTask (Core 0), submitPairing (thin orchestrator)
├── BadgeInput.h                # Button struct, buttons[], pollButtons, pollJoystick,
│                                # pollTilt, tiltFadeTransition, onButtonPressed
├── BadgeInput.cpp
└── graphics.h                  # XBM assets (unchanged from Firmware-0308)
```

**Structure decision**: Single Arduino sketch directory (standard Arduino multi-file
convention). All `.h`/`.cpp` pairs live alongside the `.ino` file. The Arduino IDE and
`arduino-cli` automatically compile all `.cpp` files in the sketch directory.

## Complexity Tracking

No constitution violations requiring justification.
