# Quickstart: Manual Verification Guide

**Branch**: `003-firmware-ui-api` | **Date**: 2026-03-10

Hardware required: Temporal Badge DELTA, USB-C cable, macOS dev machine with `esptool`, `mpremote`, ESP-IDF v5.2.3.

---

## Prerequisites

1. Firmware built and flashed (with fixed `_boot.py`):
   ```bash
   cd firmware/micropython-build
   ./build.sh            # produces firmware-conference.bin
   ./flash.sh            # full erase + flash + VFS copy
   ```
   > Full flash: hold B, press+release RST, release B → esptool runs → press RST when prompted → mpremote copies VFS files.

2. `creds.py` placed on VFS with real WiFi + server URL:
   ```bash
   cat > creds.py <<'EOF'
   SSID = "YourNetwork"
   PASS = "YourPassword"
   SERVER_URL = "http://192.168.1.10:5000"
   EOF
   mpremote connect /dev/cu.usbmodem* fs cp creds.py :creds.py
   ```

3. REPL access: `mpremote connect /dev/cu.usbmodem*`

---

## US1 — Reliable VFS Update Tool

**Goal**: Verify `./flash.sh --vfs-only` works reliably and shows per-file confirmation.

### Steps

1. Badge is powered on and running normally (boot.py executing).
2. Run:
   ```bash
   ./flash.sh --vfs-only
   ```
3. **Expected**: Tool interrupts the badge mid-boot (within 10 s). Each file name appears **after** it is copied:
   ```
   [1/1] Copying VFS files...
     -> config.py  OK
     -> boot.py  OK
     -> main.py  OK
     ...
   === VFS copy complete ===
   ```
4. If any file fails:
   ```
     ERROR: boot.py copy failed (exit code 1)
   ```
5. Badge restarts cleanly after copy.

### Verification via REPL

After copy, connect to REPL and confirm:
```python
import os
os.listdir('/')
# Expected: ['config.py', 'boot.py', 'main.py', 'ir_nec.py',
#            'badge_sdk.py', 'graphics.py', 'uQR.py', 'badge_api.py', 'creds.py']
```

**Acceptance**: SC-001 — complete in < 60 s, zero ambiguity about copy status.

---

## US2 — Hardware Input Verification

**Goal**: All inputs produce correct OLED feedback. Requires badge running `main.py` (main loop).

**Note**: `config.BYPASS = True` is fine for this test — no WiFi needed.

### Button test

Hold each button individually. While held, a 3×3 dot appears in the d-pad area:
| Button | Dot position (x, y) |
|--------|-------------------|
| BTN_UP (GPIO 8) | 118, 48 |
| BTN_DOWN (GPIO 9) | 118, 58 |
| BTN_LEFT (GPIO 10) | 113, 53 |
| BTN_RIGHT (GPIO 11) | 123, 53 |

Dot disappears immediately on release.

### Joystick test

Move joystick in each direction. A 3×3 dot tracks inside the 6-pixel-radius circle centered at (100, 56). Dot does not escape circle boundary.

### Tilt test

Tilt badge. After holding tilt 300 ms, the 4×4 indicator block moves:
- Tilted (active low, GPIO 7 = 0): block at (84, 48)
- Level (GPIO 7 = 1): block at (84, 54)

### IR test

**TX**: Press BTN_UP. Filled TX arrow (up) appears in header area for ~1 s.
**RX**: Have another badge (or IR remote) send NEC signal address 0x42. Filled RX arrow (down) appears for ~1 s.

**Acceptance**: SC-002 — 5 min interactive session with zero unhandled exceptions.

---

## US3 — WiFi Connection at Startup

**Goal**: Badge with valid `creds.py` connects, syncs clock, shows connected state.

### Steps

1. Ensure `creds.py` is on VFS with valid credentials.
2. Press RST to reboot badge.
3. Watch OLED during boot sequence.

**Expected sequence**:
```
UID: <12-char hex>          ← 2 s
Connecting to <SSID>...     ← up to 5 s
Connected to <SSID>         ← 1.5 s, then fades
```

4. Verify clock synced via REPL:
   ```python
   import utime; utime.localtime()
   # Should return current UTC time (year 2026), not 2000-01-01
   ```

**Failure path**: Provide wrong credentials in `creds.py`:
- OLED shows "WiFi failed"
- Badge stops; does **not** show nametag or enter main loop

**Acceptance**: SC-003, SC-004.

---

## US4 — Badge API Client (REPL test)

**Goal**: All `badge_api` functions return correct types and fail gracefully.

Connect to REPL after badge is WiFi-connected:

```python
import badge_api

# Test 1: Registered badge info
info = badge_api.get_badge_info("aabbccddeeff")
print(info)
# {"name": "Jane Smith", "title": "Staff Engineer", "company": "Acme Corp", ...}

# Test 2: Unregistered badge (404)
info_none = badge_api.get_badge_info("000000000000")
print(info_none)
# None

# Test 3: Network error (disconnect WiFi first, or use bad SERVER_URL in creds.py)
# info_err = badge_api.get_badge_info("aabbccddeeff")
# print(info_err)  # None — no unhandled exception

# Test 4: Boop (requires two real badge UUIDs)
result = badge_api.create_boop("aabbccddeeff", "112233445566")
print(result)
# {"workflow_id": "...", "status": "pending", ...}

# Test 5: Get boop status
status = badge_api.get_boop_status(result["workflow_id"], "aabbccddeeff")
print(status)
# {"status": "pending"} or {"status": "confirmed"}
```

**Acceptance**: SC-005 — no unhandled exceptions across success, timeout, and 404 scenarios.

---

## US5 — Live Attendee Nametag at Startup

**Goal**: Registered badge shows real attendee data on OLED at boot.

### Registered badge

1. Badge has `creds.py` pointing to server where the badge UUID is registered.
2. Press RST.
3. After WiFi connects, OLED shows nametag:
   ```
   [line 1] Jane Smith
   [line 2] Staff Engineer
   [line 3] Acme Corp
   ```
   Displayed for ~3 s, then main UI loads.

4. Verify truncation: if any field > 16 chars, it is cut at 16 with no display overflow.

### Unregistered badge (404)

1. Badge UUID not in server DB.
2. After WiFi connects, OLED shows:
   ```
   Not registered
   ```
   For 2 s, then main UI loads normally.

### Network error during fetch

1. WiFi connected but server unreachable (wrong SERVER_URL).
2. OLED shows error message briefly, then main UI loads — badge does **not** crash or hang.

**Acceptance**: SC-003 (within 15 s total), SC-006 (unregistered badge reaches main UI within 5 s of message).

---

## Post-Verification Checklist

- [ ] US1: `./flash.sh --vfs-only` completes < 60 s with per-file OK/ERROR
- [ ] US1: `os.listdir('/')` confirms all files present after VFS-only update
- [ ] US1: Badge restarts cleanly (no mount errors in boot log)
- [ ] US2: All 4 buttons show dot at correct position when held
- [ ] US2: Joystick dot tracks within circle, does not escape boundary
- [ ] US2: Tilt debounce 300 ms observed before indicator moves
- [ ] US2: IR TX/RX arrows appear and auto-clear after 1 s
- [ ] US3: "Connected to \<SSID\>" shown; `utime.localtime()` returns 2026 year
- [ ] US3: Wrong credentials → "WiFi failed" + halt (no nametag, no main loop)
- [ ] US4: All 5 REPL function calls produce correct types or None
- [ ] US4: No unhandled exceptions on 404 or network error
- [ ] US5: Registered badge → name/title/company on OLED lines 1/2/3
- [ ] US5: Unregistered badge → "Not registered" 2 s → main UI
- [ ] US5: 16-char truncation verified for long field
