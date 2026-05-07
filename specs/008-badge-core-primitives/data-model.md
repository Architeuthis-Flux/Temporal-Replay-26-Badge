# Data Model: Badge Core Primitives Validation & Integration

**Feature**: `008-badge-core-primitives`
**Date**: 2026-03-18

---

## Entities

### Badge (NVS-persisted)

Owned by `BadgeStorage`. Loaded during `setup()`. Written on pairing.

| Field | Type | NVS Key | Notes |
|-------|------|---------|-------|
| uuid | string (12 hex chars) | `uid` (read from eFuse) | Hardware-derived; read-only |
| paired | bool | `paired` (via `BadgeStorage::loadState`) | true = BADGE_PAIRED |
| role | int (ROLE_NUM_*) | stored alongside paired state | 1=Attendee, 2=Staff, 3=Vendor, 4=Speaker |
| qr_bitmap | blob (1024 bytes) | `qr_data` | XBM bytes; cached from server |
| badge_bitmap | blob (1024 bytes) | (heap only; loaded from server on boot) | Attendee nametag graphic |

**State machine**:
```
BADGE_UNPAIRED → (HTTP 200 from polling) → BADGE_PAIRED
```
No DEMO state in spec-008 scope; DEMO is a legacy reference in Firmware-0308.

**Validation rules**:
- QR bitmap: exactly 1024 bytes expected (128×64 XBM); firmware validates `byteCount` before write
- Badge bitmap: same size constraint
- UUID: 12 hex chars from eFuse OPTIONAL_UNIQUE_ID; immutable after manufacture
- Role: if server returns unknown attendee_type, defaults to ROLE_NUM_ATTENDEE (1)

---

### Enrollment (backend-side; detected by polling)

Not persisted in firmware NVS directly. Firmware detects enrollment by polling the badge
info endpoint and receiving HTTP 200. On detection, badge transitions to PAIRED and stores
the returned bitmap + role.

**Enrollment trigger** (backend, out of scope for firmware):
- `POST /api/v1/link-user-to-badge` (existing) or `POST /api/v1/badges/enroll` (planned)
- After enrollment: `GET /api/v1/badge/{uid}/info` returns HTTP 200 with `{name, title,
  company, attendee_type, bitmap: int[]}`

---

### Boop (IR pairing event)

Managed by `BadgeIR` (Core 0 FreeRTOS task) + `BadgeAPI::createBoop()`.

| Field | Source | Notes |
|-------|--------|-------|
| my_uuid | eFuse | Sent in boop POST body |
| peer_uuid | IR NEC frame (6×DATA packets) | Received from peer badge via TCP-IR exchange |
| peer_name | Backend response | `partner_name` from POST /boops |
| workflow_id | Backend response | Used for polling boop status (202 case) |
| pairing_id | Backend response | Returned on immediate confirmation (200 case) |

**IrStatus struct** (volatile; read by Core 1 for display, written by Core 0 irTask):

| Field | Type | Notes |
|-------|------|-------|
| phase | IrPhase | Current state machine phase |
| statusMsg | char[48] | Human-readable status for display (e.g. "TX 0xC0:0xNN retry 2") |
| peerUidHex | char[13] | Peer UID being received (fills in byte-by-byte during RX_UID) |
| role | IrRole | IR_INITIATOR or IR_RESPONDER; set at IR_ESTABLISHED |
| my_cookie | uint8_t | Random 1–255; sent in SYN/SYN_ACK for role determination |
| peer_cookie | uint8_t | Peer's cookie received in SYN_ACK; compared to my_cookie |

**IrPhase state machine** (TCP-IR protocol — spec-008 Session 2026-03-19):

```
IR_IDLE          → passive SYN listener active; IrReceiver enabled
IR_SYN_SENT      → UP pressed (edge trigger); sending SYN frames with backoff
IR_SYN_RECEIVED  → SYN received; sent SYN_ACK; waiting for ACK
IR_ESTABLISHED   → three-way handshake complete; INITIATOR/RESPONDER role assigned
IR_TX_UID        → sending our 6 UID bytes (DATA 0xD0–0xD5), each ACK'd by peer
IR_RX_UID        → receiving peer's 6 UID bytes; partial UID fills in on display
IR_PAIR_CONSENT  → both UIDs exchanged; HTTP POST to /api/v1/boops in progress
IR_PAIRED_OK     → POST succeeded; result screen
IR_PAIR_FAILED   → timeout / retries exhausted / RST received; result screen
IR_PAIR_CANCELLED → BTN_RIGHT pressed before IR_ESTABLISHED; result screen
```

**State transitions**:
```
IR_IDLE → IR_SYN_SENT        (BTN_UP edge trigger, irHardwareEnabled)
IR_IDLE → IR_SYN_RECEIVED    (SYN frame received from peer)
IR_SYN_SENT → IR_ESTABLISHED (SYN_ACK received, ACK sent; my_cookie < peer_cookie → INITIATOR)
IR_SYN_SENT → IR_ESTABLISHED (SYN_ACK + peer SYN received; role determined by cookie comparison)
IR_SYN_SENT → IR_PAIR_CANCELLED (BTN_RIGHT)
IR_SYN_SENT → IR_PAIR_FAILED (SYN retries exhausted)
IR_SYN_RECEIVED → IR_ESTABLISHED (ACK received)
IR_SYN_RECEIVED → IR_PAIR_FAILED (SYN_ACK retries exhausted)
IR_ESTABLISHED → IR_TX_UID   (role == INITIATOR) or IR_RX_UID (role == RESPONDER)
IR_TX_UID → IR_RX_UID        (all 6 bytes ACK'd; INITIATOR transitions to receive)
IR_RX_UID → IR_PAIR_CONSENT  (all 6 peer bytes received and ACK'd)
IR_TX_UID → IR_PAIR_FAILED   (ACK retries exhausted for any byte)
IR_RX_UID → IR_PAIR_FAILED   (DATA timeout or RST received)
IR_PAIR_CONSENT → IR_PAIRED_OK / IR_PAIR_FAILED (HTTP POST result)
```

**BoopResult struct** (unchanged):
- ok, httpCode, status (BOOP_STATUS_*), workflowId[96], pairingId, partnerName[64],
  partnerTitle[64], partnerCompany[64], partnerAttendeeType[16]

---

### Tilt State

Ephemeral; not persisted. Read from hardware on each `pollTilt()` call.

| Field | Type | Source | Notes |
|-------|------|--------|-------|
| tiltState | bool | `digitalRead(TILT_PIN)` | HIGH = upright (nametag orientation) |
| tiltNametagActive | bool | Computed after TILT_HOLD_MS debounce | Set true after 1500ms sustained HIGH |
| tiltHoldPending | bool | Transient | Tracks ongoing hold timing |

**Activation conditions** (spec-008 — new):
- `tiltState == HIGH` for `TILT_HOLD_MS` (1500ms) continuously
- AND `renderMode == MODE_MENU` (new guard — Principle II documented deviation)
- AND `badgeState == BADGE_PAIRED` (implied: `badgeBits != nullptr`)

**Deactivation**:
- `tiltState == LOW` (any orientation change)

---

### PingResult (ephemeral; C struct + Python return value)

Not persisted. Returned from `BadgeAPI::probeBadgeExistence()` and wrapped for Python.

| Field | Type | Notes |
|-------|------|-------|
| ok | bool | true = HTTP 200 received from badge info endpoint |
| httpCode | int | Raw HTTP code; 0 on connection failure |

**Python exposure**: `badge.ping()` returns `True` (ok) or `False` (any failure), discarding
the httpCode. If WiFi is not connected, the HTTP call will fail with httpCode=0 → returns `False`.

---

### Python App

Stored in VFS at `/apps/<name>.py`. Managed by `BadgePython`.

| Field | Constraint | Notes |
|-------|-----------|-------|
| source file | ≤16KB | MP_SCRIPT_MAX_BYTES limit |
| filename | `[a-z0-9_]+.py` | Menu item name = filename without .py |
| heap budget | 128KB shared | MICROPY_HEAP_SIZE |
| runtime state | ephemeral | Soft-reset between app launches; no persistence |

**Exception handling**: Unhandled Python exceptions are caught at firmware boundary in
`BadgePython::execApp()`. Traceback is displayed on OLED; control returns to main badge screen.
No hardware reset required.

---

## State Transitions Summary

```
Power on
  → BADGE_UNPAIRED: show QR, poll backend, await enrollment
  → BADGE_PAIRED:   show menu, tilt-to-nametag available

On menu (BADGE_PAIRED, renderMode == MODE_MENU):
  → BTN_DOWN on "Boop":        IR pairing flow
  → BTN_DOWN on "QR / Pair":   rePair() → fetchBadgeXBM refresh
  → BTN_DOWN on "Input Test":  renderMode = MODE_INPUT_TEST
  → BTN_DOWN on "Apps":        app sub-menu → execApp()
  → Tilt held 1500ms:          renderMode → (tilt nametag display, spec-008 gated)

While Python app runs:
  → loop() is blocked in execApp(); no tilt, no button polling, no IR
  → Escape chord (BTN_UP + BTN_DOWN): raises KeyboardInterrupt → app exit
  → Unhandled exception: caught, displayed, return to menu
```
