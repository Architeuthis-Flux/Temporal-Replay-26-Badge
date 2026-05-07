# Feature Specification: MicroPython Port for Temporal Badge DELTA

**Feature Branch**: `001-micropython-port`
**Created**: 2026-03-10
**Status**: Draft
**Input**: User description: "Convert the existing Arduino firmware (firmware/Firmware-0306/Firmware-0306.ino) to a MicroPython build targeting the Temporal Badge DELTA (ESP32-S3-MINI-1, XIAO form factor)."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Developer builds and flashes the badge (Priority: P1)

A firmware developer clones the repo, runs `build.sh`, then `flash.sh`, and ends up with a working badge that boots, connects to WiFi (or enters BYPASS mode), displays the splash graphic on the OLED, and responds to USB serial REPL commands.

**Why this priority**: This is the gate for all other work. Nothing else is testable until the badge is flashed and reachable over serial.

**Independent Test**: Run `build.sh`, then `flash.sh` on a freshly cloned repo. Connect to `/dev/cu.usbmodem*` with `screen` or `mpremote`. Verify the REPL prompt appears and `import machine` succeeds.

**Acceptance Scenarios**:

1. **Given** a clean checkout with correct host toolchain, **When** `build.sh` is run, **Then** `firmware-conference.bin` is produced with no errors and the script exits 0.
2. **Given** a badge connected via USB-C, **When** `flash.sh` is run, **Then** the device is erased, firmware written, VFS files copied, and after reboot a USB serial REPL is reachable at `/dev/cu.usbmodem*`.

---

### User Story 2 - Badge boots with full display sequence (Priority: P2)

On power-up the badge performs the same visual boot sequence as the Arduino reference: splash graphic, welcome message with badge ID, WiFi connect, QR code display for 10 seconds, nametag fetch, then main interactive display.

**Why this priority**: Preserves the conference attendee experience. Behaviorally identical output to the Arduino reference is the acceptance bar for the port.

**Independent Test**: Flash badge with `BYPASS = True` in 1`config.py`. Observe OLED: should cycle splash → welcome text → "Connected (bypass)" → QR mock bitmap for 10 s → nametag mock bitmap → main UI with joystick/button overlays.

**Acceptance Scenarios**:

1. **Given** `BYPASS = True`, **When** badge powers on, **Then** OLED shows splash with Temporal wordmark, then welcome text including the 12-hex-char badge ID derived from `machine.unique_id()`.
2. **Given** `BYPASS = True`, **When** boot sequence reaches QR step, **Then** mock QR bitmap (converted from `graphics.h` PROGMEM) fills the 128×64 display for 10 seconds.
3. **Given** `BYPASS = True`, **When** boot completes, **Then** main UI renders: base graphic, two text lines, joystick dot, tilt indicator, IR TX/RX arrows, and button state dots.
4. **Given** live WiFi credentials in `creds.py` and `BYPASS = False`, **When** badge powers on, **Then** WiFi connects within 5 seconds and NTP syncs before continuing boot.

---

### User Story 3 - IR badge-to-badge pairing (Priority: P3)

Pressing BTN_UP transmits the badge's 6-byte ID over IR (NEC protocol, address 0x42). A second badge in receive mode picks up the 6 bytes, reassembles the ID, and displays it on screen.

**Why this priority**: Core social/game mechanic. Depends on P1 and P2 being stable first.

**Independent Test**: Two flashed badges facing each other. Press BTN_UP on badge A. Badge B screen should show "RX UID: <12-char hex>". Badge A screen should show "IR TX: sent!".

**Acceptance Scenarios**:

1. **Given** two badges in range, **When** BTN_UP is pressed on badge A, **Then** badge A displays "IR TX: sending UID..." then "IR TX: sent!", and badge B displays each of 6 received bytes with a counter "UID 1/6"…"UID 6/6", then "RX UID: <hex>".
2. **Given** IR receive is listening, **When** a non-badge NEC signal is received, **Then** the decoded protocol name and command byte are displayed for 1 second, then display returns to "IR: listening...".
3. **Given** a partial UID is being received, **When** no further bytes arrive for 500 ms, **Then** the partial buffer is discarded and the display returns to "IR: listening...".

---

### User Story 4 - Button, joystick, and tilt inputs (Priority: P4)

All physical inputs work correctly: buttons debounced at 40 ms, joystick polled at 50 ms with normalized clamped output, tilt switch debounced at 300 ms.

**Why this priority**: Input peripherals are test-verifiable in isolation; needed before any interactive feature builds on them.

**Independent Test**: In main loop, observe screen indicators. Press each button — the corresponding dot should appear. Move joystick — the small square should track. Tilt badge — tilt indicator rect changes position.

**Acceptance Scenarios**:

1. **Given** main UI is displayed, **When** BTN_UP/DOWN/LEFT/RIGHT is held, **Then** the corresponding indicator dot is filled; releasing it clears the dot.
2. **Given** joystick at center, **When** joystick is pushed to an extreme, **Then** the position dot moves to the edge of the 6-pixel-radius circle and does not exceed it.
3. **Given** badge is upright, **When** badge is tilted past threshold, **Then** tilt indicator moves after 300 ms debounce with no false triggers during normal handling.

---

### Edge Cases

- What happens when WiFi credentials are present but the network is unreachable? → Boot continues after `WIFI_TIMEOUT_MS` (5 s); badge displays failure message and halts (mirrors Arduino behavior).
- What happens when the QR fetch fails (HTTP non-200 or network error)? → "QR unavailable, continuing..." message for 2 s; transitions directly to main loop.
- What happens when `machine.unique_id()` returns fewer than 6 bytes? → Pad to 6 bytes; badge ID is always 12 hex chars.
- What happens when IR TX is triggered while IR RX is active? → RX is temporarily suspended during the 6-frame TX burst, then re-enabled.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The firmware MUST be buildable from source using `build.sh` with MicroPython v1.24.0 and ESP-IDF v5.2.3, producing a single `.bin` file flashable to the badge hardware.
- **FR-002**: The build system MUST define a custom board `TEMPORAL_BADGE_DELTA` with the exact GPIO assignments from `Firmware-0306`: I2C SDA=GPIO5, SCL=GPIO6; IR TX=GPIO3, RX=GPIO4; BTN_UP=GPIO8, DOWN=GPIO9, LEFT=GPIO10, RIGHT=GPIO11; JOY_X=GPIO1, JOY_Y=GPIO2; TILT=GPIO7.
- **FR-003**: `flash.sh` MUST erase flash, write the firmware binary, and copy all VFS Python files (`config.py`, `boot.py`, `main.py`, `ir_nec.py`, `badge_sdk.py`, `graphics.py`) to the device filesystem via `mpremote`.
- **FR-004**: After flashing, a USB serial REPL MUST be accessible at `/dev/cu.usbmodem*` without additional host configuration.
- **FR-005**: The badge MUST derive its ID from `machine.unique_id()` (6 bytes, hex-encoded to 12 lowercase characters) and use this ID throughout boot messages and IR transmission.
- **FR-006**: `config.py` MUST define all GPIO pin numbers, timing constants (`WIFI_TIMEOUT_MS`, `QR_DISPLAY_MS`, `BYPASS_DELAY_MS`, `TILT_SHOWS_BADGE`), and a `BYPASS` flag; it MUST NOT contain WiFi credentials or server URLs.
- **FR-007**: WiFi credentials (`SSID`, `PASS`) and `SERVER_URL` MUST be read from a VFS-resident `creds.py`; this file MUST be listed in `.gitignore` and deployed via `mpremote fs cp`.
- **FR-008**: When `BYPASS = True`, `boot.py`/`main.py` MUST skip live network calls and substitute mock XBM bitmaps (Python `bytes` literals translated from `graphics.h` PROGMEM arrays) for QR and nametag images.
- **FR-009**: `boot.py` MUST connect to WiFi (or simulate via BYPASS), optionally sync NTP, and display status messages on the SSD1309 OLED during boot.
- **FR-010**: `main.py` MUST replicate the full Arduino boot sequence: welcome message → WiFi connect → QR display (10 s) → nametag fetch → main interactive loop.
- **FR-011**: `main.py` MUST poll buttons (40 ms debounce), joystick (50 ms interval, 12-bit ADC, normalized ±1.0 clamped to circle radius), and tilt switch (300 ms debounce) on every main loop iteration.
- **FR-012**: `ir_nec.py` MUST implement NEC IR TX and RX using the ESP32 RMT peripheral; TX sends 6 NEC frames (address 0x42, one badge-ID byte per frame, 50 ms inter-frame gap); RX reassembles 6 consecutive frames with address 0x42 into a badge ID.
- **FR-013**: IR RX MUST run concurrently with the main loop (via a thread or equivalent); a partial UID buffer MUST be discarded after 500 ms of silence.
- **FR-014**: `badge_sdk.py` MUST provide a `Badge` class with methods: `uid()` → 12-char hex string, `display(line1, line2)`, `ping(target_uid)` → IR transmit, `pings()` → list of received UIDs since last call; no HMAC or eFuse usage.
- **FR-015**: The `boards/TEMPORAL_BADGE_DELTA/` board definition MUST freeze the `ssd1306` and `urequests` modules; it MUST NOT reference any files from `firmware/shy-micropython-build/`.
- **FR-016**: The build MUST enable USB-CDC for the serial REPL; Bluetooth support is NOT required for this spec.

### Key Entities

- **Badge ID**: 12-character lowercase hex string derived from `machine.unique_id()` (6 MAC bytes). Stable per device. Used in display messages, IR transmission, and future HTTP requests.
- **XBM Bitmap**: 128×64 monochrome bitmap (1024 bytes) displayed on the SSD1309. Sourced from server (live mode) or from `graphics.py` mock arrays (BYPASS mode).
- **VFS Python Files**: Files stored on the device filesystem and copied via `mpremote`: `config.py`, `boot.py`, `main.py`, `ir_nec.py`, `badge_sdk.py`, `graphics.py`, and `creds.py` (gitignored).
- **Board Definition**: MicroPython board directory (`boards/TEMPORAL_BADGE_DELTA/`) containing `mpconfigboard.h`, `mpconfigboard.cmake`, `sdkconfig.board`, `partitions.csv`, `manifest.py`, `pins.csv`.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: `build.sh` completes without errors on macOS (Apple Silicon) with the pinned toolchain and produces a flashable `.bin` in under 20 minutes.
- **SC-002**: After running `flash.sh`, the badge boots and a USB serial REPL is reachable within 30 seconds of device reset.
- **SC-003**: In BYPASS mode, the full boot sequence (splash → welcome → simulated WiFi → QR → nametag → main loop) completes without crashing, observable end-to-end via OLED and serial output.
- **SC-004**: Pressing BTN_UP on one badge causes a second badge to display the sender's badge ID within 2 seconds, verified by reading the receiver's OLED.
- **SC-005**: All four buttons, joystick, and tilt switch produce correct visual feedback in the main UI with no missed events during manual exercise of each input.
- **SC-006**: The firmware binary fits within the 4 MB app partition defined in `partitions.csv`.

## Assumptions

- Host build machine is macOS (Apple Silicon). `build.sh` applies `CFLAGS_EXTRA="-Wno-gnu-folding-constant"` for Clang 15+ compatibility; this is proven to work.
- `machine.unique_id()` on ESP32-S3 returns exactly 6 bytes (the MAC address base). If fewer bytes are returned, the badge ID is zero-padded to 12 hex chars.
- The SSD1309 OLED at I2C address 0x3C is compatible with the `ssd1306` MicroPython driver (same I2C protocol, different controller IC).
- NTP sync requires WiFi; in BYPASS mode it is skipped without error.
- A two-badge IR pairing test requires two physical DELTA rev badges; single-badge testing covers TX-only behavior via serial logging.
- `TILT_SHOWS_BADGE` defaults to `False` (matching Arduino reference), so tilt only changes the indicator widget; it does not switch to full nametag display mode.
- Mock XBM bitmaps in `graphics.py` are Python `bytes` literals directly translated from the PROGMEM byte arrays in `graphics.h` — no format conversion needed.

## Out of Scope

- `badge_crypto` C extension module (eFuse HMAC peripheral operations)
- eFuse key burning or badge enrollment workflow
- HMAC-signed HTTP requests to the backend
- Backend API changes (`registrationScanner/`)
- OTA firmware update
- BLE functionality
