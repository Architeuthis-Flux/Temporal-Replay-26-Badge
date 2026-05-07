# Tasks: Badge Core Primitives — TCP-IR Boop Protocol

**Input**: Design documents from `/specs/008-badge-core-primitives/`
**Spec**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md)

**Scope** (spec.md Session 2026-03-19 scope refinement):
- US3: Full TCP-IR protocol rewrite (replaces broken PING-burst approach — T002/T003 jitter fix proven structurally insufficient by hardware testing)
- US2: Tilt-to-nametag menu-only guard (single fix)
- US4: `badge.ping()` Python primitive
- US1: Firmware already complete; verification blocked on backend dep
- US5: README docs partially done (T009/T010 in prior pass); verify TCP-IR sections are current

**Out of scope**: Backend enrollment endpoint, QR boot flow firmware changes, tilt debounce changes (250ms is intentional).

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no unresolved depend encies)
- **[Story]**: US2=tilt, US3=IR boop, US4=Python ping, US5=docs

## Path Conventions

Base path: `firmware/Firmware-0308-modular/`

---

## Phase 1: Setup

**Purpose**: Confirm build baseline before making any changes

- [X] T001 Run `./build.sh -n` from `firmware/Firmware-0308-modular/` — record exit code and pre-existing warnings; do not proceed if baseline is broken

---

## Phase 2: Foundational

No foundational phase required — existing build infrastructure and module boundaries are stable. Proceed directly to user story phases.

---

## Phase 3: User Story 1 — QR Pairing End-to-End (Priority: P1)

**Goal**: Firmware already complete (Decision 3). Verification requires live backend enrollment endpoint (`POST /api/v1/badges/enroll` or equivalent).

**Independent Test**: Factory-reset badge powers on, shows QR, backend enrollment triggered via `curl`, badge transitions to paired nametag view.

**⚠️ NOTE**: This phase is blocked on backend work (out of scope for this spec). Resume when backend enrollment endpoint is confirmed reachable.

- [X] T002 [US1] Verify QR→enrollment→paired flow end-to-end: factory-reset badge (clear NVS), power on, observe QR on screen, trigger enrollment against live backend, confirm badge transitions to paired nametag — no firmware changes expected; this is an integration smoke test

---

## Phase 4: User Story 2 — Nametag Tilt Menu-Only Guard (Priority: P2)

**Goal**: Tilt-to-nametag must not activate outside the menu screen (FR-004). Currently `pollTilt()` has no `renderMode` check, allowing it to fire during input test and QR modes.

**Independent Test**: On a paired badge, verify nametag activates with 1500ms tilt on menu screen. Switch to input test screen, tilt — nametag must NOT appear.

- [X] T003 [US2] In `BadgeInput.cpp` `pollTilt()`: add guard `if (renderMode != MODE_MENU) { tiltNametagActive = false; return; }` at the top of the function — prevents nametag activation outside the menu screen per FR-004 (documented Principle II deviation per research.md Decision 2)

**Checkpoint**: Tilt guard complete. Compile with T011 build gate.

---

## Phase 5: User Story 3 — IR Badge-to-Badge Boop TCP-IR Rewrite (Priority: P2) 🎯

**Goal**: Replace the broken simultaneous-TX PING protocol with a TCP-inspired half-duplex exchange over NEC IR. Root cause confirmed by hardware: structural collision, not timing (research.md Decision 8). Full irTask() rewrite required.

**Independent Test**: Two flashed badges on Boop screen. One presses UP (momentary). Both show live exchange status. Both reach result screen within 15 seconds (SC-003). Any button dismisses result and returns to menu.

### BadgeIR.h — Types, Constants, Struct

- [X] T004 [US3] In `BadgeIR.h`: replace existing `IrPhase` enum with TCP-IR phases: `IR_IDLE, IR_SYN_SENT, IR_SYN_RECEIVED, IR_ESTABLISHED, IR_TX_UID, IR_RX_UID, IR_PAIR_CONSENT, IR_PAIRED_OK, IR_PAIR_FAILED, IR_PAIR_CANCELLED`; add `IrRole` enum (`IR_ROLE_NONE, IR_INITIATOR, IR_RESPONDER`); add packet constants (`IR_SYN=0xC0, IR_SYN_ACK=0xC1, IR_ACK=0xC2, IR_NACK=0xC4, IR_FIN=0xC5, IR_RST=0xC6, IR_DATA_BASE=0xD0`); add timing constants (`IR_SYN_RETRY_MS=300, IR_SYN_MAX_RETRIES=10, IR_SYNACK_TIMEOUT_MS=400, IR_SYNACK_MAX_RETRIES=5, IR_ACK_TIMEOUT_MS=250, IR_ACK_MAX_RETRIES=3, IR_DATA_TIMEOUT_MS=1500`); update `IrStatus` struct to add `role (IrRole)`, `statusMsg[48]`, `peerUidHex[13]`, `my_cookie (uint8_t)`, `peer_cookie (uint8_t)`; declare `extern volatile bool irHardwareEnabled` and `extern volatile bool irExchangeActive` in `firmware/Firmware-0308-modular/BadgeIR.h`

### BadgeIR.cpp — irTask() Full Rewrite

- [X] T005 [US3] In `BadgeIR.cpp` `irTask()`: implement `IR_IDLE` state — on `irHardwareEnabled` rising edge call `IrReceiver.begin(IR_RX_PIN)` + `IrSender.begin(IR_TX_PIN)` + reset to `IR_IDLE`; on `irHardwareEnabled` falling edge call `IrReceiver.end()` + power-off sender + reset all state; in `IR_IDLE` listen for incoming NEC frames — on SYN received (`addr==IR_SYN, cmd!=0`) store peer cookie, send SYN_ACK with own cookie, transition to `IR_SYN_RECEIVED`; on `boopEngaged` edge (cleared after consuming) generate random cookie 1–255, send SYN frame, set retry count to 0, transition to `IR_SYN_SENT` in `firmware/Firmware-0308-modular/BadgeIR.cpp`

- [X] T006 [US3] In `BadgeIR.cpp` `irTask()`: implement `IR_SYN_SENT` state — listen for SYN_ACK (`addr==IR_SYN_ACK`): on receipt, store peer cookie, send ACK(`seq=0`), determine role (lower cookie = INITIATOR; tie → lower `uid_hex[0]`), set `irExchangeActive=true`, transition to `IR_ESTABLISHED`; also handle simultaneous SYN (recv SYN while in SYN_SENT): send SYN_ACK, stay in SYN_SENT waiting for peer SYN_ACK; on `BTN_RIGHT` while in SYN_SENT: send RST, transition to `IR_PAIR_CANCELLED`; on retry timeout (`IR_SYN_RETRY_MS`) retransmit SYN up to `IR_SYN_MAX_RETRIES`; on exhausted retries transition to `IR_PAIR_FAILED`; implement `IR_SYN_RECEIVED` state — wait for ACK from initiator; on recv ACK: determine role, set `irExchangeActive=true`, transition to `IR_ESTABLISHED`; on retransmit SYN_ACK up to `IR_SYNACK_MAX_RETRIES`; on exhausted retries → `IR_PAIR_FAILED` in `firmware/Firmware-0308-modular/BadgeIR.cpp`

- [X] T007 [US3] In `BadgeIR.cpp` `irTask()`: implement `IR_ESTABLISHED` — immediately branch: `IR_INITIATOR → IR_TX_UID (seq=0)`, `IR_RESPONDER → IR_RX_UID`; implement `IR_TX_UID` — send `DATA(IR_DATA_BASE|seq, uid_bytes[seq])`, start ACK timer; on ACK received for current seq: advance seq; if seq==6 transition to `IR_RX_UID` (INITIATOR) or `IR_PAIR_CONSENT` (RESPONDER); on timeout retransmit up to `IR_ACK_MAX_RETRIES`; on NACK retransmit immediately; on retries exhausted: send RST → `IR_PAIR_FAILED`; implement `IR_RX_UID` — wait for DATA frames; on `DATA(0xD0|seq, byte)` with correct seq: store byte, send ACK, advance seq; on duplicate (same seq): resend ACK; if seq==6 transition to `IR_PAIR_CONSENT`; on `IR_DATA_TIMEOUT_MS` elapsed without frame: send RST → `IR_PAIR_FAILED`; on RST received in any state → `IR_PAIR_FAILED` in `firmware/Firmware-0308-modular/BadgeIR.cpp`

- [X] T008 [US3] In `BadgeIR.cpp` `irTask()`: implement `IR_PAIR_CONSENT` — call `BadgeAPI::createBoop(uid_hex, peerUidHex)` synchronously; on success (`r.ok`) store `partnerName`, transition to `IR_PAIRED_OK`; on failure transition to `IR_PAIR_FAILED`; in `IR_PAIRED_OK` / `IR_PAIR_FAILED` / `IR_PAIR_CANCELLED` states: set `boopTaskDone = true`, `irExchangeActive = false`; these are terminal — irTask waits here until `irHardwareEnabled` goes false or mode changes; update `irStatus.statusMsg` and `irStatus.peerUidHex` throughout all state transitions for display in `firmware/Firmware-0308-modular/BadgeIR.cpp`

### BadgeInput.cpp + .ino — Edge Trigger + IR Lifecycle Signal

- [X] T009 [P] [US3] In `BadgeInput.cpp` `onButtonPressed(case 0)` (BTN_UP): change from continuous held-state assignment to edge trigger — add `if (renderMode == MODE_MAIN && !boopTaskInFlight && !irExchangeActive) { boopEngaged = true; }` instead of the current `boopEngaged = (buttons[0].state == LOW) && !boopTaskInFlight;` pattern; `boopEngaged` is consumed and cleared by `irTask()` after one SYN fire in `firmware/Firmware-0308-modular/BadgeInput.cpp`

- [X] T010 [P] [US3] In `Firmware-0308-modular.ino`: declare `volatile bool irHardwareEnabled = false;` and `volatile bool irExchangeActive = false;` at file scope; in the `renderMode` transition logic add `irHardwareEnabled = (renderMode == MODE_MAIN);` whenever `renderMode` is assigned — so entering Boop screen enables IR and leaving it disables IR; also add `irExchangeActive` declaration to `BadgeIR.h` extern already added in T004 in `firmware/Firmware-0308-modular/Firmware-0308-modular.ino`

### BadgeDisplay.cpp — Live TCP-IR Status

- [X] T011 [US3] In `BadgeDisplay.cpp` `renderMain()`: update the `irStatus.phase` switch to cover all TCP-IR phases — `IR_IDLE`: "Ready to boop"; `IR_SYN_SENT`: "Seeking..." + second line `irStatus.statusMsg`; `IR_SYN_RECEIVED`: "Connecting..." + `irStatus.statusMsg`; `IR_ESTABLISHED`: "Connected" + role string ("INIT"/"RESP"); `IR_TX_UID`: "Sending UID..." + `irStatus.statusMsg`; `IR_RX_UID`: "Receiving..." + partial `irStatus.peerUidHex` filling in; `IR_PAIR_CONSENT`: "Booping..." + `irStatus.peerUidHex`; `IR_PAIRED_OK/FAILED/CANCELLED`: delegate to existing `renderBoopResult()` (already handles all three per T005 in prior pass); remove any cases for deleted phases (`IR_WAITING`, `IR_INCOMING`, `IR_NO_REPLY`, `IR_PAIR_IGNORED` — already removed per T006/T007 in prior pass) in `firmware/Firmware-0308-modular/BadgeDisplay.cpp`

### Build Gate (US3)

- [X] T012 [US3] Run `./build.sh -n` from `firmware/Firmware-0308-modular/` — must exit 0 with no new warnings (SC-007 gate for US2 + US3 changes)

**Checkpoint**: IR boop TCP-IR protocol complete. Validate on two physical badges per SC-003 (15-second window from UP press to result screen).

---

## Phase 6: User Story 4 — Python `badge.ping()` Primitive (Priority: P3)

**Goal**: Expose `badge.ping()` in the MicroPython `badge` module. Follows established bridge pattern (research.md Decision 6, contracts/badge-c-abi.md).

**Independent Test**: Upload a Python script that calls `badge.ping()` to badge VFS, launch from Apps menu, confirm it returns True with WiFi and False without crashing when offline (SC-004: result in <3s).

### Bridge Declaration + Implementation

- [X] T013 [P] [US4] In `BadgePython_bridges.h`: add declaration `extern "C" bool BadgePython_probe_backend(void);` in `firmware/Firmware-0308-modular/badge_mp/BadgePython_bridges.h`

- [X] T014 [P] [US4] In `BadgePython_bridges.cpp`: implement `bool BadgePython_probe_backend() { extern char uid_hex[]; BadgeAPI::ProbeResult r = BadgeAPI::probeBadgeExistence(uid_hex); return r.ok; }` in `firmware/Firmware-0308-modular/BadgePython_bridges.cpp`

### MicroPython C Binding + Module Registration

- [X] T015 [US4] In `badge_mp/badge_http_mp.c`: add `STATIC mp_obj_t badge_ping_fn(void) { return mp_obj_new_bool(BadgePython_probe_backend()); }` and `STATIC MP_DEFINE_CONST_FUN_OBJ_0(badge_ping_fn_obj, badge_ping_fn);` — file already contains other HTTP bridge functions; follow the existing `badge_http_get_fn` pattern in `firmware/Firmware-0308-modular/badge_mp/badge_http_mp.c`

- [X] T016 [US4] In `badge_mp/badge_module.c`: add `{ MP_ROM_QSTR(MP_QSTR_ping), MP_ROM_PTR(&badge_ping_fn_obj) },` to the module globals table — follow the pattern of existing entries in `firmware/Firmware-0308-modular/badge_mp/badge_module.c`

**Checkpoint**: `badge.ping()` registered and reachable from Python. Verify with T018 build gate before hardware test.

---

## Phase 7: US3 Hardware Verification (Manual)

**Purpose**: Confirm TCP-IR boop works on physical hardware before proceeding to US4. T012 build gate passes but IR correctness can only be verified on-device.

**Required hardware**: Two flashed badges, both running the T004–T011 firmware.

- [X] T017 [US3] Flash both badges with `./flash.py`, navigate both to Boop screen, press UP on one badge (momentary) — verify: initiating badge shows "Seeking..." with retry count updating; passive badge auto-transitions from "Ready to boop" to live exchange status without any button press
- [X] T018 [US3] Complete the handshake: verify both badges show "Connected" with role ("INIT"/"RESP"); observe UID exchange phase transitions ("Sending UID..." / "Receiving...") and partial peer UID filling in character-by-character on the receiving badge
- [X] T019 [US3] Verify result screen: both badges reach result screen within 15 seconds (SC-003); success case shows partner name; any button dismisses and returns to menu
- [X] T020 [US3] Verify cancel path: press BTN_RIGHT on initiating badge before handshake completes — badge shows "Boop cancelled" result screen; verify that pressing BTN_RIGHT after handshake is established has no effect until the result screen
- [X] T021 [US3] Verify failure path: initiate boop with no peer badge present — initiating badge exhausts SYN retries (~3s) and shows failure result screen; no hang or reset required

**Checkpoint**: IR boop verified on hardware. Proceed to US4 and US5.

---

## Phase 8: User Story 5 — Documentation Accuracy (Priority: P3)

**Goal**: README files must accurately describe the TCP-IR protocol and all current `badge` primitives. Prior pass (T009/T010) updated docs for the old PING approach; those sections need updating for TCP-IR.

**Independent Test**: SC-005 — developer follows firmware README quickstart and flashes successfully. SC-006 — developer writes a working `badge.ping()` app using only apps/README.md.

- [X] T022 [P] [US5] Read `firmware/Firmware-0308-modular/README.md` IR pairing section — update to describe TCP-IR: momentary UP press initiates SYN; three-way handshake; live status display; 15-second window; BTN_RIGHT cancels pre-handshake only; replace any description of the old PING/hold-to-boop/listen-window approach in `firmware/Firmware-0308-modular/README.md`
- [X] T023 [P] [US5] Read `firmware/Firmware-0308-modular/apps/README.md` — confirm `badge.ping()` section documents the implemented bridge behavior (returns `True` on HTTP 200, `False` on any failure, blocks up to 8s); confirm `badge.ir_start()` / `badge.ir_stop()` / `badge.ir_available()` / `badge.ir_read()` are documented as v1 limitations (stubs — IR frames not queued to Python); update anything stale in `firmware/Firmware-0308-modular/apps/README.md`

---

## Phase 9: Polish & Final Gate

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — start immediately
- **US1 (Phase 3)**: Blocked on backend enrollment endpoint — defer
- **US2 (Phase 4)**: Independent of US3/US4 after T001; shares BadgeInput.cpp with T009 — sequence T003 before T009
- **US3 (Phase 5)**: T003 (US2) must be done first if editing BadgeInput.cpp together; otherwise independent
  - T004 first (defines all types used by T005–T011)
  - T005 → T006 → T007 → T008 sequentially (same function, irTask())
  - T009 and T010 parallel (different files: BadgeInput.cpp vs .ino)
  - T011 after T004 (needs new phase constants)
  - T012 after T003–T011 (build gate)
- **US3 hardware verification (Phase 7)**: T017–T021 after T012; must all pass before US4 starts
- **US4 (Phase 6)**: Starts after T021; T013 and T014 parallel; T015 after T013+T014; T016 after T015
- **US5 docs (Phase 8)**: T022 and T023 parallel; can run alongside US4 after T021
- **Final gate (Phase 9)**: T024 after T016 + T022 + T023

### User Story Dependencies

- **US2 (P2)**: Independent — single fix in BadgeInput.cpp
- **US3 (P2)**: Independent of US2/US4; largest phase; T004 blocks all US3 tasks
- **US4 (P3)**: Independent — can run in parallel with US3 if desired
- **US1 (P1)**: Blocked — backend dep; do not delay US3/US4 waiting for it

### Parallel Opportunities

```
After T001 (baseline):
  ├── T003 (US2, BadgeInput.cpp tilt guard)
  └── T004 (US3, BadgeIR.h types)
        ├── T005 → T006 → T007 → T008 (irTask() rewrite, sequential)
        ├── T009 (BadgeInput.cpp edge trigger — after T003)
        ├── T010 (Firmware.ino IR lifecycle)
        └── T011 (BadgeDisplay.cpp — after T004)

After T012 (US3 build gate):
  ├── T017–T021 (hardware verification — must pass before US4)
  └── [US4 starts after T021]
        ├── T013 (bridges.h declare)
        ├── T014 (bridges.cpp implement)
        └── T022+T023 (doc updates, parallel)
            T015 after T013+T014
            T016 after T015
            T024 final gate after T016+T022+T023
```

---

## Implementation Strategy

### MVP (US3 — IR boop working)

1. T001 (baseline)
2. T003 (US2 tilt guard — quick, same file touch as T009)
3. T004–T011 (TCP-IR rewrite)
4. T012 (build gate)
5. T017–T021 (hardware verification — two badges required)
6. **STOP**: IR boop confirmed on hardware (SC-003)

### Full Delivery

7. T013–T016 (badge.ping())
8. T022–T023 (doc updates)
9. T024 (final build gate)
10. T002 (US1 smoke test — when backend ready)

---

## Notes

- T005–T008 are a single conceptual unit (irTask() rewrite) split for reviewability — implement in order without committing between them
- T003 and T009 both touch BadgeInput.cpp: do T003 first, then T009
- `./build.sh -n` at T012 (US3 build gate) and T024 (final gate); SC-003/SC-004 require physical hardware
- Hardware verification (T017–T021) is a hard gate before US4 — do not start badge.ping() until IR boop is confirmed working on device
- Prior pass completed: T006/T007 (dead IrPhase cleanup), T009 (firmware README), T010 (apps/README ping docs), T011 (flash.sh refs) — do not redo
- `badge.ping()` docs already in apps/README.md (prior T010); T013–T016 is the C implementation that makes them accurate
