# Quickstart: Modularize Arduino Badge Firmware

## What this produces

A new Arduino sketch directory `firmware/Firmware-0308-modular/` that compiles identically
to `firmware/Firmware-0308/Firmware-0308.ino` but split into focused modules. No behavior
changes.

## Prerequisites

- Arduino IDE 2.x **or** `arduino-cli` installed
- ESP32 Arduino core 3.x installed (Board: "Seeed XIAO ESP32S3" or generic ESP32-S3)
- Libraries: U8G2, IRremote, ArduinoJson 6.x (all available via Arduino Library Manager)
- Badge hardware: ESP32-S3-MINI-1 (XIAO form factor) with USB-C cable

## Build (arduino-cli)

```bash
# Compile check (substitute your FQBN if different)
arduino-cli compile \
  --fqbn esp32:esp32:XIAO_ESP32S3 \
  firmware/Firmware-0308-modular

# Upload
arduino-cli upload \
  --fqbn esp32:esp32:XIAO_ESP32S3 \
  --port /dev/cu.usbmodem* \
  firmware/Firmware-0308-modular
```

## Build (Arduino IDE)

Open `firmware/Firmware-0308-modular/Firmware-0308-modular.ino` in Arduino IDE 2.x.
Select board "Seeed XIAO ESP32S3". Click Verify (compile), then Upload.

## Configuration

Edit `BadgeConfig.h` before building:

```cpp
const char* WIFI_SSID  = "your-network";
const char* WIFI_PASS  = "your-password";
const char* SERVER_URL = "https://your-backend.example.com";
const bool  BYPASS     = false;  // true to skip live server calls
```

**Important**: `BadgeConfig.h` contains WiFi credentials — it is in `.gitignore` and must
not be committed. Copy `BadgeConfig.h.example` to `BadgeConfig.h` and fill in values.

## File map (where to make each type of change)

| Change | File |
|--------|------|
| WiFi SSID/password, server URL | `BadgeConfig.h` |
| Pairing timeout, poll interval | `BadgeConfig.h` |
| IR NEC role bits, PKT types | `BadgeConfig.h` + `BadgeIR.h` |
| IR state machine, TX/RX logic | `BadgeIR.cpp` |
| Backend API call details | `BadgeAPI.cpp` |
| Add new API endpoint | `BadgeAPI.h` + `BadgeAPI.cpp` only |
| Display rendering, fonts | `BadgeDisplay.cpp` |
| Button debounce, joystick | `BadgeInput.cpp` |
| NVS keys, persistence | `BadgeStorage.cpp` |
| Boot sequence, state machine | `Firmware-0308-modular.ino` |
| XBM graphics assets | `graphics.h` |

## Verify behavior parity

After flashing, confirm against `Firmware-0308/Firmware-0308.ino` behavior:

1. **Boot**: splash + "Connecting to <SSID>..." → connected or "WiFi failed. Entering demo mode."
2. **Unpaired flow**: QR bitmap displayed, countdown timer shown, polling badge endpoint
3. **Paired flow**: badge XBM rendered when tilt-held upright for 1.5s
4. **IR TX**: BTN_UP → sending arrows → waiting → consent modal → "Paired!" or timeout
5. **IR RX**: BTN_DOWN held → listening arrow → receives UID → consent modal → pairing
6. **Demo mode**: IR listening active, BTN_UP shows "Pairing unavail."
7. **NVS**: Reboot after pairing → loads DEMO state and cached QR, shows badge state in corner

## Module boundaries (quick reference)

```
BadgeConfig.h     ← all constants, zero logic
BadgeUID          ← read_uid() + uid_hex — call first in setup()
BadgeDisplay      ← u8g2, mutex, renderScreen(), showModal(), bootPrint()
BadgeStorage      ← NVS save/load — no display, no IR calls
BadgeAPI          ← HTTP functions, typed result structs — no display, no IR state
BadgeIR           ← irTask (Core 0), IrPhase, IrStatus, submitPairing (calls BadgeAPI)
BadgeInput        ← buttons, joystick, tilt, onButtonPressed (sets BadgeIR flags)
*.ino             ← setup(), loop(), osRun + stages, wifiConnect (≤ 150 lines)
```
