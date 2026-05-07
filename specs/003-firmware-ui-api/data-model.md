# Data Model: Firmware UI & API Integration

**Branch**: `003-firmware-ui-api` | **Date**: 2026-03-10

This spec introduces no database schema or persistent on-device storage beyond what is already in NVS. All entities below are transient Python dicts passed between badge_api.py and its callers.

---

## Entities

### AttendeeInfo

Returned by `badge_api.get_badge_info(badge_uuid)`.

```python
{
    "name":          str,   # Display name. May be empty string if null in response.
    "title":         str,   # Job title. May be empty string.
    "company":       str,   # Company name. May be empty string.
    "attendee_type": str,   # e.g. "speaker", "attendee", "sponsor". May be absent.
}
# Returns None on HTTP 404 or network error.
```

**Validation rules**:
- Each field truncated to 16 chars before display (enforced in boot.py, not badge_api.py).
- badge_api.py returns raw server values; caller is responsible for truncation.

**State transitions**: None — read-only, fetched once at boot.

---

### BoopResult

Returned by `badge_api.create_boop(uuid1, uuid2)`.

```python
{
    "workflow_id":  str,    # Temporal workflow ID — used to poll status.
    "status":       str,    # "pending" | "confirmed"
    # Additional fields from server may be present; callers should use .get()
}
# Returns None on network error or non-2xx response.
```

**Status semantics**:
- `pending`: first badge to POST; workflow is waiting for the second badge.
- `confirmed`: second badge to POST; both sides have consented.

**Request shape** (POST /api/v1/boops):
```python
{"badge_uuids": [uuid1, uuid2]}
```

---

### BoopStatus

Returned by `badge_api.get_boop_status(workflow_id, badge_uuid)`.

```python
{
    "status": str,  # "pending" | "confirmed" | "cancelled"
    # Additional partner nametag fields may be present
}
# Returns None on network error or not-found.
```

---

### BoopList

Returned by `badge_api.list_boops(badge_uuid)`.

```python
[
    {
        "workflow_id": str,
        "status":      str,
        "partner":     dict,   # optional partner nametag fields
    },
    ...
]
# Returns None on network error. Returns [] on empty list.
```

---

### CancelResult

Returned by `badge_api.cancel_pending_boop(badge_uuid)`.

```python
True   # on success (HTTP 200 or 204)
None   # on network error or non-2xx
```

---

## Configuration Entities (on-device)

### creds module (VFS: creds.py)

```python
SSID       = "..."   # WiFi network name
PASS       = "..."   # WiFi password
SERVER_URL = "..."   # e.g. "http://192.168.1.10:5000" — no trailing slash
```

Imported by both `boot.py` and `badge_api.py`. badge_api.py reads `SERVER_URL` lazily on first request.

---

## Entity Relationships

```
Badge (hardware) — has one —> AttendeeInfo  (fetched at boot via badge_api)
Badge            — creates —> BoopResult    (via badge_api.create_boop)
BoopResult       — polled  —> BoopStatus    (via badge_api.get_boop_status)
Badge            — lists   —> BoopList      (via badge_api.list_boops)
```

---

## What Is NOT in Scope

- QR code data: derived entirely on-device from `badge_uid` (machine.unique_id hex). Not fetched from server.
- NVS credentials (uuid, hmac_secret, enrolled): managed by `badge_crypto` C module (future spec).
- Ping stream entities: deferred to future spec covering the Badge App Platform.
