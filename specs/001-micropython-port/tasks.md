---

description: "Task list for MicroPython Port — Temporal Badge DELTA"
---

# Tasks: MicroPython Port for Temporal Badge DELTA

**Input**: Design documents from `/specs/001-micropython-port/`
**Prerequisites**: plan.md ✓, spec.md ✓, research.md ✓, data-model.md ✓, contracts/badge_sdk.md ✓, quickstart.md ✓

**Tests**: Manual only per project constitution. No automated test runner. Verification via flash → USB REPL → OLED observation → two-badge IR test.

**Organization**: Tasks grouped by user story to enable independent implementation and manual verification of each story.

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies on incomplete tasks)
- **[Story]**: Which user story this task belongs to (US1, US2, US3, US4)
- All paths relative to repository root

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create the `firmware/micropython-build/` directory tree and protect sensitive files

- [X] T001 Create `firmware/micropython-build/` directory tree: `boards/TEMPORAL_BADGE_DELTA/`, `modules/.gitkeep` (keeps modules dir in git for future badge_crypto C extension)
- [X] T002 [P] Add `firmware/micropython-build/creds.py` to `.gitignore` (WiFi SSID/PASS + SERVER_URL — never commit)

---

## Phase 2: Foundational (Board Definition — Blocking Prerequisites)

**Purpose**: Board definition files that MUST exist before `build.sh` can produce any firmware binary. All files are independent of each other.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete — the board definition is the prerequisite for the MicroPython build system.

- [X] T003 [P] Create `firmware/micropython-build/boards/TEMPORAL_BADGE_DELTA/mpconfigboard.h`: define `MICROPY_HW_BOARD_NAME "TEMPORAL_BADGE_DELTA"`, `MICROPY_HW_MCU_NAME "ESP32S3"`, enable USB-CDC (`MICROPY_HW_USB_CDC 1`), disable Bluetooth stack (`MICROPY_PY_BLUETOOTH 0`), disable BLE (`MICROPY_BLUETOOTH_NIMBLE 0`)
- [X] T004 [P] Create `firmware/micropython-build/boards/TEMPORAL_BADGE_DELTA/mpconfigboard.cmake`: set IDF target `esp32s3`, sdkconfig layer order `base → usb → ble → sdkconfig.board`, set `MICROPY_FROZEN_MANIFEST` to `manifest.py`, do NOT set `USER_C_MODULES` here (passed via build.sh command line)
- [X] T005 [P] Create `firmware/micropython-build/boards/TEMPORAL_BADGE_DELTA/sdkconfig.board`: enable 8 MB QIO flash (`CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`, `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y`), enable SPIRAM (`CONFIG_SPIRAM=y`), enable HMAC peripheral (`CONFIG_EFUSE_HMAC=y`), set custom partitions path (`CONFIG_PARTITION_TABLE_CUSTOM=y`, `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"`)
- [X] T006 [P] Create `firmware/micropython-build/boards/TEMPORAL_BADGE_DELTA/partitions.csv`: NVS 24 KB at 0x9000, factory app 4 MB at 0x10000, vfs 4 MB following factory partition (no header row, standard ESP-IDF CSV format)
- [X] T007 [P] Create `firmware/micropython-build/boards/TEMPORAL_BADGE_DELTA/manifest.py`: freeze `ssd1306` from `$(MPY_LIB_DIR)/micropython/drivers/display/ssd1306`, freeze `urequests` from `$(MPY_LIB_DIR)/micropython/urequests`; do NOT reference any files from `firmware/shy-micropython-build/`
- [X] T008 [P] Create `firmware/micropython-build/boards/TEMPORAL_BADGE_DELTA/pins.csv`: no header row; `BOARD_NAME,GPIOx` format; include all 11 pins from constitution: I2C_SDA=GPIO5, I2C_SCL=GPIO6, IR_TX=GPIO3, IR_RX=GPIO4, BTN_UP=GPIO8, BTN_DOWN=GPIO9, BTN_LEFT=GPIO10, BTN_RIGHT=GPIO11, JOY_X=GPIO1, JOY_Y=GPIO2, TILT=GPIO7
- [X] T009 Create `firmware/micropython-build/config.py`: define all GPIO pin constants (`I2C_SDA=5`, `I2C_SCL=6`, `I2C_FREQ=400000`, `BTN_UP=8`, `BTN_DOWN=9`, `BTN_LEFT=10`, `BTN_RIGHT=11`, `JOY_X_PIN=1`, `JOY_Y_PIN=2`, `TILT_PIN=7`, `IR_TX_PIN=3`, `IR_RX_PIN=4`), timing constants (`WIFI_TIMEOUT_MS=5000`, `QR_DISPLAY_MS=10000`, `BYPASS_DELAY_MS=3000`), `TILT_SHOWS_BADGE=False`, `BYPASS=True`; NO WiFi credentials or SERVER_URL

**Checkpoint**: Board definition complete — `firmware/micropython-build/` is a valid MicroPython board directory. User story implementation can now begin.

---

## Phase 3: User Story 1 — Developer Builds and Flashes (Priority: P1) 🎯 MVP

**Goal**: `build.sh` produces a flashable firmware binary; `flash.sh` erases, flashes, and copies VFS files; USB serial REPL is accessible after reboot.

**Independent Test**: Run `./build.sh` → verify `firmware-conference.bin` ≤ 4 MB, exit 0. Connect badge via USB-C → `./flash.sh` → `mpremote connect /dev/cu.usbmodem*` → REPL prompt appears → `import machine` succeeds without error.

### Implementation for User Story 1

- [X] T010 [US1] Create `firmware/micropython-build/build.sh`: clone or use cached MicroPython v1.24.0 + ESP-IDF v5.2.3; set `CFLAGS_EXTRA="-Wno-gnu-folding-constant"` for Apple Silicon Clang 15+; build target `TEMPORAL_BADGE_DELTA` with `idf.py -DMICROPY_BOARD=TEMPORAL_BADGE_DELTA build`; output binary as `firmware-conference.bin`; print file size and fail with non-zero exit if > 4 MB
- [X] T011 [P] [US1] Create `firmware/micropython-build/flash.sh`: detect `/dev/cu.usbmodem*` port; `esptool.py erase_flash`; `esptool.py write_flash 0x0 firmware-conference.bin`; `mpremote fs cp config.py :config.py` for each VFS file (`config.py`, `boot.py`, `main.py`, `ir_nec.py`, `badge_sdk.py`, `graphics.py`); copy `creds.py` only if it exists; `mpremote reset`
- [X] T012 [P] [US1] Create `firmware/micropython-build/boot.py` (stub): try/except import of `creds` (set `BYPASS=True` in `config` namespace if `ImportError`); import `config`; initialize SSD1309 I2C display (`ssd1306.SSD1306_I2C(128, 64, machine.I2C(0, sda=machine.Pin(config.I2C_SDA), scl=machine.Pin(config.I2C_SCL), freq=config.I2C_FREQ))`); display "Temporal / Badge DELTA / Starting..." on OLED; no network calls in this stub
- [X] T013 [P] [US1] Create `firmware/micropython-build/main.py` (stub): `import config, utime`; compute `badge_uid` from `machine.unique_id()` with zero-padding to 12 hex chars (`''.join(f'{b:02x}' for b in machine.unique_id()).ljust(12,'0')[:12]`); infinite loop with `utime.sleep_ms(50)`
- [X] T014 [P] [US1] Create `firmware/micropython-build/badge_sdk.py` (stub): `Badge` class with `__init__` that reads `machine.unique_id()` and stores `self._uid`; `uid` property returning 12-char zero-padded hex string; `display_text(line1, line2)` raising `NotImplementedError`; `ping(target_uid)` raising `NotImplementedError`; `pings()` returning empty list
- [X] T015 [P] [US1] Create `firmware/micropython-build/ir_nec.py` (stub): `NECTX` class with `__init__(self, pin_num)` and `send(self, address, command)` raising `NotImplementedError`; `NECRX` class with `__init__(self, pin_num, on_frame)` and `decode_loop(self)` raising `NotImplementedError`
- [X] T016 [P] [US1] Create `firmware/micropython-build/graphics.py` (stub): `GRAPHICS_BASE = bytes(1024)`, `MOCK_QR = bytes(1024)`, `MOCK_NAMETAG = bytes(1024)`, arrow sprites as zero-filled bytes (`DOWN_ARROW_FILLED = bytes(12)`, `DOWN_ARROW = bytes(12)`, `UP_ARROW_FILLED = bytes(12)`, `UP_ARROW = bytes(12)`)

**Checkpoint**: `build.sh` exits 0, firmware-conference.bin is ≤ 4 MB. `flash.sh` succeeds. USB REPL is accessible and `import machine` works. User Story 1 is independently verified.

---

## Phase 4: User Story 2 — Boot Display Sequence (Priority: P2)

**Goal**: Full Arduino-parity boot sequence visible on OLED — splash → welcome text with badge ID → WiFi/BYPASS status → QR bitmap 10 s → nametag bitmap → main interactive display.

**Independent Test**: Set `BYPASS = True` in `firmware/micropython-build/config.py`. Flash badge. Observe OLED: splash Temporal wordmark → welcome text with 12-char hex badge ID → "Connected (bypass)" → mock QR bitmap for 10 s → mock nametag bitmap → main UI with joystick dot, tilt indicator, button dots. Serial REPL shows boot log.

### Implementation for User Story 2

- [X] T017 [US2] Replace `firmware/micropython-build/boot.py` stub with full implementation: BYPASS=True path: delay `BYPASS_DELAY_MS`, display "Connected (bypass)"; BYPASS=False path: `network.WLAN(network.STA_IF)`, connect with `WIFI_TIMEOUT_MS` timeout, on failure display "WiFi failed" and `raise SystemExit`; on success display "Connected to \<SSID\>"; NTP sync via `ntptime.settime()` (BYPASS skips); fetch QR XBM from `GET \<SERVER_URL\>/api/v1/badge/\<badge_uid\>/qr.xbm` (BYPASS uses `graphics.MOCK_QR`); display QR framebuf for `QR_DISPLAY_MS`; on HTTP failure display "QR unavailable, continuing..." for 2 s
- [X] T018 [P] [US2] Replace `firmware/micropython-build/graphics.py` stub with full conversion: parse `firmware/Firmware-0306/graphics.h` PROGMEM arrays using regex (`re.findall(r'0x[0-9a-fA-F]+', ...)`) on dev machine; output `GRAPHICS_BASE` (1024 bytes), `MOCK_QR` (1024 bytes), `MOCK_NAMETAG` (1024 bytes), `DOWN_ARROW_FILLED` (12 bytes), `DOWN_ARROW` (12 bytes), `UP_ARROW_FILLED` (12 bytes), `UP_ARROW` (12 bytes) as Python bytes literals; validate each 128×64 bitmap is exactly 1024 bytes
- [X] T019 [US2] Replace `firmware/micropython-build/main.py` stub with full display entry: fetch nametag XBM from `GET \<SERVER_URL\>/api/v1/badge/\<badge_uid\>/info` (BYPASS uses `graphics.MOCK_NAMETAG`); render nametag via `framebuf.FrameBuffer(bytearray(xbm[:1024]), 128, 64, framebuf.MONO_HLSB)`; implement `_parse_xbm_response(text)` to extract bytes from XBM text format; implement mode constants `MODE_BOOT=0`, `MODE_QR=1`, `MODE_MAIN=2`; `current_mode` starts at `MODE_MAIN` after boot; infinite main loop placeholder calling `utime.sleep_ms(50)`
- [X] T020 [US2] Update `firmware/micropython-build/badge_sdk.py`: replace stub with working `display` property returning `ssd1306.SSD1306_I2C` instance (128×64, I2C from `config.I2C_SDA/SCL/FREQ`); implement `display_text(line1, line2)` — `fill(0)`, `text(line1[:16], 0, 0)`, `text(line2[:16], 0, 16)`, `show()`; `Badge.__init__` must NOT crash when called before WiFi connects

**Checkpoint**: In BYPASS mode, full boot sequence runs without crashing. OLED shows all expected screens in order. Verifiable via OLED observation alone. User Story 2 independently verified.

---

## Phase 5: User Story 3 — IR Badge-to-Badge Pairing (Priority: P3)

**Goal**: BTN_UP on badge A transmits the 6-byte badge ID as 6 NEC frames (address 0x42). Badge B receives, reassembles, and displays the sender's UID. Non-badge NEC signals display protocol info for 1 s.

**Independent Test**: Two flashed badges facing each other at < 2 m. Press BTN_UP on badge A. Badge A OLED: "IR TX: sending UID..." → "IR TX: sent!". Badge B OLED: "IR: UID 1/6" through "IR: UID 6/6" → "RX UID: \<12-char hex\>". Single-badge test: press BTN_UP, confirm serial log shows TX attempt.

### Implementation for User Story 3

- [X] T021 [US3] Replace `firmware/micropython-build/ir_nec.py` stub with `NECTX` full implementation: `esp32.RMT(0, pin=machine.Pin(pin_num), clock_div=80, tx_carrier=(38000, 50, 1), idle_level=False)`; `_encode_frame(address, command)` → flat tuple of (burst_µs, space_µs) pairs: leader 9000/4500, 32 data bits (BIT_BURST=562, BIT_0_SPACE=562, BIT_1_SPACE=1688), end burst 562, trailing 0; `send(address, command)` calls `self._rmt.write_pulses(...)` then `self._rmt.wait_done(timeout=100)`
- [X] T022 [US3] Replace `firmware/micropython-build/ir_nec.py` `NECRX` stub with full implementation: fixed-size ring buffer (bytearray, length 256) for ISR-safe edge recording; `machine.Pin(pin_num, machine.Pin.IN).irq(self._isr, machine.Pin.IRQ_RISING | machine.Pin.IRQ_FALLING, hard=True)`; `_isr` records `(pin.value(), utime.ticks_us())` into ring buffer; `decode_loop()` runs in background thread, drains ring buffer, implements NEC state machine (idle → leader → 32 bits → frame complete), calls `on_frame(address, command)` on successful decode (depends on T021 for class structure)
- [X] T023 [US3] Add IR UID TX function to `firmware/micropython-build/main.py`: `irTransmitUID(tx, badge_uid_hex)` — decode hex to 6 bytes, iterate 6 bytes calling `tx.send(0x42, byte)` with `utime.sleep_ms(50)` inter-frame gap; update display "IR TX: sending UID..." before TX, "IR TX: sent!" after; temporarily disable RX thread during TX burst by setting a shared `_tx_active` flag checked in decode_loop
- [X] T024 [US3] Add IR RX thread to `firmware/micropython-build/main.py`: allocate `_rx_lock = _thread.allocate_lock()`, `_received_uids = []`; `on_frame(addr, cmd)` callback — if `addr == 0x42`: append `cmd` to `_rx_buf`, update `_last_byte_ms`; if `len(_rx_buf) == 6`: assemble hex string, append to `_received_uids` under `_rx_lock`, clear `_rx_buf`; if `addr != 0x42`: display "IR: \<addr hex\>" for 1 s; implement 500 ms partial buffer discard in decode loop; launch with `_thread.start_new_thread(_ir_rx_loop, (rx,))` before main loop (depends on T022)
- [X] T025 [P] [US3] Update `firmware/micropython-build/badge_sdk.py`: implement `ping(target_uid)` — validate `len(target_uid) == 12` and all hex chars (raise `ValueError` otherwise); decode to bytes; transmit 6 NEC frames via `NECTX.send(0x42, byte)` with 50 ms gaps; implement `pings()` — acquire `_rx_lock`, copy and clear `_received_uids`, release lock, return copy; `Badge.__init__` starts IR RX background thread via `_thread.start_new_thread`
- [X] T026 [US3] Wire BTN_UP press to IR TX in `firmware/micropython-build/main.py` main loop: on `BTN_UP` state-change to pressed, call `irTransmitUID(tx, badge_uid)` (depends on T023, T024)

**Checkpoint**: Two-badge IR pairing works end-to-end. Single-badge TX verified via serial log. Partial buffer discard verified by stopping mid-sequence and waiting 500 ms. User Story 3 independently verified.

---

## Phase 6: User Story 4 — Button, Joystick, and Tilt Inputs (Priority: P4)

**Goal**: All physical inputs produce correct on-screen feedback. Buttons debounced at 40 ms. Joystick normalized ±1.0 clamped to 6-pixel circle. Tilt debounced at 300 ms.

**Independent Test**: Flash badge in BYPASS mode. In main UI, press each button — corresponding indicator dot fills at correct OLED position. Move joystick to extremes — position dot reaches edge of circle without exceeding it. Tilt badge past threshold — tilt indicator rect changes position after 300 ms with no jitter.

### Implementation for User Story 4

- [X] T027 [US4] Implement button debounce in `firmware/micropython-build/main.py`: create 4 `machine.Pin` objects (GPIO 8–11, `Pin.IN`, `Pin.PULL_UP`); per-button debounce state: `last_reading`, `last_debounce_ms`, `state`; 40 ms debounce — only update `state` when `ticks_diff(ticks_ms(), last_debounce_ms) >= 40` and raw reading differs; draw indicator dots at confirmed positions: BTN_UP=(118,48), BTN_DOWN=(118,58), BTN_LEFT=(113,53), BTN_RIGHT=(123,53); fill dot when `state == False` (active-low pressed)
- [X] T028 [US4] Implement joystick ADC polling in `firmware/micropython-build/main.py`: `machine.ADC(machine.Pin(JOY_X_PIN), atten=machine.ADC.ATTN_11DB)` and same for Y; poll every 50 ms; normalize: `nx = (raw_x - 2048) / 2048.0`, same for Y; clamp to unit circle: `dist = math.sqrt(nx*nx + ny*ny); if dist > 1.0: nx /= dist; ny /= dist`; compute dot pixel: `dot_x = 100 + round(nx * 6)`, `dot_y = 53 + round(ny * 6)`; draw 3×3 fill_rect at dot position (depends on T027 — same file, write after button implementation)
- [X] T029 [US4] Implement tilt switch debounce in `firmware/micropython-build/main.py`: `machine.Pin(TILT_PIN, Pin.IN, Pin.PULL_UP)`; 300 ms debounce (same pattern as buttons); when tilted (`state == False`): `display.fill_rect(84, 48, 4, 4, 1)`; when upright: `display.fill_rect(84, 54, 4, 4, 1)` (depends on T028 — same file)
- [X] T030 [US4] Integrate all inputs into main loop render cycle in `firmware/micropython-build/main.py`: each loop iteration: `display.fill(0)`; blit `GRAPHICS_BASE` via framebuf; draw all button indicator dots; draw joystick position dot; draw tilt indicator rect; draw IR TX/RX arrow sprites from `graphics.py` at correct positions; `display.show()`; total loop ≤ 50 ms (depends on T027, T028, T029)

**Checkpoint**: All 4 buttons, joystick, and tilt switch produce correct visual feedback with no missed events during manual exercise. Main loop iteration ≤ 50 ms observable via serial timing. User Story 4 independently verified.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Final validation, cleanup, and edge case handling that spans multiple stories

- [X] T031 [P] Verify `.gitignore` covers `firmware/micropython-build/creds.py` — run `git check-ignore -v firmware/micropython-build/creds.py`; also confirm build artifacts (`.bin`, `build/` dirs) are excluded
- [X] T032 Run complete quickstart.md walkthrough end-to-end: fresh clone → `./build.sh` (verify binary ≤ 4 MB, exit 0) → `./flash.sh` → BYPASS mode boot sequence (all 6 OLED screens) → main UI inputs → IR TX (BTN_UP) → confirm all SC-001 through SC-006 success criteria pass
- [X] T033 Add edge case guards to `firmware/micropython-build/main.py` and `boot.py`: WiFi present but unreachable → display "WiFi failed" and halt after `WIFI_TIMEOUT_MS`; QR HTTP non-200 or network error → "QR unavailable, continuing..." for 2 s then proceed; `machine.unique_id()` < 6 bytes → zero-pad (already in data model formula; verify it's applied everywhere `badge_uid` is computed)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — start immediately
- **Foundational (Phase 2)**: Depends on Phase 1 — **BLOCKS all user stories**
- **User Story 1 (Phase 3)**: Depends on Phase 2 — cannot build without board definition
- **User Story 2 (Phase 4)**: Depends on Phase 3 — requires build + flash infrastructure working
- **User Story 3 (Phase 5)**: Depends on Phase 4 — IR TX/RX UI relies on main loop from US2
- **User Story 4 (Phase 6)**: Depends on Phase 3 (can start after US1 — inputs don't depend on display content from US2/US3, but same main.py file makes parallel work conflict-prone)
- **Polish (Phase 7)**: Depends on all user stories complete

### User Story Dependencies

- **US1 (P1)**: After Foundational — build/flash infrastructure is self-contained
- **US2 (P2)**: After US1 — requires working flash/REPL to verify OLED output
- **US3 (P3)**: After US2 — IR display messages integrate into the main UI from US2
- **US4 (P4)**: After US1 — inputs are independent but all edits land in `main.py`, so sequential after US2/US3 to avoid conflicts

### Within Each User Story

- Board definition files (Phase 2): all independent, write in parallel
- US1 files: all independent (different files), write in parallel
- US2: boot.py and graphics.py are parallel; main.py display entry is sequential after graphics.py
- US3: NECTX before NECRX (same file); RX thread before BTN_UP wiring (same file); badge_sdk.py update is parallel
- US4: buttons → joystick → tilt → integrate (all in main.py; sequential to avoid conflicts)

### Parallel Opportunities (Within Phase)

- Phase 2: T003–T009 can all run in parallel (7 separate files)
- Phase 3 US1: T010–T016 can all run in parallel (7 separate files)
- Phase 4 US2: T017 (boot.py) and T018 (graphics.py) in parallel; T020 (badge_sdk.py) in parallel; T019 (main.py) after T018
- Phase 5 US3: T021+T022 (ir_nec.py, sequential) in parallel with T025 (badge_sdk.py)

---

## Parallel Example: Phase 2 (Foundational)

```bash
# All 7 board definition files can be written simultaneously:
Task T003: boards/TEMPORAL_BADGE_DELTA/mpconfigboard.h
Task T004: boards/TEMPORAL_BADGE_DELTA/mpconfigboard.cmake
Task T005: boards/TEMPORAL_BADGE_DELTA/sdkconfig.board
Task T006: boards/TEMPORAL_BADGE_DELTA/partitions.csv
Task T007: boards/TEMPORAL_BADGE_DELTA/manifest.py
Task T008: boards/TEMPORAL_BADGE_DELTA/pins.csv
Task T009: config.py
```

## Parallel Example: Phase 3 (User Story 1)

```bash
# All US1 files are independent — write simultaneously:
Task T010: build.sh
Task T011: flash.sh
Task T012: boot.py (stub)
Task T013: main.py (stub)
Task T014: badge_sdk.py (stub)
Task T015: ir_nec.py (stub)
Task T016: graphics.py (stub)
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001–T002)
2. Complete Phase 2: Foundational (T003–T009) — **CRITICAL gate**
3. Complete Phase 3: User Story 1 (T010–T016)
4. **STOP and VALIDATE**: Run `build.sh` and `flash.sh`, verify REPL accessible
5. Proceed to US2 only after US1 is verified

### Incremental Delivery

1. Setup + Foundational → board definition ready
2. US1 (build + flash + stubs) → REPL accessible, binary flashing verified (MVP!)
3. US2 (boot sequence + display) → full visual boot experience verified
4. US3 (IR pairing) → two-badge interaction working
5. US4 (inputs) → all hardware inputs verified
6. Polish → quickstart walkthrough passes all success criteria

### Single-Developer Sequence (Recommended)

```
Phase 1 → Phase 2 (all parallel) → Phase 3 (all parallel) → verify build
→ Phase 4 → verify OLED → Phase 5 → verify IR → Phase 6 → verify inputs
→ Phase 7 (quickstart walkthrough)
```

---

## Notes

- `[P]` tasks touch different files — safe to write concurrently
- Story labels trace each task to the user story it enables
- No automated tests — verification is manual flash → observe → interact
- `BYPASS = True` in `config.py` enables development without WiFi/server/second badge
- `firmware/shy-micropython-build/` is reference-only — do NOT copy files from it
- Pin assignments are authoritative from `Firmware-0306` — always cross-check against `constitution` table before writing `pins.csv` or `config.py`
- Commit after each verified checkpoint (US1 verified → commit; US2 verified → commit; etc.)
