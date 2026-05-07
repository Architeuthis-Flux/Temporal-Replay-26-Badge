# Implementation Plan: MicroPython Port for Temporal Badge DELTA

**Branch**: `001-micropython-port` | **Date**: 2026-03-10 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/001-micropython-port/spec.md`

## Summary

Port the Arduino Firmware-0306 badge firmware to MicroPython targeting the ESP32-S3-MINI-1 (XIAO form factor). Deliverables: canonical `firmware/micropython-build/` directory with board definition, Python VFS files, NEC IR implementation via RMT, build script, and flash script. Badge ID sourced from `machine.unique_id()`. No eFuse/HMAC operations in scope.

## Technical Context

**Language/Version**: MicroPython v1.24.0 (Python 3.4 subset); ESP-IDF v5.2.3; build host macOS Apple Silicon
**Primary Dependencies**: `ssd1306` (frozen driver), `urequests` (frozen), `esp32.RMT` (built-in), `_thread` (built-in), `machine.ADC`, `machine.Pin`
**Storage**: 8 MB QIO flash. Partitions: NVS 24 KB, factory app 4 MB, VFS 4 MB. VFS holds Python files deployed via `mpremote`.
**Testing**: Manual only per constitution (no automated test runner). Verification: flash → USB REPL → OLED observation → two-badge IR test.
**Target Platform**: ESP32-S3-MINI-1 XIAO; board definition `TEMPORAL_BADGE_DELTA`. USB-CDC REPL at `/dev/cu.usbmodem*`.
**Project Type**: Embedded firmware (conference badge OS)
**Performance Goals**: Boot to main loop ≤ 10 s (BYPASS mode); main loop iteration ≤ 50 ms; IR RX non-blocking (runs on second thread).
**Constraints**: App binary ≤ 4 MB (partition limit); offline-capable BYPASS mode; no OTA; no debug-build watchdog.
**Scale/Scope**: 2,000 units; flashed once; no OTA path after manufacturing flash.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. MicroPython-First | ✅ PASS | All application logic in Python. badge_crypto C module is **out of scope** for this spec — not built, not linked. Badge ID from `machine.unique_id()`, not eFuse. |
| II. Firmware-0306 Parity | ✅ PASS | This spec IS the parity port. Boot sequence, display modes, IR pairing (NEC 0x42), button/joystick/tilt behavior, and BYPASS mode all replicate Firmware-0306.ino exactly. |
| III. Credentials-at-Build | ✅ PASS | Prototyping exception active. WiFi creds in VFS `creds.py` (gitignored). `config.py` contains only GPIO pins and timing constants — no secrets. |
| IV. Backend Contract Compliance | ✅ PASS | This spec does not make HMAC-signed requests. Existing unsigned endpoints (`/qr.xbm`, `/info`) accessed via `urequests`. `badge_sdk.py` `ping()` → IR transmit only; no HTTP pings in scope. |
| V. Reproducible Build | ✅ PASS | Pinned: MP v1.24.0, IDF v5.2.3. Apple Silicon: `CFLAGS_EXTRA="-Wno-gnu-folding-constant"` (proven from prior work). sdkconfig layer order: base → usb → ble → sdkconfig.board. |
| VI. Hardware Safety | ✅ PASS | `pins.csv` MUST use the confirmed GPIO table from constitution (SDA=5, SCL=6, BTN_UP=8, BTN_DOWN=9, BTN_LEFT=10, BTN_RIGHT=11, JOY_X=1, JOY_Y=2, TILT=7, IR_TX=3, IR_RX=4). No eFuse operations. |

**Constitution verdict: ALL GATES PASS. Proceed to Phase 0.**

## Project Structure

### Documentation (this feature)

```text
specs/001-micropython-port/
├── plan.md              # This file (/speckit.plan command output)
├── research.md          # Phase 0 output (/speckit.plan command)
├── data-model.md        # Phase 1 output (/speckit.plan command)
├── quickstart.md        # Phase 1 output (/speckit.plan command)
├── contracts/           # Phase 1 output (/speckit.plan command)
│   └── badge_sdk.md     # badge_sdk.py public API contract
└── tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)

```text
firmware/micropython-build/          # canonical source directory (NEW — does not exist yet)
├── boards/
│   └── TEMPORAL_BADGE_DELTA/
│       ├── mpconfigboard.h          # board name, USB-CDC, PSRAM, no BT stack
│       ├── mpconfigboard.cmake      # IDF target esp32s3, sdkconfig layers, frozen manifest
│       ├── sdkconfig.board          # 8 MB QIO, HMAC peripheral, SPIRAM, partitions path
│       ├── partitions.csv           # NVS(24K) + factory(4M) + vfs(4M)
│       ├── manifest.py              # freeze ssd1306 + urequests (no _wifi_creds for this spec)
│       └── pins.csv                 # GPIO → board name (must match constitution table)
├── modules/                         # C extension directory (empty for this spec)
│   └── .gitkeep                     # keeps directory in git
├── config.py                        # GPIO pins, timing constants, BYPASS flag
├── boot.py                          # WiFi+NTP+QR display boot sequence
├── main.py                          # enrollment wait, nametag fetch, main interactive loop
├── ir_nec.py                        # NEC TX/RX via esp32.RMT; IRQRX thread
├── badge_sdk.py                     # Badge class: uid(), display(), ping(), pings()
├── graphics.py                      # Python bytes literals from graphics.h PROGMEM arrays
├── build.sh                         # reproducible build (pins MP v1.24.0 + IDF v5.2.3)
└── flash.sh                         # esptool erase+flash + mpremote VFS copy

```

**Structure Decision**: Single embedded-firmware project rooted at `firmware/micropython-build/`. No frontend/backend split. Python VFS files live at root level (copied verbatim to badge VFS by `flash.sh`). Board definition self-contained under `boards/TEMPORAL_BADGE_DELTA/`. The `modules/` directory is reserved for future badge_crypto C extension (a separate spec); this spec leaves it empty.

**Reference material used during design**: `firmware/shy-micropython-build/` (from `remotes/origin/shy-micropython`) provides validated board definition patterns (cmake layer order, manifest format, build.sh structure, mpy-cross flag). **Files from that directory MUST NOT be copied wholesale** — pin assignments are wrong and the firmware scope (HMAC, NVS UUID, enrollment) does not match this spec.

## Complexity Tracking

> No Constitution violations requiring justification.

