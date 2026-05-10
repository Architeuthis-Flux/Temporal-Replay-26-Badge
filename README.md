# Temporal Badge

Conference badge for Temporal's events. ESP32-S3-MINI-1 (XIAO form factor) with SSD1309
OLED, IR TX/RX for attendee pairing, joystick, tilt switch, and NVS-backed identity.

## Hardware

- **MCU**: ESP32-S3 16r8, dual-core Xtensa LX7, 8 MB flash
- **Display**: SSD1309 128×64 OLED via hardware I2C (SDA=D4/GPIO5, SCL=D5/GPIO6)
- **IR**: NEC protocol TX (D2/GPIO3) + RX (D3/GPIO4)
- **Input**: 4 buttons (D7–D10), analog joystick (D0/D1), tilt switch (D6)

## Prerequisites

- Python 3.9+
- All other dependencies (PlatformIO, Temporal CLI, esptool) are installed by `ignition/setup.sh`

## Build & Flash (Ignition)

```bash
cd ignition
./setup.sh        # first time only — installs all dependencies
./start.sh        # build + flash all connected badges
./start.sh --build-and-flash  # explicit build + fresh filesystem + flash
./start.sh -y     # skip the pre-flash Enter prompt for a ready batch
```

Ignition treats each run as one connected hub batch. Temporal starts a parent
`FlashBadgesWorkflow` plus one human-readable `BadgeFlashWorkflow` child per
detected badge, so the UI shows per-badge flash/WiFi progress and results.

See [`ignition/README.md`](ignition/README.md) for full usage, including `--no-build`, `--build-only`, and `--firmware-dir` options.

## Build (Firmware)

```bash
cd firmware

# Echo badge (default)
./build.sh

# Charlie board
./build.sh charlie

# Non-interactive build (skip build-time network prompt)
./build.sh echo -n
```

Or directly via PlatformIO:
```bash
pio run -e echo
pio run -e charlie
```
Direct PlatformIO builds read `firmware/wifi.local.env` or `BADGE_*`
environment variables and fail if WiFi credentials or the server URL are
missing.

### Dev menu build

Normal firmware hides internal debug/test screens. To flash an Echo badge with
the dev menu enabled for diagnostics and badge-info editing:

```bash
cd firmware
pio run -e echo-dev -t upload --upload-port /dev/cu.usbmodem1101
```

`echo-dev` still uses the configured boop API; it only changes menu/debug
visibility. Use a local settings/server override if you need to avoid staging.

Normal firmware boots straight into the badge menu. Production builds show
Boop, Messages, Contacts, New Msg, Settings, and QR / Pair only while the
badge is unpaired; debug/test screens stay behind the dev menu build.

### MicroPython apps

MicroPython app source lives in `firmware/initial_filesystem/apps/` and is
embedded into firmware builds through `firmware/scripts/generate_startup_files.py`.
Use folder apps with a `main.py` entry point for anything larger than a demo.

Normal firmware does not show the generic Apps menu. Attendee-facing
MicroPython apps need a native `GUI.cpp` launcher and icon; diagnostics can
stay in the dev-menu Apps browser. See
[`firmware/initial_filesystem/apps/README.md`](firmware/initial_filesystem/apps/README.md)
for the full authoring, validation, and build workflow.

### Serial logging

To capture logs from every connected badge while testing hardware flows:

```bash
cd firmware
~/.platformio/penv/bin/python serial_log.py
```

The logger auto-detects USB serial ports, clears `firmware/logs/` by default,
and writes one log file per badge.

### Two-badge hardware test harness

The planned firmware hardware harness uses Ignition to build/flash two badges,
then drives them over USB serial for physical boop and clean-state tests.
See [`docs/hardware-test-harness.md`](docs/hardware-test-harness.md) and
[`specs/012-hardware-test-harness/tasks.md`](specs/012-hardware-test-harness/tasks.md).

## Power Management

The badge uses a 1000 mAh battery drawing ~10 mA in normal operation (~100 hours).
Power saving is tuned for multi-day conferences where WiFi reconnects (~400 mA
for several seconds) cost more than the deep sleep saves if triggered too often.

| State | Trigger | What happens |
|-------|---------|--------------|
| **Active** | Motion / button press | Full brightness, WiFi connected |
| **Dim** | 5 min no motion | LED matrix dims to brightness 3, WiFi stays connected |
| **Deep sleep** | 20 min no motion | 12 μA draw, WiFi disconnects, wakes on IMU motion |
| **Deep sleep (unpaired)** | 5 min no motion | Unpaired badges sleep sooner to save battery in storage |
| **Force sleep** | Hold **UP** button 5 seconds | Immediately enters deep sleep (disabled over USB) |

To wake from deep sleep, move or tilt the badge. After waking, WiFi reconnects
automatically.

## Configuration

Display, input, boop, and timing settings live in `settings.txt` on the
badge's FAT filesystem — no recompile needed. WiFi credentials and the server
URL are build-time secrets and are not written to the badge filesystem.

To configure a badge:
1. Copy `firmware/wifi.local.env.example` to `firmware/wifi.local.env`:
```
BADGE_WIFI_SSID=YourNetwork
BADGE_WIFI_PASS=YourPassword
BADGE_SERVER_URL=https://your-server.example.com
```
2. Build/flash from `firmware/`; the build embeds those values into the firmware.
3. Edit non-secret settings in `settings.txt` as needed:
```
[wifi]
ep_badge = /api/v1/badge
ep_boops = /api/v1/boops
```

`firmware/wifi.local.env` is ignored by git. Environment variables with the
same names override that file for one-off builds. Changing WiFi or backend URL
now requires rebuilding and reflashing the firmware.
Builds require `BADGE_WIFI_SSID`, `BADGE_WIFI_PASS`, and `BADGE_SERVER_URL`;
the badge depends on API connectivity for pairing, messaging, and schedule data.
Badge-facing API calls use MessagePack where supported. Schedule data uses a
compact MessagePack schema and is cached on-device with ETag sidecars. The
schedule app supports `MINE` and `ALL` modes, refreshes in the background on
open, and shows a ticket-page QR for editing an attendee's custom schedule.
Boop and ping list reads also use compact MessagePack arrays; see
[`firmware/src/README.md`](firmware/src/README.md#backend-wire-format) for the
wire contract and measured payload savings.

## Pairing QR

The on-device QR code encodes the badge's compact 12-character machine id,
uppercased for QR alphanumeric mode. The registration scanner uses that id to
attach the physical badge to an attendee record and the badge then fetches the
server-owned identity data.

## Serial Monitor

```bash
cd firmware
pio device monitor --port /dev/cu.usbmodem114101 --baud 115200
```

Or use `./flash.py` (default behavior) to flash and then auto-open monitor logs for one badge.

## Hardware Targets

| Target | Board | Defines file |
|--------|-------|------|
| `delta` | Temporal Badge DELTA (ESP32-S3-MINI-1, XIAO form factor) | `firmware/src/DeltaDefines.h` |
| `charlie` | Jumperless/Charlie board (ESP32-S3-DevKitC-1-N16R8V) | `firmware/src/CharlieDefines.h` |
| `echo` | Echo board production firmware | `firmware/src/EchoDefines.h` |
| `echo-dev` | Echo board with internal dev menu enabled | `firmware/src/EchoDefines.h` |

### Adding a new hardware target

1. Copy `firmware/src/DeltaDefines.h` → `firmware/src/MyBoardDefines.h`
2. Update pin numbers to match your schematic
3. Add `BADGE_HAS_*` flags for any optional peripherals your board has (see `firmware/src/CharlieDefines.h` for the full list)
4. Add a new `[env:myboard]` section to `firmware/platformio.ini` with `-DHARDWARE_MYBOARD`
5. Add `#include "MyBoardDefines.h"` to `firmware/src/HardwareConfig.h`

## Key Source Files

- [firmware/src/main.cpp](firmware/src/main.cpp) — setup() and loop()
- [firmware/src/HardwareConfig.h](firmware/src/HardwareConfig.h) — hardware target selector
- [firmware/src/DeltaDefines.h](firmware/src/DeltaDefines.h) — DELTA pin assignments
- [firmware/src/CharlieDefines.h](firmware/src/CharlieDefines.h) — Charlie pin assignments + feature flags
- [firmware/platformio.ini](firmware/platformio.ini) — build environments

## Repo Structure

| Directory | Contents |
|-----------|----------|
| [`firmware/`](firmware/) | Arduino C++ firmware — active PlatformIO project |
| [`hardware/DELTA/`](hardware/DELTA/) | KiCad PCB — ESP32-S3-MINI-1 XIAO, SSD1309 OLED, IR, buttons |
| [`registrationScanner/`](registrationScanner/) | Quart + Temporal backend (badge enrollment + boop workflows) |
| [`specs/`](specs/) | Feature specs and implementation plans |
| [`assets/`](assets/) | Art and design resources |
| [`docs/`](docs/) | Design notes, photos, work log |

## Legacy Firmware

`firmware/Firmware-0308-modular/` and `firmware/Firmware-0308/` are previous Arduino IDE builds kept for reference. They use a different build system and are not maintained.
