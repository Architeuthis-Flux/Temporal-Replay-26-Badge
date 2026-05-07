# Contract: badge_sdk.py — Badge App SDK

**Phase**: 1 | **Date**: 2026-03-10 | **Feature**: `001-micropython-port`

This document defines the stable public Python API exposed by `badge_sdk.py`. Badge apps deployed to the VFS import this module and depend on this contract. Changes to this interface require a new spec.

---

## Overview

`badge_sdk.py` provides the `Badge` class — the entry point for all badge apps. It abstracts hardware access (UID, display, IR) into a clean Python API that badge apps can use without knowing GPIO pin numbers or IR protocol details.

**Scope for this spec**: IR-only `ping()/pings()`. No HTTP boops/pings, no HMAC authentication, no backend calls. Those are defined in the constitution's Badge App Platform section and belong to a future spec.

---

## Import

```python
from badge_sdk import Badge
```

---

## class Badge

### Constructor

```python
b = Badge()
```

Initializes the badge hardware:
- Reads UID from `machine.unique_id()`
- Initializes the SSD1309 I2C display
- Starts the IR RX background thread

No network calls. Safe to construct before WiFi is connected.

**Side effects**: Starts `_thread` for IR RX. Sets display to blank.

---

### Properties

#### `Badge.uid` → `str`

```python
uid = b.uid   # e.g. "a1b2c3d4e5f6"
```

Returns the 12-character lowercase hex badge ID derived from `machine.unique_id()` (6 MAC bytes). This value is stable for the lifetime of the `Badge` instance and does not change between calls.

**Guarantees**:
- Always exactly 12 characters
- Always lowercase hex
- Never `None`

---

#### `Badge.display` → `ssd1306.SSD1306_I2C`

```python
b.display.fill(0)
b.display.text("Hello", 0, 0)
b.display.show()
```

The raw `ssd1306.SSD1306_I2C` display instance (128×64 pixels). Badge apps MAY use it directly for custom rendering.

**Warning**: If `boot.py` has already initialized a display instance and exported it, apps MUST use the same I2C bus. The `Badge` class handles this internally.

---

### Methods

#### `Badge.display_text(line1, line2)` → `None`

```python
b.display_text("Hello", "World")
```

Clears the display and renders two lines of text at fixed positions:
- `line1` at y=0 (top)
- `line2` at y=16

Lines are truncated at 16 characters (128px / 8px per char).

**Parameters**:
- `line1: str` — first line (truncated to 16 chars)
- `line2: str` — second line (truncated to 16 chars)

---

#### `Badge.ping(target_uid)` → `None`

```python
b.ping("a1b2c3d4e5f6")
```

Transmits `target_uid` as 6 NEC IR frames (address 0x42, one UID byte per frame, 50 ms inter-frame gap). Blocks until all 6 frames are transmitted (~1 second).

**In this spec**: `ping()` is IR-only (not HTTP). It transmits the target UID over IR so a nearby badge can receive it. The caller's own UID is NOT transmitted — the `target_uid` argument is what gets sent. This enables directed "tap" style interactions.

**Parameters**:
- `target_uid: str` — 12-char hex badge ID to transmit; must be exactly 12 chars

**Raises**: `ValueError` if `target_uid` is not 12 hex characters.

**Side effects**: IR TX LED active for ~1 second. IR RX temporarily suspended during TX burst (re-enabled after).

---

#### `Badge.pings()` → `list[str]`

```python
received = b.pings()
# e.g. ["a1b2c3d4e5f6", "b2c3d4e5f6a1"]
```

Returns and clears the list of badge UIDs received via IR since the last call. Each entry is a 12-char hex string assembled from 6 consecutive NEC frames with address 0x42.

**Thread safety**: The internal UID buffer is protected by a mutex. Calling `pings()` is safe from the main loop while the IR RX thread is running.

**Returns**: `list[str]` — may be empty. Each string is exactly 12 lowercase hex characters.

---

## Usage Example

```python
from badge_sdk import Badge
import utime

b = Badge()

while True:
    # Check for received badge UIDs
    received = b.pings()
    for uid in received:
        b.display_text(f"Got ping!", uid[:12])
        utime.sleep_ms(2000)

    # Show own UID
    b.display_text("My ID:", b.uid[:12])
    utime.sleep_ms(500)
```

---

## Invariants

1. `Badge()` constructor MUST NOT raise on a fresh badge (no creds.py, no WiFi).
2. `Badge.uid` MUST always return a 12-char string, never `None`.
3. `Badge.pings()` MUST clear the internal buffer on each call (not accumulate indefinitely).
4. `Badge.ping()` MUST NOT affect the IR RX buffer (received UIDs are not cleared by TX).
5. The display is 128×64 pixels; any rendering outside this bounds is silently clipped by the framebuf layer.

---

## Out of Scope (Future Spec)

The following methods are defined in the constitution's Badge App Platform section but are **NOT part of this spec**:

```python
# Future — badge_crypto + HMAC required:
b.connect()                              # WiFi + NTP
b.boops()                               # GET /api/v1/boops
b.boop(target_badge_uuid)               # POST /api/v1/boops
b.boop_partner(pairing_id)             # GET /api/v1/boops/<id>/partner
b.ping(target_ticket_uuid, activity_type, data={})   # POST /api/v1/pings (HTTP)
b.pings(activity_type, source=None, target=None)     # GET /api/v1/pings (HTTP)
```

When those are implemented, the `ping(target_uid)` IR-only method in this spec will be renamed to `ir_ping(target_uid)` or a separate `ir_send(uid)` to avoid collision. This renaming is a known future breaking change and is acceptable given the conference deployment schedule.
