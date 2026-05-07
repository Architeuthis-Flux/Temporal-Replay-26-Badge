# Feature Specification: Firmware Second-Pass Modularization

**Feature Branch**: `005-switch-button-ui`
**Created**: 2026-03-12
**Status**: Draft
**Input**: Audit of `Firmware-0308-modular` after initial modularization (004). Seven coupling and cohesion issues identified for resolution.

## Background

The 004 modularization split the original monolith into `BadgeDisplay`, `BadgeInput`, `BadgeIR`, `BadgeStorage`, `BadgeAPI`, and `BadgeUID`. The menu system was subsequently added directly to `BadgeDisplay` and `BadgeInput`. A follow-up audit identified seven areas where modules are still too large or remain tightly coupled to unrelated concerns. This spec captures that second-pass cleanup.

## User Scenarios & Testing *(mandatory)*

### User Story 1 — Firmware Builds and Behaves Identically After Refactor (Priority: P0)

A developer pulls the refactored branch, builds with `./build.sh`, and flashes to hardware. Every badge function that worked before still works: menu navigation, QR display, boop/IR pairing, WiFi pair/re-pair, tilt nametag, and joystick interaction.

**Why this priority**: This is a pure refactor. No behavioral change is intended. The only acceptable outcome is identical runtime behavior with a cleaner source structure.

**Independent Test**: Flash the pre-refactor binary, exercise all features, record behavior. Flash the post-refactor binary, exercise the same features, confirm identical behavior.

**Acceptance Scenarios**:

1. **Given** the badge is freshly flashed with the refactored firmware, **When** it boots, **Then** it connects to WiFi, runs the pairing or refresh flow, and lands on the menu — identical to pre-refactor behavior.
2. **Given** the badge is on the menu, **When** the joystick is moved up/down, **Then** the selection highlight moves — identical debounce and wrap-around behavior as before.
3. **Given** the badge is on the menu and "Boop" is selected, **When** BTN_DOWN is pressed, **Then** the IR boop flow initiates and the badge enters `MODE_MAIN` with `IR_LISTENING`.
4. **Given** the badge is on the menu and "WiFi / Pair" is selected, **When** BTN_DOWN is pressed, **Then** the WiFi reconnect + pairing flow runs identically to before.
5. **Given** the badge is tilted while on the main screen with `badgeBits` loaded, **When** held for 1.5 s, **Then** the nametag XBM displays full-screen, flipped 180°.

---

### User Story 2 — Menu Module is Self-Contained (Priority: P1)

A developer wanting to add a new menu item or restyle the menu opens `BadgeMenu.h` and `BadgeMenu.cpp` and makes the change without touching `BadgeDisplay` or `BadgeInput`.

**Acceptance Scenarios**:

1. **Given** `BadgeMenu.cpp` exists, **When** a developer adds a fourth item to `MENU_ITEMS[]` and increments `MENU_COUNT`, **Then** the menu renders the new item and joystick navigation wraps around it — no other files need editing.
2. **Given** `BadgeMenu.h` is the public interface, **When** `BadgeDisplay.cpp` needs to render the menu, **Then** it calls `renderMenu()` from `BadgeMenu.h` only — no menu state is accessed directly.

---

### User Story 3 — WiFi / Pairing Logic is Self-Contained (Priority: P1)

A developer fixing a WiFi reconnect bug opens `BadgePairing.cpp` and finds all pairing and WiFi flow code there, not scattered across the main `.ino` file.

**Acceptance Scenarios**:

1. **Given** `BadgePairing.h` exists, **When** the main sketch calls `pairingInit()` during `setup()` and `osRun()` during runtime, **Then** all WiFi connect, QR fetch, badge XBM fetch, and association poll logic executes from `BadgePairing.cpp`.
2. **Given** `rePair()` is called from `loop()`, **Then** it is implemented in `BadgePairing.cpp`, not the main `.ino`.

---

### Edge Cases

- After extracting `BadgeMenu`, `BadgeInput` must still be able to set `menuIndex` and read `MENU_COUNT` via the `BadgeMenu` public interface.
- After extracting `BadgePairing`, the main `.ino` `setup()` must still call NVS load before `osRun()` — boot order must not change.
- Moving joystick constants to `BadgeConfig.h` must not break the `BadgeConfig.h.example` — example file must include the new constants with commented defaults.
- `submitPairing()` refactor must not change the observable IR protocol or timing visible to another badge.

## Requirements *(mandatory)*

### Functional Requirements

**Menu extraction**
- **FR-001**: A new module `BadgeMenu.h` / `BadgeMenu.cpp` MUST own `renderMenu()`, `menuIndex`, `MENU_ITEMS[]`, `MENU_COUNT`, and joystick menu-navigation logic (threshold detection, `menuNavLocked`).
- **FR-002**: `BadgeDisplay.cpp` MUST call `renderMenu()` via `BadgeMenu.h` include; it MUST NOT contain menu item strings or navigation state.
- **FR-003**: `BadgeInput.cpp` MUST access `menuIndex` and `MENU_COUNT` via `BadgeMenu.h` include; it MUST NOT duplicate those declarations.

**Pairing / WiFi extraction**
- **FR-004**: A new module `BadgePairing.h` / `BadgePairing.cpp` MUST own `wifiConnect()`, `osUnpairedFlow()`, `osPairedFlow()`, `rePair()`, and `osRun()`.
- **FR-005**: The main `.ino` sketch MUST be reduced to `setup()` and `loop()` plus the global state declarations (`badgeState`, `assignedRole`, `qrBits`, `badgeBits`, `nvsWasPaired`).
- **FR-006**: `BadgePairing` MUST access shared global state (`qrBits`, `badgeBits`, `assignedRole`, `badgeState`, `nvsWasPaired`) via `extern` declarations, consistent with the existing module pattern.

**Joystick / tilt constants to config**
- **FR-007**: `JOY_DEADBAND`, `MENU_NAV_THRESHOLD`, `JOY_CIRCLE_R`, `JOY_CIRCLE_CX`, `JOY_CIRCLE_CY`, and `TILT_HOLD_MS` MUST be defined in `BadgeConfig.h` (and `BadgeConfig.h.example`) rather than as `static const` values buried in `.cpp` files.
- **FR-008**: `BadgeInput.cpp` and any other consumers MUST read these constants from `BadgeConfig.h`; local duplicates MUST be removed.

**submitPairing() decomposition**
- **FR-009**: `submitPairing()` in `BadgeIR.cpp` MUST be split into named helper functions, each with a single responsibility (e.g., send UID packet, wait for ACK, call API, handle result), so the top-level function reads as a clear state-machine and individual steps can be read or modified independently.
- **FR-010**: The public signature of `submitPairing()` and the observable IR protocol behavior MUST remain unchanged.

### Non-Functional Requirements

- **NFR-001**: `./build.sh` MUST succeed with zero errors after all changes.
- **NFR-002**: No new RAM or flash usage beyond what modular `.cpp` files inherently cost (function call overhead, no redundant buffers).
- **NFR-003**: All moved code MUST compile without warnings at the existing `-Wall` level.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: `./build.sh` produces a valid binary with zero errors.
- **SC-002**: All features verified on hardware: menu nav, boop IR, QR display, WiFi / re-pair, tilt nametag.
- **SC-003**: `BadgeDisplay.cpp` contains no menu item strings and no `menuNavLocked` variable.
- **SC-004**: The main `.ino` sketch contains only `setup()`, `loop()`, global state declarations, and includes — no WiFi or HTTP logic.
- **SC-005**: `BadgeConfig.h.example` includes all six tuning constants with comments.
- **SC-006**: `submitPairing()` body is ≤ 30 lines; helper functions cover the remainder.

## Assumptions

- The existing module boundary pattern (`.h` declares extern globals and function signatures; `.cpp` defines them) is the correct pattern to follow for new modules.
- `BadgePairing` will `#include` `BadgeAPI.h`, `BadgeDisplay.h`, `BadgeStorage.h`, and `BadgeConfig.h` — all already in the project.
- `BadgeMenu` will `#include` `BadgeDisplay.h` (for `u8g2`, `renderMode`, `screenDirty`) and `BadgeConfig.h`.
- No new FreeRTOS tasks or mutexes are required.
- Hardware pin assignments and NVS keys are not changing.
