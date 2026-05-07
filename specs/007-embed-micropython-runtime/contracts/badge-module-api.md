# Contract: `badge` Python Module API

**Module**: `badge` (C extension, registered via `MP_REGISTER_MODULE`)
**Version**: 1.0
**Source**: spec.md FR-005, clarified 2026-03-14
**Stability**: Stable per Principle VII — breaking changes require versioned migration

---

## Import

```python
import badge
```

All hardware and service access is through this single module. `badge.display` is a
sub-object accessible as an attribute — it does not need a separate import.

---

## Display Sub-Object (`badge.display`)

`badge.display` is a nested module object created once at VM init. No per-call allocation.
All display calls acquire `displayMutex` before invoking U8G2 and release it after.

Coordinate system: origin (0, 0) at top-left. Screen is 128×64 pixels.

### `badge.display.clear() → None`

Clears the display buffer. Does **not** flush to the OLED — call `badge.display.show()`.

### `badge.display.text(s, x, y) → None`

Draw string `s` at pixel position (x, y).

| Parameter | Type | Notes |
|-----------|------|-------|
| `s` | `str` | UTF-8 string |
| `x` | `int` | 0–127 |
| `y` | `int` | 0–63 |

### `badge.display.show() → None`

Flushes the display buffer to the OLED (`u8g2.sendBuffer()`). Acquires mutex for the full flush.

---

## Input

Input functions read GPIO state directly. No debounce — apps that need it should implement
it in Python with `utime.sleep_ms`.

### `badge.button_pressed(btn) → bool`

Returns `True` if the button is currently pressed (GPIO reads LOW — active-low with pull-up).

```python
if badge.button_pressed(badge.BTN_UP):
    # handle up
```

**Button constants**:

| Constant | GPIO | Physical pin |
|----------|------|--------------|
| `badge.BTN_UP` | 44 | D7 |
| `badge.BTN_DOWN` | 7 | D8 |
| `badge.BTN_LEFT` | 8 | D9 |
| `badge.BTN_RIGHT` | 9 | D10 |

> **Note**: Values are raw GPIO numbers resolved from the XIAO ESP32-S3 variant header
> (`D7=GPIO44, D8=GPIO7, D9=GPIO8, D10=GPIO9`). Do not use D-pin aliases in Python.

### `badge.tilt_read() → bool`

Returns `True` if the badge is tilted (TILT_PIN D6=GPIO43 reads LOW).

**Joystick and tilt constants**:

| Constant | GPIO | Physical pin | Use |
|----------|------|--------------|-----|
| `badge.JOY_X` | 1 | D0 | Joystick X axis (ADC) |
| `badge.JOY_Y` | 2 | D1 | Joystick Y axis (ADC) |
| `badge.TILT_PIN` | 43 | D6 | Tilt sensor (digital) |
| `badge.IR_TX_PIN` | 3 | D2 | IR transmit |
| `badge.IR_RX_PIN` | 4 | D3 | IR receive |

Use joystick constants with `badge.adc_read()` and tilt/IR constants with `badge.pin_read()`.

---

## Identity

### `badge.uid() → str`

Returns the badge's eFuse-derived UID as a 12-character lowercase hex string (e.g. `"a1b2c3d4e5f6"`).
The value is read once at boot from `BadgeUID` and cached.

### `badge.server_url() → str`

Returns the backend server base URL configured in `BadgeConfig.h` (e.g. `"https://badge.example.com"`).
Use this with `badge.uid()` to construct API URLs without hardcoding server addresses.

```python
uid = badge.uid()
server = badge.server_url()
url = server + "/api/v1/badge/" + uid + "/info"
body = badge.http_get(url)
```

---

## WiFi / HTTP

Both functions are **blocking** — they make synchronous HTTP calls on Core 1. WiFi must
already be connected (the boot sequence ensures this before Python apps can run).

Both functions raise `OSError` on failure. Success returns the response body as a string.

### `badge.http_get(url) → str`

Perform an HTTP GET request.

| Parameter | Type | Notes |
|-----------|------|-------|
| `url` | `str` | Full URL including scheme |

Returns response body string on HTTP 2xx.
Raises `OSError` on non-2xx status or connection failure.

```python
try:
    body = badge.http_get("https://example.com/api/hello")
except OSError as e:
    badge.display.text("HTTP error", 0, 32)
    badge.display.show()
```

### `badge.http_post(url, body) → str`

Perform an HTTP POST request with `body` as the request body string.
`Content-Type: application/json` is set automatically.

Returns response body string on HTTP 2xx.
Raises `OSError` on non-2xx status or connection failure.

```python
import ujson
payload = ujson.dumps({"key": "value"})
result = badge.http_post("https://example.com/api/data", payload)
```

---

## IR

The IR ISR runs on Core 0 and continues buffering received frames while Python runs on
Core 1. Python apps poll via these functions.

**IR polling requirement**: Apps that use IR MUST call `badge.ir_read()` within a 50ms
polling interval to avoid IRremote buffer overflow.

### `badge.ir_send(addr, cmd) → None`

Transmit an NEC IR frame.

| Parameter | Type | Notes |
|-----------|------|-------|
| `addr` | `int` | NEC address byte (role-encoded) |
| `cmd` | `int` | NEC command byte |

### `badge.ir_available() → bool`

Returns `True` if there is at least one received IR frame waiting in the buffer.

### `badge.ir_read() → tuple | None`

Read one frame from the IR receive buffer. Returns `(addr, cmd)` tuple if a frame is
available, or `None` if the buffer is empty.

```python
frame = badge.ir_read()
if frame is not None:
    addr, cmd = frame
```

**Role address constants**:

| Constant | Value | Meaning |
|----------|-------|---------|
| `badge.ROLE_ATTENDEE` | `0x10` | Attendee role NEC address |
| `badge.ROLE_STAFF` | `0x20` | Staff role NEC address |
| `badge.ROLE_VENDOR` | `0x30` | Vendor role NEC address |
| `badge.ROLE_SPEAKER` | `0x40` | Speaker role NEC address |

---

## Hardware (GPIO / ADC)

Direct GPIO and ADC access. Use with care — the C++ firmware owns most pins. Safe to use
on pins not already assigned to I2C, IR, buttons, or tilt.

### `badge.pin_read(n) → int`

`digitalRead(n)` — returns 0 or 1.

### `badge.pin_write(n, v) → None`

`digitalWrite(n, v)`.

### `badge.adc_read(n) → int`

`analogRead(n)` — returns 0–4095 (12-bit).

---

## GC Control

### `badge.gc_collect() → None`

Forces an immediate MicroPython GC cycle. Call this before blocking HTTP calls or at the
top of draw loops to avoid uncontrolled GC pauses (worst-case ≤ 20ms per spec SC-007).

```python
import utime

while True:
    badge.gc_collect()           # choose your own safe point
    badge.display.clear()
    badge.display.text("Hi", 0, 32)
    badge.display.show()
    utime.sleep_ms(100)
```

---

## App Exit

### `badge.exit() → None`

Raises `SystemExit` in the Python interpreter, terminating the app and returning to the
main menu. This is the **expected** exit path for apps. Calling `badge.exit()` is equivalent
to the script reaching its last line.

```python
import utime

badge.display.clear()
badge.display.text("Done!", 30, 32)
badge.display.show()
utime.sleep_ms(1500)
badge.exit()
```

---

## Emergency Escape (not a module function)

Holding **BTN_UP + BTN_DOWN simultaneously** for ≥ 50ms triggers emergency termination
of the running app. This is handled by a FreeRTOS timer in `BadgePython.cpp` — apps do
not need to poll for it. The badge returns to the main menu within 1 second.

This escape hatch is for misbehaving apps only. Normal apps should exit via `badge.exit()`
or natural end-of-script.

---

## Available Built-ins

Standard MicroPython modules enabled via `mpconfigport.h`:

| Module | Key functions |
|--------|---------------|
| `utime` | `sleep_ms(n)`, `sleep(s)`, `ticks_ms()`, `ticks_diff(a, b)` |
| `math` | Standard math functions |
| `ujson` | `dumps(obj)`, `loads(str)` |

Modules **not** available: `machine`, `esp32`, `network`, `socket`, `urequests` (blocked by omission from `mpconfigport.h`).

---

## Error Behavior

| Situation | Outcome |
|-----------|---------|
| Unhandled Python exception | `BadgePython::execApp` returns; firmware shows error, returns to menu |
| `badge.exit()` | `SystemExit` raised; caught as clean exit; returns to menu |
| BTN_UP + BTN_DOWN chord | `KeyboardInterrupt` scheduled; caught; returns to menu |
| `badge.http_get/post` non-2xx | Raises `OSError` in Python script |
| `badge.http_get/post` connection failure | Raises `OSError` in Python script |
| Invalid argument type | `TypeError` raised in Python script |
| GC heap exhausted | `MemoryError` raised; caught by firmware if unhandled; returns to menu |
| Script source > `MP_SCRIPT_MAX_BYTES` | Error displayed before exec; returns to menu without running |

---

## Stability Policy (Principle VII)

- **Breaking changes** (renamed functions, removed functions, changed return types or param types) MUST be treated as a major version bump, communicated to app authors, and documented with a migration note before shipping.
- **Additive changes** (new functions, new optional parameters with defaults) are non-breaking.
- This policy applies to the `badge.display.*` sub-object, all top-level `badge.*` functions, and all `badge.*` constants.
