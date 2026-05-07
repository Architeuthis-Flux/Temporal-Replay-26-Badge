# Contract: `badge` Python Module API (spec-008 additions)

**Module**: `badge` (C extension, `badge_mp/badge_module.c`)
**Version after spec-008**: v1.1 (additive — no breaking changes)
**Stability**: Stable. Changes treated as API breaks per constitution Principle VII.

This document covers only the **new or changed** surface introduced by spec-008.
For the full v1 module surface, see `firmware/Firmware-0308-modular/apps/README.md`.

---

## New Function: `badge.ping()`

**Signature**: `badge.ping() -> bool`

**Description**: Non-blocking backend reachability probe. Issues a `GET` request to
`/api/v1/badge/{uid}/info` (auth-exempt) and returns `True` if the backend responds with
HTTP 200. Returns `False` on any network failure, timeout, or non-200 response.

**Parameters**: None

**Returns**:
- `True` — backend is reachable and the badge's info endpoint returned HTTP 200
- `False` — WiFi not connected, connection timeout, DNS failure, or non-200 HTTP response

**Raises**: Never raises. All network errors produce `False`, not exceptions.

**Timing**: Blocks for up to 8 seconds (HTTPClient default timeout). Callers should not
call `badge.ping()` in a tight loop without a `delay()` or button check between calls.

**Example**:
```python
import badge

if badge.ping():
    badge.display.clear()
    badge.display.text("Online", 0, 0)
    badge.display.show()
else:
    badge.display.clear()
    badge.display.text("Offline", 0, 0)
    badge.display.show()
```

**Implementation path**:
```
badge.ping()  →  badge_ping_fn()  [badge_mp/badge_http_mp.c]
              →  BadgePython_probe_backend()  [BadgePython_bridges.cpp]
              →  BadgeAPI::probeBadgeExistence(uid_hex)  [BadgeAPI.cpp]
              →  GET /api/v1/badge/{uid}/info
```

---

## New Functions: `badge.ir_start()` / `badge.ir_stop()`

**Signatures**: `badge.ir_start() -> None` / `badge.ir_stop() -> None`

**Description**: Explicitly enable or disable IR receive mode. IR receive is off by default
to conserve power. Call `badge.ir_start()` before polling `badge.ir_available()` /
`badge.ir_read()`. Call `badge.ir_stop()` when done. The firmware automatically calls
the stop bridge when a Python app exits (clean exit, exception, or badge.exit()), so
a forgotten `ir_stop()` does not leave the receiver permanently armed.

`badge.ir_send()` does not require `ir_start()` — TX is fire-and-forget and manages
its own hardware state per send.

**Raises**: Never raises.

**Example**:
```python
import badge

badge.ir_start()
badge.display.clear()
badge.display.text("Listening...", 0, 0)
badge.display.show()

for _ in range(100):
    if badge.ir_available():
        frame = badge.ir_read()          # returns (addr, cmd) tuple
        badge.display.clear()
        badge.display.text(str(frame), 0, 0)
        badge.display.show()
        break
    if badge.button_pressed(badge.BTN_RIGHT):
        break

badge.ir_stop()
```

**Implementation path**:
```
badge.ir_start()  →  badge_ir_start_fn()  [badge_mp/badge_ir_mp.c]
                  →  badge_bridge_ir_start()  [BadgePython_bridges.cpp]
                  →  pythonIrListening = true  [BadgeIR.cpp global]
                  →  irTask arms IrReceiver (Core 0)

badge.ir_stop()   →  badge_ir_stop_fn()   [badge_mp/badge_ir_mp.c]
                  →  badge_bridge_ir_stop()   [BadgePython_bridges.cpp]
                  →  pythonIrListening = false  [BadgeIR.cpp global]
                  →  irTask disarms IrReceiver (Core 0, if not boopListening)
```

---

## Changed Functions: `badge.ir_available()` / `badge.ir_read()`

These were v1 stubs; spec-008 makes them functional. Behavior is unchanged from the
stub contract when `ir_start()` has not been called (queue is empty → available returns
False, read returns None). When `ir_start()` is active, frames received by irTask are
queued and returned by these functions.

| Function | v1 behavior | spec-008 behavior |
|----------|-------------|-------------------|
| `badge.ir_available()` | Always returns `False` | Returns `True` if ≥1 frame queued; `False` otherwise |
| `badge.ir_read()` | Always returns `None` | Returns `(addr, cmd)` tuple and pops frame; `None` if empty |

---

## Existing Functions: No Changes

The following remain unchanged. Documented here for completeness of scope verification.

| Function | Status |
|----------|--------|
| `badge.display.clear()` | ✅ Unchanged |
| `badge.display.text(s, x, y)` | ✅ Unchanged |
| `badge.display.show()` | ✅ Unchanged |
| `badge.button_pressed(pin)` | ✅ Unchanged |
| `badge.tilt_read()` | ✅ Unchanged — returns raw digital read (HIGH/LOW bool) |
| `badge.ir_send(addr, cmd)` | ✅ Unchanged — fire-and-forget TX; does not require ir_start(); use `BADGE_IR_ADDR` (see BadgeIR.h) as addr for badge-to-badge comms |
| `badge.http_get(url)` | ✅ Unchanged |
| `badge.http_post(url, body)` | ✅ Unchanged |
| `badge.uid()` | ✅ Unchanged |
| `badge.server_url()` | ✅ Unchanged |
| `badge.gc_collect()` | ✅ Unchanged |
| `badge.exit()` | ✅ Unchanged |

---

## Tilt Behavior Change (firmware-level, not Python API change)

`badge.tilt_read()` returns the raw tilt sensor value and is unchanged. However, the
**C++ firmware behavior** around tilt-triggered nametag display changes in spec-008:
the nametag now only activates when `renderMode == MODE_MENU`. This is a firmware
behavior change, not a Python API change — Python apps still receive the raw tilt reading.

---

## Removed Constants: `ROLE_ATTENDEE`, `ROLE_STAFF`, `ROLE_VENDOR`, `ROLE_SPEAKER`

These constants (`0x10`, `0x20`, `0x30`, `0x40`) were registered in the v1 Python module
but served no purpose for Python app developers — they encoded the IR NEC address role
nibble, which was a firmware-internal protocol detail. The boop protocol no longer uses
role-encoded addresses (spec-008 simplification). The constants are removed from the
`badge` module globals in `badge_module.c`.

`ROLE_NUM_*` (1–4, representing Attendee/Staff/Vendor/Speaker attendee type) remain in
the C firmware for NVS/pairing use but are not exposed to Python — Python apps have no
need to reason about attendee type at runtime.

`IR_TX_PIN` and `IR_RX_PIN` pin constants are also removed from the Python module —
Python apps should use `badge.ir_start()`/`badge.ir_send()` rather than raw GPIO.

---

## Breaking Change Policy

Per constitution Principle VII: renamed functions, changed return types, and removed
parameters are breaking changes requiring a version bump and migration note.

`badge.ping()`, `badge.ir_start()`, `badge.ir_stop()` are new additive functions — no
version bump required. Removal of `ROLE_*` constants is technically a breaking change
for any app that referenced them, but no production apps exist yet — no migration note
required.
