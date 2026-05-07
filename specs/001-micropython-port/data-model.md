# Data Model: MicroPython Port for Temporal Badge DELTA

**Phase**: 1 | **Date**: 2026-03-10 | **Feature**: `001-micropython-port`

---

## Entities

### 1. Badge ID

| Field | Type | Source | Constraints |
|-------|------|--------|-------------|
| `uid_bytes` | `bytes` | `machine.unique_id()` | Always 6 bytes on ESP32-S3; zero-padded if shorter |
| `uid_hex` | `str` | derived | 12 lowercase hex characters; `uid_hex = ''.join(f'{b:02x}' for b in uid_bytes).ljust(12,'0')[:12]` |

**Used in**: boot welcome message, IR TX payload, display status text.

---

### 2. XBM Bitmap

| Field | Type | Value |
|-------|------|-------|
| Width | int | 128 pixels |
| Height | int | 64 pixels |
| Encoding | `framebuf.MONO_HLSB` | LSB-first, row-major (matches XBM format) |
| Size | bytes | 1024 bytes (128 × 64 / 8) |

**Sources**:
- **Live mode**: Fetched from `GET /api/v1/badge/<uuid>/qr.xbm` as raw XBM text, parsed by `_parse_xbm_response()`.
- **BYPASS mode**: `graphics.MOCK_QR` or `graphics.MOCK_NAMETAG` (pre-converted bytes literals).
- **Arrow sprites**: `graphics.DOWN_ARROW_FILLED`, `graphics.UP_ARROW_FILLED`, etc. (9×6, 12 bytes each).
- **Base graphic**: `graphics.GRAPHICS_BASE` (128×64, 1024 bytes) — used as main-mode background overlay.

**Validation**: Any fetched XBM blob MUST be ≥ 1024 bytes before rendering. Truncate at 1024 if longer.

**Rendering**:
```python
import framebuf
fb = framebuf.FrameBuffer(bytearray(xbm_bytes[:1024]), 128, 64, framebuf.MONO_HLSB)
display.blit(fb, 0, 0)
display.show()
```

---

### 3. Display Mode

| Mode | Constant | Trigger |
|------|----------|---------|
| Boot | `MODE_BOOT = 0` | Active during boot sequence |
| QR | `MODE_QR = 1` | After WiFi connect; timed 10 s; also BTN_DOWN cycle |
| Main | `MODE_MAIN = 2` | After nametag fetch; default interactive mode |

**State transitions**:
```
BOOT → QR (after QR bitmap ready) → MAIN (after 10 s + nametag fetch)
MAIN ←→ QR   (BTN_UP/BTN_DOWN cycle)
```

---

### 4. Button State

Each of the 4 directional buttons:

| Field | Type | Value |
|-------|------|-------|
| `pin` | GPIO | 8=UP, 9=DOWN, 10=LEFT, 11=RIGHT |
| `state` | bool | `False` = pressed (active-low), `True` = released |
| `last_reading` | bool | Previous raw GPIO value |
| `last_debounce_ms` | int | `utime.ticks_ms()` at last state transition |
| `indicator_x` | int | OLED x pixel for press indicator dot |
| `indicator_y` | int | OLED y pixel for press indicator dot |
| `debounce_ms` | int | 40 ms |

**Indicator positions** (from Firmware-0306):
```
BTN_UP    → (118, 48)
BTN_DOWN  → (118, 58)
BTN_LEFT  → (113, 53)
BTN_RIGHT → (123, 53)
```

**Action on press**: BTN_UP → trigger `irTransmitUID()`.

---

### 5. Joystick State

| Field | Type | Constraints |
|-------|------|-------------|
| `raw_x` | int | 0–4095 (12-bit ADC, GPIO1) |
| `raw_y` | int | 0–4095 (12-bit ADC, GPIO2) |
| `norm_x` | float | ±1.0, clamped to unit circle |
| `norm_y` | float | ±1.0, clamped to unit circle |
| `dot_x` | int | OLED pixel x; center=100, radius=6 |
| `dot_y` | int | OLED pixel y; center=53, radius=6 |

**Poll interval**: 50 ms.

**Normalization**:
```python
nx = (raw_x - 2048) / 2048.0
ny = (raw_y - 2048) / 2048.0
dist = sqrt(nx**2 + ny**2)
if dist > 1.0:
    nx /= dist; ny /= dist
dot_x = 100 + round(nx * 6)
dot_y =  53 + round(ny * 6)
```

---

### 6. Tilt Switch State

| Field | Type | Value |
|-------|------|-------|
| `state` | bool | `False` = tilted (active-low), `True` = upright |
| `last_reading` | bool | Previous raw GPIO value |
| `last_debounce_ms` | int | `utime.ticks_ms()` at last transition |
| `debounce_ms` | int | 300 ms |

**Display**: When tilted (`state == False`), fill rect at (84, 48). When upright, fill rect at (84, 54).

---

### 7. IR NEC Frame

One transmitted/received unit of the NEC protocol:

| Field | Type | Value |
|-------|------|-------|
| `address` | uint8 | 0x42 (badge-to-badge protocol) |
| `command` | uint8 | One byte of the 6-byte badge UID |
| `address_inv` | uint8 | `~address & 0xFF` = 0xBD |
| `command_inv` | uint8 | `~command & 0xFF` |

**Full NEC frame bit pattern (MSB first in wire order, LSB first in RMT)**:
`leader_burst(9ms) + leader_space(4.5ms) + [address LSB→MSB] + [~address LSB→MSB] + [command LSB→MSB] + [~command LSB→MSB] + stop_burst(562µs)`

---

### 8. IR UID Transmission

One complete badge-ID transmission = 6 consecutive NEC frames.

| Field | Type | Value |
|-------|------|-------|
| Frames | int | 6 |
| Address | uint8 | 0x42 |
| Commands | bytes | `uid_bytes[0..5]` (6 MAC bytes) |
| Inter-frame gap | int | 50 ms |
| Total TX time | int | ~6 × 110 ms = ~660 ms plus gaps = ~960 ms |

---

### 9. IR UID Reception Buffer

State machine accumulating a 6-byte UID from incoming NEC frames:

| Field | Type | Value |
|-------|------|-------|
| `rx_buf` | list | 0–6 command bytes received so far |
| `rx_count` | int | 0–6 |
| `last_byte_ms` | int | `utime.ticks_ms()` of last received byte |
| `timeout_ms` | int | 500 ms — discard partial buffer on silence |

**State transitions**:
```
IDLE → receiving byte 1/6 (addr==0x42)
receiving → receiving byte N/6 (each new NEC frame with addr 0x42, < 500ms gap)
receiving → UID complete (6 bytes received → assemble hex string → post to _received_uids)
receiving → IDLE (500 ms gap with no new frame → discard rx_buf)
```

**Non-badge NEC**: Any NEC frame with `address != 0x42` is displayed as "IR: {protocol}" for 1 s, then returns to "IR: listening...".

---

### 10. VFS Python Files

Files copied to the badge VFS by `flash.sh`. Available to `import` at runtime.

| File | Description | Sensitive? |
|------|-------------|-----------|
| `config.py` | GPIO pins, timing constants, BYPASS flag | No — VFS-resident intentionally |
| `boot.py` | Boot sequence: WiFi, NTP, QR display | No |
| `main.py` | Main interactive loop; nametag mode | No |
| `ir_nec.py` | NEC TX/RX class implementations | No |
| `badge_sdk.py` | `Badge` class — public app SDK | No |
| `graphics.py` | XBM bytes literals from graphics.h | No |
| `creds.py` | WiFi SSID/PASS, SERVER_URL | **Yes — gitignored; never commit** |

---

### 11. Board Configuration Files

Files in `boards/TEMPORAL_BADGE_DELTA/` that define the MicroPython port:

| File | Purpose |
|------|---------|
| `mpconfigboard.h` | Board name macro, USB-CDC, no BT stack |
| `mpconfigboard.cmake` | IDF target, sdkconfig layer order, frozen manifest path |
| `sdkconfig.board` | 8 MB QIO, SPIRAM, HMAC peripheral enable |
| `partitions.csv` | NVS(24KB) + factory(4MB) + vfs(4MB) |
| `manifest.py` | Freeze `ssd1306`, `urequests` |
| `pins.csv` | GPIO → `BOARD_*` name mapping (must match constitution table) |

---

## State Transitions: Boot Sequence

```
power-on
  └─→ [BOOT mode] display "Temporal / Badge DELTA / Starting..."
       ├─ BYPASS=True
       │    └─ delay BYPASS_DELAY_MS
       │         └─ display "Connected (bypass)"
       └─ BYPASS=False
            └─ WiFi connect (WIFI_TIMEOUT_MS=5000)
                 └─ on failure: display "WiFi failed", halt
                 └─ on success: display "Connected to <SSID>"
                      └─ NTP sync (BYPASS skips)
                           └─ fetch QR bitmap (BYPASS uses MOCK_QR)
                                └─ [QR mode] display QR for QR_DISPLAY_MS=10000
                                     └─ fetch nametag (BYPASS uses MOCK_NAMETAG)
                                          └─ [MAIN mode] main event loop (never returns)
```

---

## Config Constants (config.py)

| Name | Type | Default | Description |
|------|------|---------|-------------|
| `I2C_SDA` | int | 5 | SSD1309 SDA |
| `I2C_SCL` | int | 6 | SSD1309 SCL |
| `I2C_FREQ` | int | 400000 | I2C frequency Hz |
| `BTN_UP` | int | 8 | Button GPIO (active-low) |
| `BTN_DOWN` | int | 9 | Button GPIO (active-low) |
| `BTN_LEFT` | int | 10 | Button GPIO (active-low) |
| `BTN_RIGHT` | int | 11 | Button GPIO (active-low) |
| `JOY_X_PIN` | int | 1 | Joystick X ADC GPIO |
| `JOY_Y_PIN` | int | 2 | Joystick Y ADC GPIO |
| `TILT_PIN` | int | 7 | Tilt switch GPIO (active-low) |
| `IR_TX_PIN` | int | 3 | IR transmit GPIO |
| `IR_RX_PIN` | int | 4 | IR receive GPIO (TSOP output) |
| `WIFI_TIMEOUT_MS` | int | 5000 | WiFi connect timeout |
| `QR_DISPLAY_MS` | int | 10000 | QR display duration |
| `BYPASS_DELAY_MS` | int | 3000 | BYPASS mode simulated delay |
| `TILT_SHOWS_BADGE` | bool | False | Future: tilt → nametag mode |
| `BYPASS` | bool | True | Dev flag; False for conference |
