# MicroPython Module Reference

The Temporal Badge runs MicroPython v1.27.0 as an embed port inside the
Arduino firmware. Users connecting via mpremote, ViperIDE, or JumperIDE get
a full REPL with the modules listed below.

## Badge-specific module

| Import | Description |
|--------|-------------|
| `badge` | OLED display, LED matrix, buttons, joystick, IR, IMU, haptics, HTTP, mouse overlay, key-value storage, identity. Auto-imported at boot (`from badge import *`). See `firmware/initial_filesystem/docs/API_REFERENCE.md`. |

## Standard library

| Module | Notes |
|--------|-------|
| `sys` | `sys.path`, `sys.exit()`, `sys.platform` ("esp32"), `sys.ps1`/`sys.ps2` |
| `os` | VFS-backed: `listdir`, `stat`, `statvfs`, `uname`, `urandom`, `dupterm` |
| `time` | `sleep`, `sleep_ms`, `sleep_us`, `ticks_ms`, `ticks_us`, `ticks_diff`, `gmtime`, `localtime`, `mktime`, `time_ns` |
| `math` | Full single-precision float math |
| `cmath` | Complex number math |
| `random` | `random()`, `randint()`, `choice()`, `uniform()`, `randrange()`, extra funcs |
| `json` | `dumps()`, `loads()` |
| `binascii` | `hexlify()`, `unhexlify()`, `a2b_base64()`, `b2a_base64()` (no CRC32) |
| `heapq` | Min-heap operations |
| `errno` | POSIX error constants |
| `uctypes` | Structured binary data access |
| `gc` | `gc.collect()`, `gc.mem_free()`, `gc.mem_alloc()` |
| `io` | `StringIO`, `BytesIO`, `BufferedWriter` |
| `asyncio` | `asyncio.run()`, tasks, events, locks, streams (C helper + pure Python) |
| `select` | `poll()`, `ipoll()` for async stream IO |
| `micropython` | `mem_info()`, `stack_use()`, `schedule()` |

## machine module

All classes are from the upstream `ports/esp32` implementation.

| Class | Status | Notes |
|-------|--------|-------|
| `machine.Pin` | OK | GPIO with IRQ support. See `firmware/micropython/embed_config/temporal_badge_pins.csv` for the board pin table. |
| `machine.ADC` | OK | 12-bit ADC with attenuation control. `ADC.read()`, `ADC.read_uv()`. |
| `machine.ADCBlock` | OK | ADC channel grouping. |
| `machine.PWM` | OK | Hardware LEDC PWM. `PWM.duty()`, `PWM.freq()`. |
| `machine.Timer` | OK | Hardware timers (4 available). `Timer(id)`, `.init(period=ms, callback=fn)`. |
| `machine.WDT` | OK | Task watchdog. `WDT(timeout=5000)`, `.feed()`. Coexists with Arduino's WDT. |
| `machine.UART` | OK | Hardware UART with IRQ. |
| `machine.SPI` | OK | Hardware SPI (HSPI/VSPI). |
| `machine.SoftSPI` | OK | Bit-banged SPI on any pins. |
| `machine.SoftI2C` | OK | Bit-banged I2C on any pins. |
| `machine.I2S` | OK | Inter-IC Sound with DMA. |
| `machine.RTC` | OK | Real-time clock, wake configuration. |
| `machine.TouchPad` | OK | Capacitive touch on supported GPIOs. |
| `machine.Pin.board` | OK | Board pin aliases from pin table. |
| `machine.I2C` | OFF | Conflicts with Arduino Wire's `driver_ng`. Use `SoftI2C`. |
| `machine.Signal` | OFF | Not needed for badge hardware. |

Additional `machine` functions: `freq()`, `reset()`, `soft_reset()`,
`unique_id()`, `idle()`, `lightsleep()`, `deepsleep()`, `disable_irq()`,
`enable_irq()`, `wake_reason()`, `time_pulse_us()`.

## Networking

WiFi is managed by Arduino's `WiFiService` for `BadgeAPI` HMAC calls.
MicroPython's network stack piggybacks on the existing driver — it never
re-initializes `esp_wifi`. Python code can read status without disrupting
the badge's connection.

| Module | Status | Notes |
|--------|--------|-------|
| `network.WLAN` | OK | `.active()`, `.status()`, `.config('rssi')`, `.ifconfig()`, `.scan()`, `.connect()`. Shares WiFi with Arduino. |
| `socket` | OK | BSD sockets: TCP/UDP client and server. |
| `ssl` | OK | TLS via mbedTLS (already linked by Arduino). `ssl.wrap_socket()`, `SSLContext`. |
| `websocket` | OK | WebSocket framing. |
| `webrepl` | OK | WebREPL server for browser-based access. |

## ESP-NOW

| Module | Status | Notes |
|--------|--------|-------|
| `espnow.ESPNow` | OK | Peer-to-peer wireless. `.active(True)`, `.send(peer, msg)`, `.recv()`. Requires WiFi active (badge boots WiFi for `BadgeAPI`). |

## Gated modules (off by default)

These can be enabled via `-D` flags in `platformio.ini`:

| Flag | Module | Why gated |
|------|--------|-----------|
| `REPLAY_ENABLE_THREAD=1` | `_thread` | GIL event-poll hook conflicts with the embed port's single-task execution model. Infrastructure is wired but crashes during Python exec. |
| `REPLAY_ENABLE_BLUETOOTH=1` | `bluetooth` (NimBLE) | Arduino ships pre-compiled NimBLE without private host headers (`ble_hs_pvcy_priv.h`). Requires vendoring the full NimBLE source tree. |

## Filesystem

| Feature | Details |
|---------|---------|
| Format | FAT on wear-levelled flash partition (`ffat`, 7 MB) |
| Mount point | `/` |
| VFS | MicroPython native VFS with `open()`, `os.listdir()`, `os.stat()` |
| Persistence | Files survive reboot; `os.statvfs('/')` shows free space |

## Hardware pin mapping

| Signal | GPIO | Badge alias |
|--------|------|-------------|
| I2C SDA | 5 | D4 |
| I2C SCL | 6 | D5 |
| IR TX | 3 | D2 |
| IR RX | 4 | D3 |
| BTN_UP | 44 | D7 |
| BTN_DOWN | 7 | D8 |
| BTN_LEFT | 8 | D9 |
| BTN_RIGHT | 9 | D10 |
| JOY_X | 1 | D0 |
| JOY_Y | 2 | D1 |
| TILT | 43 | D6 |

## REPL access

Connect via USB serial at any baud (USB-Serial/JTAG ignores baud rate).
The banner shows:

```
MicroPython v1.27.0 on 2026-05-14; Replay Badge v0.2.7 with ESP32-S3
Type "help()" for more information.
>>>
```

Serial commands (outside the REPL, at 115200 baud):
- `test` — run all self-tests
- `run:<app>` — execute `apps/<app>.py`
- `list` — list available apps

Escape chord: hold all four face buttons to force-exit a running app.
