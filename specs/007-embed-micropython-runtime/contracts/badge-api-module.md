> **SUPERSEDED**: This document describes a `badge_api` module that was planned before the
> spec's 2026-03-14 clarification session. Per the final spec (FR-005), HTTP access is part
> of the `badge` module directly (`badge.http_get`, `badge.http_post`), not a separate module.
> This document is retained for historical context only. See `badge-module-api.md` for the
> authoritative v1 contract.

# Contract: `badge_api` Python Module (SUPERSEDED)

**Module**: `badge_api` (C extension, registered via `MP_REGISTER_MODULE`)
**Version**: 1.0
**Backed by**: `BadgeAPI.h` / `BadgeAPI.cpp` (spec 006 contract library) via a C-compatible wrapper layer (`badge_api_wrapper.h` / `badge_api_wrapper.cpp`)
**Stability**: Stable per Principle VII — breaking changes require versioned migration

---

## Import

```python
import badge_api
```

No sub-modules. All functions are top-level on the `badge_api` object.

---

## Badge Identity

### `badge_api.my_uuid() → str`

Returns this badge's own UUID string, read from NVS via `BadgeStorage`. Used to identify
the caller in boop and ping operations.

```python
my_id = badge_api.my_uuid()
```

---

## Badge Info

### `badge_api.get_badge_info(uid) → dict`

Fetches public profile info for any registered badge.
Calls `BadgeAPI::getBadgeInfo(uid)`.

**Returns**:
```python
# success
{"ok": True, "name": "Alice", "title": "Engineer",
 "company": "Acme", "attendee_type": "attendee"}

# failure
{"ok": False, "http_code": 404}
```

---

## Boop (Pairing)

### `badge_api.create_boop(their_uid) → dict`

Initiates or confirms a boop pairing with another badge.
Calls `BadgeAPI::createBoop(my_uuid, their_uid)` — badge UUID is auto-injected from NVS.

**Returns (pending)**:
```python
{"ok": True, "status": "pending", "workflow_id": "badge-pairing-..."}
```

**Returns (confirmed)**:
```python
{"ok": True, "status": "confirmed", "pairing_id": 42,
 "partner_name": "Bob", "partner_title": "Designer",
 "partner_company": "Acme", "partner_attendee_type": "attendee"}
```

**Returns (failure)**:
```python
{"ok": False, "http_code": 404}
```

---

### `badge_api.get_boop_status(workflow_id) → dict`

Polls the status of a pending boop.
Calls `BadgeAPI::getBoopStatus(workflow_id, my_uuid)` — badge UUID auto-injected.

**Returns**:
```python
# still pending
{"ok": True, "status": "pending", "workflow_id": "badge-pairing-..."}

# confirmed
{"ok": True, "status": "confirmed", "pairing_id": 42,
 "partner_name": "Bob", "partner_title": "...", ...}

# not found / expired
{"ok": True, "status": "not_requested"}
```

---

### `badge_api.cancel_boop(their_uid) → dict`

Cancels a pending boop request.
Calls `BadgeAPI::cancelBoop(my_uuid, their_uid)` — badge UUID auto-injected.

**Returns**:
```python
{"ok": True}     # cancelled
{"ok": False, "http_code": 404}  # not found
```

---

### `badge_api.get_boop_partner(pairing_id) → dict`

Fetches partner profile for a confirmed pairing.
Calls `BadgeAPI::getBoopPartner(pairing_id, my_uuid)` — badge UUID auto-injected.

**Returns**:
```python
{"ok": True, "partner_name": "Bob", "partner_title": "...",
 "partner_company": "...", "partner_attendee_type": "..."}

{"ok": False, "http_code": 403}  # not a participant
```

---

## Pings

### `badge_api.send_ping(target_ticket_uuid, activity_type, data) → dict`

Sends a ping to a partner ticket. Requires an active pairing between this badge and the target.
Calls `BadgeAPI::sendPing(my_uuid, target_ticket_uuid, activity_type, data_json)` — badge UUID
auto-injected. `data` must be a JSON string (use `ujson.dumps({"key": "val"})` if starting from a dict).

```python
import badge_api, ujson

result = badge_api.send_ping(
    "target-ticket-uuid",
    "high_five",
    ujson.dumps({"note": "great talk!"})
)
```

**Returns**:
```python
{"ok": True, "id": "uuid", "activity_type": "high_five", "created_at": "2026-..."}

{"ok": False, "http_code": 403}  # no active pairing
```

---

### `badge_api.get_pings_sent(activity_type=None, limit=20) → list`

Returns pings sent by this badge. Badge UUID auto-injected.
Calls `BadgeAPI::getPings(my_uuid, source=my_uuid, target=NULL, ...)`.

```python
pings = badge_api.get_pings_sent(limit=10)
for p in pings:
    print(p["activity_type"], p["created_at"])
```

**Each record**:
```python
{"id": "uuid", "activity_type": "high_five", "data": "{...}", "created_at": "2026-..."}
```

Returns empty list on error or no results.

---

### `badge_api.get_pings_received(activity_type=None, limit=20) → list`

Returns pings received by this badge. Badge UUID auto-injected.
Calls `BadgeAPI::getPings(my_uuid, source=NULL, target=my_uuid, ...)`.

Same return shape as `get_pings_sent`.

---

## Error Behavior

All functions are **blocking** — they make synchronous HTTP calls on Core 1 and return
when the response (or timeout) is received.

| Situation | Outcome |
|-----------|---------|
| WiFi not connected | `{"ok": False, "http_code": -1}` |
| HTTP timeout | `{"ok": False, "http_code": -1}` |
| HTTP error status | `{"ok": False, "http_code": N}` |
| Badge UUID not in NVS | `badge_api.my_uuid()` returns empty string; boop/ping functions return `{"ok": False, "http_code": -1}` |

---

## Implementation Notes

- `badge_api_module.c` calls into `badge_api_wrapper.h` — a C-compatible wrapper layer
  that exposes C structs and `extern "C"` function declarations over the `BadgeAPI` C++ namespace.
- `badge_api_wrapper.cpp` implements the wrappers, calling `BadgeAPI::` functions directly.
- Badge UUID is fetched once per call via a C wrapper around `BadgeStorage` — no caching.
- Auth headers are stubbed in `BadgeAPI.cpp` (spec 006 stub) — Python apps get auth for
  free when the stub is implemented in a future feature, with no changes to `badge_api_module.c`.
- Pagination cursors (`before_ts`, `before_id`) are not exposed in the Python API v1.
  `get_pings_sent` and `get_pings_received` return the first page only. Cursor support
  is a v2 addition.
- `ujson` module is enabled in `mpconfigport.h` (`MICROPY_PY_UJSON=1`) so apps can
  serialize/deserialize JSON data for `send_ping` and inspect ping `data` fields.

---

## Stability Policy (Principle VII)

Same policy as `badge` module — breaking changes require a version bump and migration note.
Additive changes (new functions, new optional parameters) are non-breaking.
