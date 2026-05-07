---

description: "Task list for Firmware Second-Pass Modularization"
---

# Tasks: Firmware Second-Pass Modularization

**Input**: Design documents from `/specs/005-switch-button-ui/`
**Prerequisites**: plan.md ✓, spec.md ✓, research.md ✓, data-model.md ✓, contracts/ ✓, quickstart.md ✓

**Organization**: Tasks are grouped by user story. US1 = BadgeMenu extraction. US2 = BadgePairing extraction. US3 = submitPairing decomposition. All are pure refactors — no behavior change.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: US1 = BadgeMenu, US2 = BadgePairing, US3 = submitPairing
- Exact file paths are included in each description

---

## Phase 1: Setup

**Purpose**: Verify baseline build and migrate tuning constants to `BadgeConfig.h` — foundational for all module extractions.

- [x] T001 Verify baseline build: run `./build.sh` in `firmware/Firmware-0308-modular/` and confirm `build/Firmware-0308-modular.ino.bin` is produced with zero errors
- [x] T002 Add 6 tuning constants to `firmware/Firmware-0308-modular/BadgeConfig.h`: `JOY_DEADBAND` (0.08f), `MENU_NAV_THRESHOLD` (0.5f), `JOY_CIRCLE_R` (6), `JOY_CIRCLE_CX` (100), `JOY_CIRCLE_CY` (53), `TILT_HOLD_MS` (1500UL) — add under a new `// ─── Joystick / tilt tuning` section
- [x] T003 Add the same 6 constants to `firmware/Firmware-0308-modular/BadgeConfig.h.example` with comments explaining each one
- [x] T004 Update `firmware/Firmware-0308-modular/BadgeInput.cpp`: remove the duplicate `static const` declarations for all 6 constants (JOY_DEADBAND, MENU_NAV_THRESHOLD, JOY_CIRCLE_R, JOY_CIRCLE_CX, JOY_CIRCLE_CY, TILT_HOLD_MS); ensure `BadgeConfig.h` is already included at the top (it is via BadgeInput.h → BadgeConfig.h)
- [x] T005 Build verification: run `./build.sh` — confirm zero errors before proceeding to module extractions

---

## Phase 2: User Story 1 — BadgeMenu Module (Priority: P1)

**Goal**: All menu rendering and joystick navigation logic lives in `BadgeMenu.h` / `BadgeMenu.cpp`. A developer editing menu items or nav behavior touches only those files.

**Independent Test**: Flash firmware, navigate menu with joystick, confirm all three items render, selection highlight moves, BTN_DOWN executes item — identical to pre-refactor behavior.

### Implementation

- [x] T006 [US1] Create `firmware/Firmware-0308-modular/BadgeMenu.h`: `#pragma once`, include guards, `#include <Arduino.h>`, `#define MENU_COUNT 3`, `extern int menuIndex`, declare `void renderMenu()`, declare `void menuHandleJoystick(float ny)`
- [x] T007 [US1] Create `firmware/Firmware-0308-modular/BadgeMenu.cpp`: define `int menuIndex = 0`, define `static const char* MENU_ITEMS[MENU_COUNT] = { "Boop", "QR Code", "WiFi / Pair" }`, define `static bool menuNavLocked = false` — implement `renderMenu()` by cutting the full `renderMenu()` body from `BadgeDisplay.cpp` (including the setDisplayFlip call, box highlight loop, footer text), implement `menuHandleJoystick(float ny)` by extracting the `if (renderMode == MODE_MENU)` block from `pollJoystick()` in `BadgeInput.cpp` — include `BadgeConfig.h` (for MENU_NAV_THRESHOLD), `BadgeDisplay.h` (for u8g2, renderMode, screenDirty), `BadgeMenu.h`
- [x] T008 [US1] Update `firmware/Firmware-0308-modular/BadgeDisplay.h`: remove `#define MENU_COUNT 3`, remove `extern int menuIndex`, remove `void renderMenu()` declaration — these are now in `BadgeMenu.h`; add `#include "BadgeMenu.h"` so consumers of `BadgeDisplay.h` can still access `menuIndex` and `MENU_COUNT` transitively, or update each consumer directly
- [x] T009 [US1] Update `firmware/Firmware-0308-modular/BadgeDisplay.cpp`: add `#include "BadgeMenu.h"` at top, remove the `int menuIndex = 0`, `MENU_ITEMS[]`, and full `renderMenu()` implementation, keep `case MODE_MENU: renderMenu(); break` in `renderScreen()` (now calls into `BadgeMenu.cpp`)
- [x] T010 [US1] Update `firmware/Firmware-0308-modular/BadgeInput.cpp`: add `#include "BadgeMenu.h"` at top, remove the `if (renderMode == MODE_MENU)` joystick nav block from `pollJoystick()`, replace with a single call `menuHandleJoystick(ny)` (or call it only when `renderMode == MODE_MENU` — match original conditional), remove now-unused `MENU_NAV_THRESHOLD` and `menuNavLocked` local declarations (moved to BadgeMenu.cpp)
- [x] T011 [US1] Build verification: run `./build.sh` — confirm zero errors

**Checkpoint**: BadgeMenu is self-contained. Firmware behavior is identical.

---

## Phase 3: User Story 2 — BadgePairing Module (Priority: P1)

**Goal**: All WiFi connect, QR/badge fetch, association poll, and re-pair logic lives in `BadgePairing.h` / `BadgePairing.cpp`. The main `.ino` sketch is reduced to `setup()` + `loop()` + global state declarations.

**Independent Test**: Flash firmware, power on — full boot sequence runs (WiFi → pairing flow → menu). Select "WiFi / Pair" from menu → re-pair flow runs. All identical to pre-refactor.

### Implementation

- [x] T012 [US2] Create `firmware/Firmware-0308-modular/BadgePairing.h`: `#pragma once`, `#include <Arduino.h>`, declare `void osRun()`, declare `void rePair()` — these are the only two functions the main sketch calls
- [x] T013 [US2] Create `firmware/Firmware-0308-modular/BadgePairing.cpp`: includes for `BadgeConfig.h`, `BadgeDisplay.h`, `BadgeStorage.h`, `BadgeAPI.h`, `BadgeMenu.h`, `<WiFi.h>`, `<HTTPClient.h>` — declare `extern BadgeState badgeState`, `extern int assignedRole`, `extern uint8_t* qrBits`, `extern int qrByteCount`, `extern uint8_t* badgeBits`, `extern int badgeByteCount`, `extern bool nvsWasPaired` — implement `static bool wifiConnect()` by cutting from `.ino`, implement `static void osConnectWiFi()` by cutting from `.ino`, implement `static void osUnpairedFlow()` by cutting from `.ino`, implement `static void osPairedFlow()` by cutting from `.ino`, implement `void rePair()` by cutting from `.ino`, implement `void osRun()` by cutting from `.ino`
- [x] T014 [US2] Update `firmware/Firmware-0308-modular/Firmware-0308-modular.ino`: remove `wifiConnect()`, `osConnectWiFi()`, `osUnpairedFlow()`, `osPairedFlow()`, `rePair()`, `osRun()` — add `#include "BadgePairing.h"` — verify `setup()` still calls `osRun()` and `loop()` still calls `rePair()` when `pairingCheckRequested` is set — retain all global state declarations (`badgeState`, `assignedRole`, `qrBits`, `badgeBits`, `qrByteCount`, `badgeByteCount`, `nvsWasPaired`) and remove the `#include <WiFi.h>` and `#include <HTTPClient.h>` includes from the .ino if they are no longer used there
- [x] T015 [US2] Build verification: run `./build.sh` — confirm zero errors

**Checkpoint**: BadgePairing is self-contained. Main .ino is now ~80 lines. Firmware behavior is identical.

---

## Phase 4: User Story 3 — submitPairing() Decomposition (Priority: P1)

**Goal**: `submitPairing()` in `BadgeIR.cpp` is ≤ 30 lines; three private helpers cover the POST, poll loop, and cancel paths. Duplicate cancel-check blocks are eliminated.

**Independent Test**: Two badges boop each other — full pairing flow completes (or is cancelled by button press) identically to pre-refactor. Serial log shows identical phase transitions.

### Implementation

- [x] T016 [US3] In `firmware/Firmware-0308-modular/BadgeIR.cpp`: extract a file-scope (static) helper `static void pairingCancel(const char* theirUID)` — body: set debugMsg "cancelling...", call `BadgeAPI::cancelBoop(uid_hex, theirUID)`, log the HTTP code, call `irSetPhase(IR_PAIR_CANCELLED, 2000)` — this consolidates the two duplicate cancel blocks in the current poll loop
- [x] T017 [US3] In `firmware/Firmware-0308-modular/BadgeIR.cpp`: extract a file-scope helper `static bool pairingPost(const char* theirUID, uint8_t theirAddr, String& workflowId)` — body: check WiFi, call `BadgeAPI::createBoop`, handle HTTP error (set IR_PAIR_FAILED, return false), handle immediate-confirm path (set IR_PAIRED_OK + irSetPeer, return false), populate `workflowId` for pending path (return true); the function sets irStatus phase and debug internally
- [x] T018 [US3] In `firmware/Firmware-0308-modular/BadgeIR.cpp`: extract a file-scope helper `static void pairingPollLoop(const String& workflowId, const char* theirUID, uint8_t theirAddr)` — body: the full 30-second while loop including `pairingCancelRequested` checks (call `pairingCancel()` when set), `BadgeAPI::getBoopStatus()` call, confirmed/not_requested/pending handling, and timeout handling; remove the duplicate cancel-check block that existed before the `vTaskDelay`
- [x] T019 [US3] In `firmware/Firmware-0308-modular/BadgeIR.cpp`: rewrite `submitPairing()` as a dispatcher: call `pairingPost(theirUID, theirAddr, workflowId)` — if it returns false, return; otherwise call `pairingPollLoop(workflowId, theirUID, theirAddr)` — total body should be ≤ 15 lines
- [x] T020 [US3] Build verification: run `./build.sh` — confirm zero errors

**Checkpoint**: submitPairing() is readable as a state-machine dispatcher. Behavior is identical.

---

## Phase 5: Polish & Validation

**Purpose**: Final build, flash, and full manual hardware verification.

- [x] T021 [P] Review `firmware/Firmware-0308-modular/BadgeDisplay.h` — confirm `MENU_COUNT`, `menuIndex`, and `renderMenu()` are no longer declared there (now in BadgeMenu.h); confirm the header is still minimal and correct
- [x] T022 [P] Review `firmware/Firmware-0308-modular/Firmware-0308-modular.ino` — confirm it contains only: includes, global state declarations (`badgeState`, `assignedRole`, `qrBits`, `badgeBits`, `qrByteCount`, `badgeByteCount`, `nvsWasPaired`), `setup()`, and `loop()` — no WiFi or HTTP logic
- [ ] T023 Flash firmware and run full manual verification per `specs/005-switch-button-ui/quickstart.md`: boot sequence, menu navigation, all three menu actions, BTN_UP/RIGHT back-to-menu, tilt nametag, joystick dot on main screen, IR boop flow, serial log sanity checks

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 (Setup)**: No dependencies — start immediately
- **Phase 2 (US1 BadgeMenu)**: Depends on Phase 1 completion (constants must be in BadgeConfig.h before BadgeMenu.cpp can compile)
- **Phase 3 (US2 BadgePairing)**: Depends on Phase 2 completion (BadgePairing.cpp includes BadgeMenu.h)
- **Phase 4 (US3 submitPairing)**: Independent — can start after Phase 1 in parallel with US1/US2
- **Phase 5 (Polish)**: Depends on Phases 2, 3, 4 all complete

### User Story Dependencies

- **US1 (BadgeMenu)**: Depends on constants migration (Phase 1 T002–T004)
- **US2 (BadgePairing)**: Depends on US1 complete (BadgePairing.cpp includes BadgeMenu.h)
- **US3 (submitPairing)**: Independent of US1 and US2 — operates only on BadgeIR.cpp

### Parallel Opportunities

- T002 and T003 (BadgeConfig changes) can run in parallel — different files
- T006 (BadgeMenu.h) and T007 (BadgeMenu.cpp) are sequential (header before impl)
- T012 (BadgePairing.h) and T013 (BadgePairing.cpp) are sequential
- T016–T019 (submitPairing helpers) are sequential within BadgeIR.cpp
- T021 and T022 (final reviews) can run in parallel

---

## Parallel Example: Phase 1 + Phase 4

```
Phase 1 T002-T003 (BadgeConfig.h and .example) can run together:
  Task: "Add 6 constants to BadgeConfig.h"
  Task: "Add 6 constants to BadgeConfig.h.example"

After Phase 1 complete, US3 is independent and can start while US1/US2 proceed:
  Track A: T006 → T007 → T008 → T009 → T010 → T011 (US1)
  Track B: T016 → T017 → T018 → T019 → T020     (US3)
  Then:    T012 → T013 → T014 → T015             (US2, after US1)
```

---

## Implementation Strategy

### MVP: Phase 1 + Phase 2 (US1) only

1. Migrate constants → T001–T005
2. Extract BadgeMenu → T006–T011
3. Build succeeds → demo self-contained menu module

### Full Delivery

1. Phase 1: Constants migration
2. Phase 2: BadgeMenu (US1)
3. Phase 4: submitPairing decomposition (US3) — can overlap with US1/US2
4. Phase 3: BadgePairing (US2)
5. Phase 5: Polish and hardware verify

---

## Notes

- This is a pure refactor. Every task is a cut-and-paste of existing logic into a new home, not new behavior.
- Run `./build.sh` after every phase — do not accumulate unbuildable states.
- If a build fails mid-refactor, restore the cut code to its original location and debug before re-extracting.
- Manual hardware verification (T023) is the final gate — build success alone is not sufficient.
