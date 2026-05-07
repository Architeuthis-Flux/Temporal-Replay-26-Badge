# Research: Firmware UI & API Integration

**Branch**: `003-firmware-ui-api` | **Date**: 2026-03-10

---

## R-001: mpremote Ctrl+C interrupt behavior vs. MicroPython sleep

**Decision**: Break all `utime.sleep_ms(N)` calls in `boot.py` that are > ~100 ms into loops of `utime.sleep_ms(50)` within a `for` range.

**Rationale**: `utime.sleep_ms(N)` is a single C-level call that holds the GIL for its entire duration. MicroPython can only service a Ctrl+C (KeyboardInterrupt) between bytecode instructions — not mid-C-call. When mpremote connects it sends one or two `\x03` (Ctrl+C) bytes, but if the badge is inside `sleep_ms(2000)` at the moment of connection, the interrupt is not received until the sleep returns. Breaking into `for _ in range(N // 50): utime.sleep_ms(50)` makes Ctrl+C deliverable on every 50 ms boundary, guaranteeing interrupt within 50–100 ms of mpremote connecting.

**Affected sleeps in boot.py**:
- `utime.sleep_ms(2000)` after splash → `for _ in range(20): utime.sleep_ms(100)`
- `utime.sleep_ms(1500)` after WiFi connect → `for _ in range(15): utime.sleep_ms(100)`
- `utime.sleep_ms(config.QR_DISPLAY_MS)` (10000 ms) → `for _ in range(100): utime.sleep_ms(100)`
- `utime.sleep_ms(2000)` after nametag → `for _ in range(20): utime.sleep_ms(100)`
- `utime.sleep_ms(config.BYPASS_DELAY_MS)` (3000 ms) → `for _ in range(30): utime.sleep_ms(100)`

**Alternatives considered**:
- Hardware button to skip boot (BTN_DOWN held → immediate REPL): more robust but requires GPIO polling in boot.py, more complex, and doesn't fix the shell script confirmation issue.
- `machine.soft_reset()` before copy session: causes mpremote reconnect timing problem; two-session approach reintroduces the "can't enter raw repl" race.
- No change to boot.py; only change flash.sh to wait longer: unreliable, sleep_ms is never interruptible within its window.

---

## R-002: flash.sh --vfs-only exec VFS mount necessity

**Decision**: Remove the `exec "import os,esp32; bdev=..."` mount command from the `--vfs-only` session. Rely exclusively on `_boot.py` (frozen) to mount the VFS before `boot.py` runs.

**Rationale**: The `_boot.py` frozen module always attempts `os.mount(bdev, '/')` on every boot, treating EBUSY (already mounted) as success and ENODEV (unformatted) as a format trigger. When the badge is running normally, VFS is already mounted. When mpremote connects and interrupts the badge, `_boot.py` has already executed — the VFS is mounted. The manual `exec` mount was added to handle cases where _boot.py wasn't doing this correctly; that bug is now fixed in source. The exec also requires the badge to be in a state where it can evaluate an import statement before boot.py runs, which is a race condition. Dropping it simplifies the flow to: `Ctrl+C → raw REPL → fs cp` directly.

**Alternatives considered**:
- Keep exec mount for safety: adds race condition risk; if VFS already mounted, the exec may fail or double-mount; cleaner to trust the frozen module.
- Use `mpremote exec "import os; os.listdir('/')"` to verify mount first: unnecessary round-trip; adds complexity.

---

## R-003: flash.sh per-file confirmation strategy

**Decision**: Switch `--vfs-only` from a single chained mpremote session to individual `mpremote connect PORT fs cp SRC :DST` invocations per file. Print the file name AFTER checking the exit code of each invocation.

**Rationale**: The chained session `mpremote connect PORT + fs cp a + fs cp b + reset` cannot provide per-file confirmation because all output is interleaved in a single session exit. Individual invocations let the shell check `$?` (exit code) after each copy and print either `  -> filename  OK` or `  ERROR: filename copy failed`. Each mpremote fs cp session connects, interrupts boot.py (now interruptible thanks to R-001), copies one file, and disconnects. The reconnect overhead per file is acceptable for a developer tool (< 2 s per file on USB).

**Alternatives considered**:
- Parse mpremote session output for per-file success markers: mpremote does not emit structured per-file status; parsing stdout is fragile.
- Single session with `+` separators but echo after: Bash executes all `echo` commands during array construction, before mpremote runs. There is no way to interleave bash echoes with mid-session mpremote output without external process communication.
- Use Python script wrapping mpremote: over-engineered; bash with per-invocation exit codes is sufficient.

---

## R-004: urequests JSON parsing in MicroPython

**Decision**: Use `r.json()` method provided by `urequests` (frozen). This calls `ujson.loads(r.text)` internally. No additional import needed.

**Rationale**: The frozen `urequests` module exposes a `.json()` method on the response object that delegates to MicroPython's built-in `ujson`. This is the idiomatic approach, consistent with the CPython requests API. The `/api/v1/badge/{uuid}/info` endpoint returns `application/json` with keys `name`, `title`, `company`, `attendee_type`.

**Key finding — boot.py bug**: The current `boot.py` calls `_parse_xbm(r.text)` on the `/info` response, treating it as XBM binary data. This is incorrect — `/info` returns JSON. This bug silently produces garbage on the OLED. The fix is to use `badge_api.get_badge_info()` which correctly calls `.json()`.

**Alternatives considered**:
- `ujson.loads(r.text)` directly: equivalent but bypasses urequests abstraction; `r.json()` is cleaner.
- Manual JSON parsing with `re`: unnecessary complexity; ujson is available.

---

## R-005: badge_api.py transport layer design

**Decision**: Implement a single `_request(method, path, body=None)` internal function. All named endpoint functions call `_request`. Authentication headers (X-Badge-ID, X-Timestamp, X-Signature) will be added exclusively inside `_request` when the HMAC module is available, with zero changes to endpoint functions.

**Rationale**: FR-018 requires a single transport function for auth insertion. The design: `_request` reads `SERVER_URL` from the `creds` module (imported lazily on first call). It builds the URL, sets Content-Type for POST, calls `urequests.request(method, url, headers=headers, data=ujson.dumps(body))`, closes the response, and returns the parsed JSON or None. Named functions (`get_badge_info`, `create_boop`, etc.) are thin wrappers over `_request`.

**HMAC insertion point**: When HMAC signing is added in a future spec, only `_request` changes:
```python
def _request(method, path, body=None):
    headers = {"Content-Type": "application/json"}
    # FUTURE: add X-Badge-ID, X-Timestamp, X-Signature here
    ...
```
No endpoint function or caller (boot.py, main.py, badge_sdk.py) requires modification.

**Error handling contract**:
- HTTP 2xx: return parsed JSON dict (or True for DELETE/no-body responses)
- HTTP 404: return `None`
- HTTP other non-2xx: return `None` (log the status code)
- Network exception (OSError, etc.): return `None` (log the exception)
- Always: `r.close()` in a `finally` block

**Alternatives considered**:
- Class-based client (BadgeApiClient): unnecessary for 5 functions with no shared mutable state; module-level functions are simpler and work better in MicroPython's import model.
- Async urequests: not available in MicroPython standard urequests; would require uasyncio integration which conflicts with the IR _thread loop.

---

## R-006: Nametag display layout

**Decision**: Display nametag using three `display.text()` calls at fixed y-positions after `display.fill(0)`, with each field truncated to 16 characters. No call to `draw_base()` during nametag display (full-screen nametag, matching Firmware-0306 behavior).

**Rationale**: The constitution (Principle II) specifies a distinct "nametag" display mode separate from the main UI chrome. In Firmware-0306, the nametag is a full-screen display. Three text lines at y=10, y=26, y=42 give comfortable 16-pixel spacing. Truncation at 16 chars (matching `display.text()` 8×8 font × 16 chars = 128 px full width) prevents overflow. Null/missing fields render as empty string via `str(info.get('name') or '')[:16]`.

**Response field mapping**:
- `name` → line 1, y=10
- `title` → line 2, y=26
- `company` → line 3, y=42

**Alternatives considered**:
- Scrolling text for long fields: complex, not in spec, deferred.
- Using `draw_mock_nametag` as a template: that function uses a border frame and hardcoded text; replacing it with JSON data requires rewriting it anyway.
