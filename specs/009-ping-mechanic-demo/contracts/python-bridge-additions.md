# Contract: Python Bridge Additions (spec-009)

## Overview

The `badge` MicroPython C extension gains three new functions to support the Conquest app. Implemented in `BadgePython_bridges.cpp` following the existing bridge pattern.

---

## New Functions (`import badge`)

### `badge.boops()` → str (JSON array)

Calls `BadgeAPI_getBoops(my_badge_uuid)` and returns active (non-revoked) pairings as a JSON array string. Each entry contains the partner's ticket UUID (resolved from `BoopRecord.ticket_uuids` using own ticket UUID from NVS) plus partner name and company (from `GET /api/v1/boops/{id}/partner`).

- Empty array `[]` if no active pairings.
- Returns `[]` on HTTP error (show "No contacts" to user).
- Caller must parse with `ujson.loads()`.

**Return format:**
```json
[
  {"pairing_id": 42, "ticket": "...", "name": "Alice", "company": "Acme"},
  {"pairing_id": 17, "ticket": "...", "name": "Bob",   "company": "Beta"}
]
```

**Note**: This function makes N+1 HTTP calls (1 for boops list + 1 per pairing for partner name). Acceptable for conference-scale contact counts (<50).

**Example:**
```python
import ujson
boops = ujson.loads(badge.boops())
army_size = len(boops)
for b in boops:
    print(b["name"], b["company"])
    # target_ticket_uuid = b["ticket"]
```

---

### `badge.my_uuid()` → str

Returns the badge's hardware UUID. Used as `source_badge_uuid` in `POST /api/v1/pings` and `requester_badge_uuid` in `GET /api/v1/pings`.

- 12-character hex string (hardware UID from eFuse).
- Same value as `badge.uid()` — explicit alias for clarity in app code.

**Example:**
```python
my_id = badge.my_uuid()   # "a1b2c3d4e5f6"
```

---

### `badge.my_ticket_uuid()` → str

Returns the badge's own ticket UUID from NVS (`badge_identity.ticket_uuid`). Required as the `target=` query parameter when polling `GET /api/v1/pings` for incoming pings.

- 36-character UUID string (e.g. `"xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"`).
- Returns empty string `""` if ticket UUID not yet stored (badge not enrolled or pre-spec-009 firmware; caller should show error or skip poll).

**Example:**
```python
my_ticket = badge.my_ticket_uuid()
# Used as: "?target=" + my_ticket
```

---

## Unchanged Functions Used by spec-009

These existing functions are used by the Conquest app without modification:

| Function | Usage in Conquest App |
|----------|----------------------|
| `badge.http_post(url, body)` → str | Send challenge/response pings |
| `badge.http_get(url)` → str | Poll for incoming pings |
| `badge.button_pressed(pin)` → int | BTN_RIGHT exit, UP/DOWN navigation |
| `badge.server_url()` → str | Base URL for API calls |
| `badge.gc_collect()` | Called in polling loop to prevent OOM |

---

## Ping URL Patterns (Python layer)

```python
SERVER       = badge.server_url()
MY_UUID      = badge.my_uuid()         # hardware UUID
MY_TICKET    = badge.my_ticket_uuid()  # ticket UUID — required as source/target filter

# Send ping
POST_URL = SERVER + "/api/v1/pings"

# Poll for incoming conquest challenges (directed AT me)
# target= is required — without source or target the endpoint returns 422
POLL_CHALLENGES = (SERVER + "/api/v1/pings"
    + "?requester_badge_uuid=" + MY_UUID
    + "&target=" + MY_TICKET
    + "&type=conquest_challenge&limit=5")

# Poll for conquest responses (from me, directed at opponent)
# source= scopes to pings I sent — responses come back as target=MY_TICKET on opponent's side
# but from MY perspective, I look for conquest_response directed AT me:
POLL_RESPONSES = (SERVER + "/api/v1/pings"
    + "?requester_badge_uuid=" + MY_UUID
    + "&target=" + MY_TICKET
    + "&type=conquest_response&limit=5")
```

**Note**: Python apps call `badge.http_post(url, body)` directly — HMAC headers are NOT attached for Python-layer calls in v1. The Conquest app will be tested against a backend that either has HMAC disabled for the demo activity types, or the HMAC middleware is implemented server-side to handle both authenticated and test modes.

**Amendment required if HMAC is enforced for Python callers**: A `badge.http_post_auth(url, body)` bridge function would need to be added that internally calls the HMAC helper before the request. This is deferred — the C-layer (Messages) handles HMAC via `BadgeAPI.cpp`; the Python layer needs separate bridge work.
