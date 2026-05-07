# Research: Modularize Arduino Badge Firmware

## Q1 — Arduino multi-file sketch conventions

**Decision**: Use standard Arduino `.h`/`.cpp` convention — all module files in the sketch
directory alongside the `.ino` file.

**Rationale**: The Arduino IDE and `arduino-cli` automatically compile every `.cpp` in the
sketch directory. `#include "BadgeConfig.h"` works without any build-system changes. This is
the only supported Arduino multi-file pattern.

**Alternatives considered**:
- Arduino libraries (`libraries/BadgeAPI/`): requires `arduino-cli lib install` or manual
  symlinking. Adds friction for a repo-local refactor. Rejected.
- PlatformIO `lib/` layout: different build system, not what this repo uses. Rejected.

---

## Q2 — Shared mutable state across modules in Arduino C++

**Decision**: Use extern globals declared in module headers, defined in module `.cpp` files.
Volatile cross-core flags (`boopListening`, `irPairingRequested`, `pairingCancelRequested`)
defined in `BadgeIR.cpp`, declared `extern volatile bool` in `BadgeIR.h`.

**Why not accessor functions**: Arduino embedded convention favors direct extern access for
performance-sensitive inter-task state. Accessor functions add indirection for state that
Core 0 and Core 1 access in tight loops. Not warranted here.

**Ownership mapping** (which module defines each global):

| Global | Defined in | Why |
|--------|-----------|-----|
| `uid[]`, `uid_hex[]` | BadgeUID.cpp | UID module owns eFuse data |
| `irStatus` | BadgeIR.cpp | IR module owns phase state machine |
| `boopListening`, `irPairingRequested`, `pairingCancelRequested` | BadgeIR.cpp | IR module owns inter-core communication |
| `modalActive` | BadgeDisplay.cpp | Display module owns modal gate |
| `screenDirty` | BadgeDisplay.cpp | Display module owns dirty flag |
| `tiltState`, `tiltNametagActive`, `tiltHoldPending` | BadgeInput.cpp | Input module owns tilt state |
| `joySquareX`, `joySquareY` | BadgeInput.cpp | Input module owns joystick state |
| `buttons[]` | BadgeInput.cpp | Input module owns button array |
| `badgeState` | Firmware-0308-modular.ino | Main sketch owns state machine |
| `assignedRole` | Firmware-0308-modular.ino | Main sketch owns role (set by BadgeAPI result) |
| `qrBits`, `badgeBits`, `qrByteCount`, `badgeByteCount` | Firmware-0308-modular.ino | Main sketch owns XBM buffers (allocation via BadgeAPI) |

**Cross-module includes**:
- `BadgeDisplay.h` includes `BadgeIR.h` (for `IrPhase`, `IrStatus` in renderMain)
- `BadgeDisplay.h` includes `BadgeInput.h` (for `Button`, tilt/joystick externs in renderMain)
- `BadgeIR.h` includes `BadgeDisplay.h` (for `showModal` called from irTask)
- `BadgeInput.h` includes `BadgeDisplay.h` (for `renderScreen` called from tiltFadeTransition)
- **Circular risk**: BadgeDisplay ↔ BadgeInput ↔ BadgeDisplay; resolved by forward-declaring
  `renderScreen()` in BadgeInput.h instead of including BadgeDisplay.h. `renderScreen` is
  declared extern and defined in BadgeDisplay.cpp.

---

## Q3 — FreeRTOS stack budget for extracted BadgeAPI HTTP functions

**Decision**: `BadgeAPI` functions run within the existing 8 KB Core 0 stack. No stack
increase required. `submitPairing` in `BadgeIR.cpp` becomes a thin orchestrator that calls
`BadgeAPI::createBoop`, `BadgeAPI::getBoopStatus`, `BadgeAPI::cancelBoop` — the HTTP
work is the same; only the call site changes.

**Analysis**:
- `HTTPClient` + `DynamicJsonDocument(512)` in `submitPairing` currently fits in 8 KB.
- The extracted `BadgeAPI::createBoop` uses the same `HTTPClient` + `DynamicJsonDocument(512)`.
- Calling convention: one `BadgeAPI` function active at a time; stack depth is unchanged.
- `BadgeAPI::fetchBadgeXBM` uses `DynamicJsonDocument(8192)` — currently called from
  `setup()` / `osUnpairedFlow()` on the main task (Core 1), which has the default Arduino
  stack (~8 KB). No change.

**Alternatives considered**: Increasing irTask stack to 12 KB — unnecessary. Rejected.

---

## Q4 — `showModal` dependency from BadgeIR on BadgeDisplay

**Decision**: `BadgeIR.cpp` includes `BadgeDisplay.h` and calls `showModal()` directly.
Dependency direction: BadgeIR → BadgeDisplay. This is a one-way dependency; BadgeDisplay
does not include BadgeIR.h — instead it uses extern declarations for `IrStatus` and
`IrPhase` to avoid a header include cycle.

**Rationale from spec**: "showModal() belongs in the display module even though the IR
module calls it." (Assumptions section). This is the clean split — modal is a display
concern; IR is a caller.

---

## Q5 — QR buffer ownership across modules

**Decision**:
- `BadgeAPI::fetchQR(uid, &outBuf, &outLen)` allocates via `malloc` and returns pointer
  in `outBuf`. Caller (main sketch) owns the buffer and is responsible for `free()`.
- Same for `BadgeAPI::fetchBadgeXBM(uid, &outBuf, &outLen, &outRole)`.
- `BadgeStorage::nvsSaveQR(bits, len)` takes the buffer by const pointer and copies to NVS;
  it does not take ownership.
- This matches the existing behavior (main sketch holds `qrBits` and `badgeBits` globally).

---

## Q6 — `renderMain` reads from many modules — how to avoid tight coupling

**Decision**: `renderMain()` in BadgeDisplay.cpp accesses extern globals from BadgeIR,
BadgeInput, and the main sketch (`badgeState`, `assignedRole`, `qrBits`, `badgeBits`).
This is unavoidable for a render function that composites the full screen. The coupling is
one-directional: BadgeDisplay reads state owned by other modules but never writes to it.

**Pattern**: `BadgeDisplay.h` declares all required externs explicitly at the top of the
file. This makes dependencies explicit and auditable.

---

## Q7 — `renderQR` reads `SERVER_URL` and `EP_QR` for status line

**Decision**: `BadgeDisplay.cpp` includes `BadgeConfig.h` directly and uses `SERVER_URL`
and `EP_QR` in `renderQR()`. This is a display rendering constant — acceptable for a config
include. Alternative (passing URL as parameter) adds unnecessary indirection.

---

## Summary of Key Decisions

| Decision | Chosen | Alternative Rejected |
|----------|--------|---------------------|
| File layout | .h/.cpp in sketch dir | Arduino library, PlatformIO |
| Shared state | extern globals | Accessor functions |
| showModal dependency | BadgeIR → BadgeDisplay (one-way) | Callback/event system |
| Buffer ownership | Caller owns (malloc in API, free in main) | Module-owned buffers |
| Cross-core volatiles | extern volatile in BadgeIR.h | Atomic types (not needed for byte-wide bools on ESP32) |
| renderMain coupling | extern globals, one-directional reads | Parameter passing (too verbose) |
