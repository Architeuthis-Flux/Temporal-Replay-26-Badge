# Tasks: Firmware UI & API Integration

**Input**: Design documents from `/specs/003-firmware-ui-api/`
**Prerequisites**: plan.md ✓, spec.md ✓, research.md ✓, data-model.md ✓, contracts/badge_api.md ✓, quickstart.md ✓

**Tests**: Manual only — no automated test runner on device. Verification steps from quickstart.md.

**Organization**: Tasks grouped by user story for independent implementation and verification.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no blocking dependencies)
- **[Story]**: User story label (US1–US5)

---

## Phase 1: Setup

**Purpose**: Read current source state before making any edits. Zero file modifications.

- [X] T001 Read firmware/micropython-build/boards/TEMPORAL_BADGE_DELTA/_boot.py and confirm the always-attempt-mount fix is present in source (EBUSY treated as success, ENODEV triggers format)
- [X] T002 [P] Read firmware/micropython-build/boot.py and identify all utime.sleep_ms() calls > 100 ms that need loop replacement (per R-001; expect 5 sites)
- [X] T003 [P] Read firmware/micropython-build/flash.sh and locate the --vfs-only branch: the exec VFS mount command, the chained + session, the VFS_FILES list, and where file names are currently printed

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Rebuild firmware to bake in the fixed _boot.py auto-format behavior. Required before US3–US5 can be verified on hardware. Also needed for reliable VFS flashing in US1.

**⚠️ CRITICAL**: US1 hardware verification and all US3–US5 testing require this firmware to be flashed first.

- [ ] T004 Rebuild firmware by running ./build.sh in firmware/micropython-build/; confirm firmware-conference.bin is produced with no build errors (pinned: MicroPython v1.24.0 + ESP-IDF v5.2.3)
- [ ] T005 Full-flash rebuilt firmware by running ./flash.sh in firmware/micropython-build/ (hold B + press RST for bootloader entry; press RST again after esptool completes; confirm REPL accessible via USB-CDC)

**Checkpoint**: Badge running updated firmware — _boot.py auto-format fix is now active.

---

## Phase 3: User Story 1 — Reliable Badge Update Tool (Priority: P1) 🎯 MVP

**Goal**: `./flash.sh --vfs-only` reliably interrupts a running badge mid-boot, copies each file with a post-copy exit-code check, and prints `OK` or `ERROR` per file. badge_api.py included in VFS deploy list.

**Independent Test**: Run `./flash.sh --vfs-only` on a powered-on badge; each filename appears only after confirmed copy; `os.listdir('/')` in REPL shows all files including badge_api.py; completes in < 60 s.

### Implementation for User Story 1

- [X] T006 [US1] Replace all utime.sleep_ms(N) calls > 100 ms in firmware/micropython-build/boot.py with for-loop equivalents using utime.sleep_ms(100) per iteration, preserving total duration (R-001; 5 sites: 2000 ms splash, 1500 ms WiFi connect, QR_DISPLAY_MS=10000 ms, 2000 ms nametag, BYPASS_DELAY_MS=3000 ms)
- [X] T007 [US1] Refactor firmware/micropython-build/flash.sh --vfs-only path: (1) remove the exec "import os,esp32; bdev=..." VFS mount command entirely (R-002); (2) replace chained mpremote + session with a per-file loop calling mpremote connect PORT fs cp SRC :DST individually; (3) check $? after each invocation and print "  -> filename  OK" on success or "  ERROR: filename copy failed (exit code N)" on failure (R-003)
- [X] T008 [P] [US1] Add badge_api.py to the VFS_FILES list in firmware/micropython-build/flash.sh alongside config.py, boot.py, main.py, ir_nec.py, badge_sdk.py, graphics.py, uQR.py (FR-017)

**Checkpoint**: US1 complete — `./flash.sh --vfs-only` < 60 s, per-file OK/ERROR output, `os.listdir('/')` confirms badge_api.py present. (Verification: quickstart.md US1 section)

---

## Phase 4: User Story 2 — Verified Hardware Inputs & Display (Priority: P2)

**Goal**: All physical inputs (4 buttons, joystick, tilt sensor, IR TX/RX) produce correct and immediate OLED visual feedback. draw_base() chrome renders all required elements every frame.

**Independent Test**: Exercise each input on a live badge running main.py (config.BYPASS=True is fine); observe OLED per quickstart.md US2 tables; zero unhandled exceptions over a 5-minute session.

### Implementation for User Story 2

- [X] T009 [P] [US2] Audit and fix button rendering in firmware/micropython-build/main.py: verify active-low logic (3×3 dot drawn when GPIO reads 0, removed when GPIO reads 1); confirm dot positions match quickstart.md table: UP=(118,48) DOWN=(118,58) LEFT=(113,53) RIGHT=(123,53) (FR-005)
- [X] T010 [P] [US2] Audit and fix joystick tracking in firmware/micropython-build/main.py: read ADC values from JOY_X (GPIO1) and JOY_Y (GPIO2); scale to ±6 px offset; constrain dot within 6-pixel radius circle centered at (100, 56); ensure dot never escapes circle boundary (FR-006)
- [X] T011 [US2] Implement 300 ms tilt debounce in firmware/micropython-build/main.py: record utime.ticks_ms() when TILT GPIO 7 changes state; only move display indicator after new state has held for 300 ms; tilted position=(84,48), level position=(84,54) (FR-007)
- [X] T012 [US2] Audit IR TX/RX arrow animations in firmware/micropython-build/main.py: TX arrow (up, filled) appears in header area for ~1 s on outgoing IR transmission; RX arrow (down, filled) appears for ~1 s on valid NEC address 0x42 receive; arrows clear automatically (FR-008)
- [X] T013 [P] [US2] Audit firmware/micropython-build/graphics.py draw_base() function: confirm every call renders TEMPORAL header text, status bar area, joystick circle at (100,56) r=6, and d-pad crosshair at (118,53); fix any missing elements or rendering artifacts (FR-009)

**Checkpoint**: US2 complete — all inputs verified on hardware, all accept criteria from quickstart.md US2 section met, zero crashes. (SC-002)

---

## Phase 5: User Story 3 — WiFi Connection at Startup (Priority: P3)

**Goal**: Badge connects to WiFi using creds.py, syncs NTP clock, shows "Connected to \<SSID\>" on OLED. On failure shows "WiFi failed" and halts without proceeding to server-dependent features.

**Independent Test**: Badge with valid creds.py powers on; OLED shows "Connected to \<SSID\>"; `utime.localtime()` in REPL returns year 2026. Badge with bad creds shows "WiFi failed" and halts.

**Note**: Depends on T006 (interruptible boot sleeps) for reliable iteration during development.

### Implementation for User Story 3

- [X] T014 [US3] Audit firmware/micropython-build/boot.py WiFi connection block: ensure creds module is imported in try/except (missing creds → halt with clear display error); display "Connecting to \<SSID\>..." on OLED before network.connect() is called; display.show() must be called so message is visible during connection attempt (FR-010)
- [X] T015 [US3] Implement NTP sync in firmware/micropython-build/boot.py after successful WiFi connection: call ntptime.settime() inside try/except; on OSError or any NTP failure continue without halting (spec assumption: badge continues with unsynchronized clock) (FR-011)
- [X] T016 [US3] Implement WiFi failure path in firmware/micropython-build/boot.py: on timeout or connection exception display "WiFi failed" on OLED (display.fill(0); display.text("WiFi failed", 0, 28, 1); display.show()); halt execution (return from boot function or raise SystemExit) without proceeding to badge_api calls or nametag display (FR-012)

**Checkpoint**: US3 complete — verified on hardware with valid credentials (clock synced) and bad credentials (halt observed). (SC-003, SC-004)

---

## Phase 6: User Story 4 — Badge-to-Server Communication Client (Priority: P4)

**Goal**: New `badge_api.py` module with single `_request()` transport and 5 named endpoint functions. All connections closed on return. None returned on any error. SERVER_URL sourced from creds module.

**Independent Test**: Deploy badge_api.py via `./flash.sh --vfs-only`; call each function from REPL per quickstart.md US4; verify correct types returned on success, None on 404 and network error, no unhandled exceptions.

**Note**: All tasks edit only firmware/micropython-build/badge_api.py (new file). Can be worked in parallel with US2 and US3.

### Implementation for User Story 4

- [X] T017 [US4] Create firmware/micropython-build/badge_api.py with _request(method, path, body=None) transport function: lazy-import creds to get SERVER_URL; build full URL as SERVER_URL + path; set headers = {"Content-Type": "application/json"}; call urequests.request(method, url, headers=headers, data=ujson.dumps(body) if body else None); parse response in try/finally with r.close() in finally; 2xx status → return r.json(); 404 → return None; other non-2xx → print status and return None; OSError → print exception and return None (contracts/badge_api.md, R-005)
- [X] T018 [US4] Implement get_badge_info(badge_uuid) in firmware/micropython-build/badge_api.py: call _request("GET", "/api/v1/badge/" + badge_uuid + "/info"); returns AttendeeInfo dict {"name", "title", "company", "attendee_type"} or None (data-model.md AttendeeInfo)
- [X] T019 [US4] Implement create_boop(uuid1, uuid2) in firmware/micropython-build/badge_api.py: call _request("POST", "/api/v1/boops", {"badge_uuids": [uuid1, uuid2]}); handle both HTTP 200 (confirmed) and HTTP 202 (pending) as success returning BoopResult dict; return None on any other response (data-model.md BoopResult)
- [X] T020 [US4] Implement get_boop_status(workflow_id, badge_uuid) in firmware/micropython-build/badge_api.py: call _request("GET", "/api/v1/boops/status/" + workflow_id); returns BoopStatus dict {"status": "pending"|"confirmed"|"cancelled"} or None (data-model.md BoopStatus)
- [X] T021 [US4] Implement list_boops(badge_uuid) in firmware/micropython-build/badge_api.py: call _request("GET", "/api/v1/boops?badge_uuid=" + badge_uuid); returns list or [] on empty or None on error (data-model.md BoopList)
- [X] T022 [US4] Implement cancel_pending_boop(badge_uuid) in firmware/micropython-build/badge_api.py: call _request("DELETE", "/api/v1/boops/pending"); returns True on HTTP 200/204 or None on error (data-model.md CancelResult)
- [X] T023 [US4] Add creds-absent guard at top of _request in firmware/micropython-build/badge_api.py: wrap creds import in try/except ImportError; on failure print warning "badge_api: creds not found" and return None immediately; this makes all 5 endpoint functions safe to import in BYPASS mode (contracts/badge_api.md initialization)

**Checkpoint**: US4 complete — all 5 REPL calls produce correct types or None; no unhandled exceptions on 404 or network error. (SC-005)

---

## Phase 7: User Story 5 — Live Attendee Nametag at Startup (Priority: P5)

**Goal**: After WiFi connects, boot.py fetches attendee info via badge_api and displays name/title/company on OLED. 404 → "Not registered" 2 s → main UI. Network error → brief message → main UI.

**Independent Test**: Registered badge shows real name/title/company on OLED. Unregistered badge shows "Not registered" for 2 s then enters main UI. Network error does not crash or hang the badge.

**Note**: Depends on US3 (WiFi connect in boot.py) and US4 (badge_api.py). All edits are in firmware/micropython-build/boot.py.

### Implementation for User Story 5

- [X] T024 [US5] Fix nametag fetch in firmware/micropython-build/boot.py: add `import badge_api` near top of file; replace current _parse_xbm(r.text) call on the /info response (which is wrong — /info returns JSON, not XBM) with `info = badge_api.get_badge_info(badge_uid)` (fixes R-004 bug)
- [X] T025 [US5] Implement nametag display in firmware/micropython-build/boot.py when info is not None: display.fill(0); display.text(str(info.get('name') or '')[:16], 0, 10, 1); display.text(str(info.get('title') or '')[:16], 0, 26, 1); display.text(str(info.get('company') or '')[:16], 0, 42, 1); display.show(); then interruptible sleep loop for nametag display duration (R-006: no draw_base() during nametag, full-screen layout, 16-char truncation enforced here not in badge_api) (FR-020, FR-021)
- [X] T026 [US5] Implement 404 / unregistered path in firmware/micropython-build/boot.py: when badge_api.get_badge_info() returns None, display.fill(0); display.text("Not registered", 0, 28, 1); display.show(); run interruptible sleep loop for 2000 ms; then proceed to main UI without error (FR-022)
- [X] T027 [US5] Wrap the entire nametag fetch block (T024–T026) in firmware/micropython-build/boot.py in try/except Exception as e: print("nametag error:", e); on any unhandled exception log and proceed to main UI — badge must never hang or crash during info fetch (FR-020 edge case, spec edge cases section)

**Checkpoint**: US5 complete — registered badge shows live nametag within 15 s of power-on; unregistered badge reaches main UI within 5 s of "Not registered" message; no crashes. (SC-003, SC-006)

---

## Phase 8: Polish & Cross-Cutting Concerns

- [X] T028 [P] Review firmware/micropython-build/config.py: check if BYPASS and BYPASS_DELAY_MS are still referenced after boot.py refactors from T006 and T024–T027; remove unused constants if confirmed unreferenced (plan.md: "MODIFY if needed — remove BYPASS/BYPASS_DELAY_MS") — neither BYPASS nor BYPASS_DELAY_MS existed in config.py; no changes needed
- [X] T029 [P] Review firmware/micropython-build/badge_sdk.py: confirm badge_uid property naming is consistent with boot.py usage; confirm no raw urequests calls remain anywhere outside badge_api.py (FR-019: all callers must use named endpoint functions only) — no raw urequests calls; uid property consistent
- [ ] T030 Run full post-verification checklist from quickstart.md on hardware against all 12 acceptance criteria; confirm every checkbox passes before closing spec

---

## Phase 9: Hardware Iteration — Post-Flash Bug Fixes

**Context**: Discovered during first hardware boot (2026-03-10). Badge flashed successfully and boots. Issues found: XBM layout does not match Arduino reference, QR too small to scan, badge_api.py not yet on VFS (urequests import fails), button visual feedback needs verification.

**Device**: SSD1309 OLED — compatible with the frozen ssd1306 driver (identical I2C protocol). No driver change needed.

### US2-ext: Display Layout — XBM Blit (matches Arduino Firmware-0306 reference)

**Goal**: Replace procedural `draw_base()` drawing with a direct blit of the original `Graphics_Base.xbm` pixel art. RX/TX arrows blitted from their source XBMs. Joystick center and tilt rect corrected to match Arduino coordinates.

**Independent Test**: Power on badge, observe main UI. TEMPORAL header matches the custom pixel-art logo from `layout.bmp`. Status bar chrome, joystick circle at (100,53), and d-pad positions match the Arduino reference image pixel-for-pixel.

- [X] T031 [US2] Rewrite `draw_base()` in `firmware/micropython-build/graphics.py` to blit the `Graphics_Base` XBM: add `import framebuf` at top; store `_GRAPHICS_BASE` as a `bytes` literal containing all 1024 bytes from `firmware/Firmware-0306/graphics/graphics.h` `Graphics_Base_bits[]`; create `_BASE_FB = framebuf.FrameBuffer(bytearray(_GRAPHICS_BASE), 128, 64, framebuf.MONO_HLSB)` at module level; replace the body of `draw_base()` with `display.blit(_BASE_FB, 0, 0)` — this replaces the TEMPORAL text, hline, rect, `_circle()`, and crosshair with the exact original pixel art in one blit call
- [X] T032 [US2] Rewrite `draw_rx_arrow()` in `firmware/micropython-build/graphics.py` to blit the `Down_Arrow` XBM: store `_ARROW_RX` as a 12-byte `bytes` literal from `Down_Arrow_bits[]` in `graphics.h`; create `_ARROW_RX_FB = framebuf.FrameBuffer(bytearray(_ARROW_RX), 9, 6, framebuf.MONO_HLSB)`; replace procedural pixel drawing in `draw_rx_arrow(display, x, y, filled)` with `display.blit(_ARROW_RX_FB, x, y)` for unfilled; for filled, blit `_ARROW_RX_FILLED_FB` from `Down_Arrow_Filled_bits[]` (identical bytes — both are `0x10, 0x00, 0x10, 0x00, 0x7c, 0x00, 0x38, 0x00, 0x10, 0x00, 0x00, 0x00`)
- [X] T033 [US2] Rewrite `draw_tx_arrow()` in `firmware/micropython-build/graphics.py` to blit the `Up_Arrow` XBM: store `_ARROW_TX` as a 12-byte `bytes` literal from `Up_Arrow_bits[]` (`0x10, 0x00, 0x38, 0x00, 0x7c, 0x00, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00`); create `_ARROW_TX_FB`; for filled use `_ARROW_TX_FILLED_FB` from `Up_Arrow_Filled_bits[]` (`0xef, 0x01, 0xc7, 0x01, 0x83, 0x01, 0xef, 0x01, 0xef, 0x01, 0xff, 0x01`); replace body of `draw_tx_arrow(display, x, y, filled)` with the appropriate blit
- [X] T034 [US2] Fix joystick dot centre in `firmware/micropython-build/main.py`: change `_joy_dot_y = 56` (line ~93) to `_joy_dot_y = 53`; change `_joy_dot_y = 56 + round(ny * 6)` (line ~166) to `_joy_dot_y = 53 + round(ny * 6)` — Arduino reference uses `JOY_CIRCLE_CY = 53`; the XBM base blit (T031) draws the circle at row 53 so the dot must match
- [X] T035 [US2] Fix tilt indicator rect height in `firmware/micropython-build/main.py`: change both `display.fill_rect(84, 48, 4, 4, 1)` and `display.fill_rect(84, 54, 4, 4, 1)` to use height `5` instead of `4` — Arduino reference uses `fillRect(84, 48, 4, 5)` and `fillRect(84, 54, 4, 5)`

**Checkpoint**: D1–D5 complete — main UI visually matches `layout.bmp` reference image; joystick dot tracks correctly inside XBM circle; tilt indicator rectangles are 5 px tall.

### US4-ext: Badge API Client — VFS Deploy Fix

**Goal**: `badge_api.py` is present on badge VFS and `import urequests` succeeds. Boot log shows no `requests` module error.

**Independent Test**: After copying, type `import badge_api` in REPL — no error. Type `badge_api.get_badge_info("test")` — returns `None` (404 or network error), not an exception.

- [ ] T036 [US4] Copy `firmware/micropython-build/badge_api.py` to the badge VFS using Thonny (Files panel → right-click → Upload to /) or `mpremote connect <PORT> fs cp badge_api.py :badge_api.py`; confirm file appears in REPL via `import os; os.listdir('/')`; then test `import badge_api` completes without error

**Checkpoint**: A complete — `badge_api.py` on VFS, `urequests` import succeeds, no boot log errors for nametag fetch.

### US5-ext: QR Code — Full-Screen Display

**Goal**: QR code fills the entire 128×64 display and is scannable by a phone camera.

**Independent Test**: Boot badge, watch QR phase — QR modules are large enough to scan with a phone held ~15 cm from the display.

- [X] T037 [US5] Rework `draw_qr()` in `firmware/micropython-build/graphics.py`: after `matrix = qr.get_matrix()` and `size = len(matrix)`, compute `scale` as `min(128 // size, 64 // size)` but enforce `scale >= 1`; then add a border pass — compute `ox = (128 - size * scale) // 2` and `oy = (64 - size * scale) // 2` but set both to `0` if `scale >= 3` to maximise usable area (full-bleed); call `display.fill(0)` before drawing, draw all modules with `display.fill_rect`, call `display.show()` — verify the QR fills most of the screen at the typical badge UUID input size (version 2–3 QR, 25–29 modules)

**Checkpoint**: B complete — QR occupies majority of display area, scannable from normal viewing distance.

### US2-ext: Button Inputs — Wiring Verification

**Goal**: Confirm whether button GPIOs are wired correctly by adding explicit single-pixel indicators in the status bar so testers can distinguish no-press, held, and released states at a glance.

**Independent Test**: Hold each of the 4 directional buttons individually — corresponding indicator pixel lights up. Release — pixel goes dark. If no pixel lights, wiring/GPIO is the problem not the logic.

- [X] T038 [US2] Verify button input logic in `firmware/micropython-build/main.py`: add a temporary `print("BTN state:", [btn[2] for btn in _btns])` log inside the render loop (throttled: only print every 20 frames) so button state is visible in REPL without a display; exercise each button and confirm GPIO reads 0 when held (active-low); if any button always reads 1 regardless of press, note the GPIO for hardware investigation; remove the print after confirming — do not leave debug prints in production code

**Checkpoint**: C complete — each button's GPIO state confirmed via REPL log; any broken button identified by GPIO number for hardware team.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 (Setup)**: No dependencies — start immediately; read-only
- **Phase 2 (Foundational)**: Depends on Phase 1 completion; firmware rebuild required
- **Phase 3 (US1)**: T006/T007/T008 can be written before Phase 2 completes; hardware verification requires Phase 2 (firmware flashed) and T006 (interruptible boot)
- **Phase 4 (US2)**: Depends on Phase 2 (firmware flashed); independent of US1 except T006 overlap
- **Phase 5 (US3)**: Depends on Phase 2 + T006 (interruptible boot sleeps); independent of US2 and US4 for implementation; hardware verify needs Phase 2
- **Phase 6 (US4)**: No hardware dependency — badge_api.py is a new file; can be written concurrently with any phase; hardware REPL verification needs US1 flash tool working
- **Phase 7 (US5)**: Depends on US3 (WiFi connect implemented) and US4 (badge_api.py deployed)
- **Phase 8 (Polish)**: Depends on all user stories complete

### User Story Dependencies

- **US1 (P1)**: Depends on Foundational firmware; BLOCKS hardware verification of US3–US5
- **US2 (P2)**: Depends on Foundational firmware only; independent of all other user stories
- **US3 (P3)**: Depends on Foundational + T006 (interruptible boot sleeps); independent of US2 and US4
- **US4 (P4)**: No dependencies — new file; parallelizable with US1, US2, US3; REPL verification needs US1 flash tool
- **US5 (P5)**: Depends on US3 (WiFi path in boot.py) and US4 (badge_api.py deployed)

### Within Each User Story

- T004 (rebuild) must complete before T005 (flash)
- T017 (_request transport) must complete before T018–T023 (endpoint functions share the same file)
- T014+T015 (WiFi connect/NTP) before T016 (failure halt) for clean boot.py structure
- T024 (fix nametag fetch import) before T025 (nametag display), T026 (404 path), T027 (error wrap)

---

## Parallel Execution Opportunities

### US4 (badge_api.py) runs concurrently with US1+US2+US3:

```
[Phase 1 complete]
├── Phase 2: rebuild → flash  (blocks US1/US2/US3 hardware verify)
│   ├── US1: T006 (boot.py sleeps) → T007 (flash.sh) → T008 (VFS_FILES)
│   ├── US2: T009+T010+T013 [parallel] → T011 → T012
│   └── US3: T014 → T015 → T016
└── US4: T017 (_request) → T018–T023 [all in same file, sequential]  (no device needed)

[US3 + US4 complete]
└── US5: T024 → T025+T026 → T027
```

### Within US2, three tasks start in parallel:

```
T009 (buttons) ─┐
T010 (joystick) ─┤→ T011 (tilt debounce) → T012 (IR arrows)
T013 (draw_base) ─┘
```

### Within US1, T008 runs in parallel with T006+T007:

```
T006 (boot.py sleeps) ─┐
T007 (flash.sh refactor) ─┤ (all complete before hardware verify)
T008 [P] (VFS_FILES list) ─┘
```

---

## Implementation Strategy

### MVP (US1 Only)

1. Phase 1: Read current sources
2. Phase 2: Rebuild + reflash firmware
3. Phase 3: US1 flash tool reliability
4. **STOP and VALIDATE**: Run `./flash.sh --vfs-only` per quickstart.md US1; confirm SC-001

### Full Incremental Delivery

1. Phase 1 → Phase 2 (firmware foundation)
2. US1 → validate flash tool (SC-001)
3. US2 + US4 in parallel → validate hardware UI (SC-002) + REPL-test badge_api (SC-005)
4. US3 → validate WiFi boot path (SC-003, SC-004)
5. US5 → validate live nametag (SC-003, SC-006)
6. Phase 8 Polish → final quickstart.md checklist

---

## Notes

- No automated tests — all verification is manual on hardware per quickstart.md
- MicroPython v1.24.0 constraint: use `str.format()` not f-strings, `or ''` not walrus operator, `ujson` not `json`, `ure` for regex if needed
- Every boot.py sleep replacement must preserve total duration while allowing Ctrl+C on each 100 ms boundary
- badge_api.py MUST use `r.close()` in a `finally` block — embedded device has ≤ 240 KB heap; leaked HTTP connections cause OOM
- flash.sh per-file mpremote reconnects: ~2 s per file × 8 files ≈ 16 s, well within SC-001 (60 s limit)
- HMAC auth is NOT in scope — _request() has a comment placeholder per contracts/badge_api.md Future Auth Hook section
- [P] tasks operate on different files or non-overlapping functions with no shared mutable state
