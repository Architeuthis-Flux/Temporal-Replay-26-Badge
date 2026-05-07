# Data Model: Badge Ping Demo — Async Ping Mechanic Showcase

**Branch**: `009-ping-mechanic-demo` | **Date**: 2026-03-20

---

## Server-Authoritative Types (fetched on demand, not stored in NVS)

### BoopRecord (from `GET /api/v1/boops?badge_uuid=<id>`)

A confirmed pairing record returned by the backend. The badge uses this as its contact list.

| Field | Type | Notes |
|-------|------|-------|
| `id` | int | Pairing ID — used to fetch partner info |
| `ticket_uuids` | string[2] | Sorted pair of both participants' ticket UUIDs |
| `connected_at` | string[] | Timestamps of each boop event for this pair |
| `boop_count` | int | `len(connected_at)` — pre-computed server-side |
| `revoked_at` | string\|null | null = active; ISO timestamp = revoked (filter out) |
| `created_at` | string\|null | ISO 8601 |
| `updated_at` | string\|null | ISO 8601 |

**Partner ticket UUID resolution**: badge knows its own `ticket_uuid` from NVS. Partner = the element of `ticket_uuids` that ≠ own ticket UUID.

**Army size (Conquest)**: count of `BoopRecord`s where `revoked_at == null`.

Defined in `BadgeAPI_types.h` as `BoopRecord` C struct.

---

### PartnerInfo (from `GET /api/v1/boops/{id}/partner?badge_uuid=<my_uuid>`)

Display name and company for a specific booped contact.

| Field | Type | Notes |
|-------|------|-------|
| `pairing_id` | int | Echoes the pairing ID |
| `partner_name` | string\|null | Display name |
| `partner_company` | string\|null | Company |

Defined in `BadgeAPI_types.h` as `PartnerInfoResult` C struct (already exists as `PartnerInfoResponse` in the existing firmware spec — verify and reuse).

---

### PingRecord (from `GET /api/v1/pings` and `POST /api/v1/pings` response)

A single directed badge ping event.

| Field | Type | Notes |
|-------|------|-------|
| `id` | string (UUID) | Opaque — used as pagination cursor |
| `source_ticket_uuid` | string | Sender's ticket UUID |
| `target_ticket_uuid` | string | Recipient's ticket UUID |
| `activity_type` | string | Stream name |
| `data` | JSON object (raw string in C) | Payload; structure per activity_type |
| `created_at` | string | ISO 8601 — pagination cursor field |
| `updated_at` | string | ISO 8601 |

Defined in `BadgeAPI_types.h` as `PingRecord` C struct.

---

## Result Types (stack-allocated, transient)

### GetBoopsResult

| Field | Type | Notes |
|-------|------|-------|
| `ok` | bool | true on HTTP 200 |
| `httpCode` | int | Raw HTTP status |
| `count` | int | Number of valid entries in `boops[]` |
| `boops` | BoopRecord[50] | Active (non-revoked) pairings |

Max 50 entries; server returns all pairings, firmware filters `revoked_at != null`.

### SendPingResult

| Field | Type | Notes |
|-------|------|-------|
| `ok` | bool | true on HTTP 200 |
| `httpCode` | int | |
| `pingId` | char[37] | UUID of created ping |

### GetPingsResult

| Field | Type | Notes |
|-------|------|-------|
| `ok` | bool | |
| `httpCode` | int | |
| `count` | int | Populated entries in `pings[]` |
| `pings` | PingRecord[10] | Fetched records |
| `nextCursorTs` | char[32] | Empty when no next page |
| `nextCursorId` | char[37] | Empty when no next page |

---

## NVS Changes

### Additions to `badge_identity` namespace

| Key | Type | Description |
|-----|------|-------------|
| `ticket_uuid` | string (37) | **NEW** — badge's own ticket UUID, stored at enrollment |

**Source**: returned by `POST /api/v1/link-user-to-badge` response field `ticket_uuid`. Also retrievable via `GET /api/v1/lookup-attendee/<machine_id>` as fallback.

### Removed (not needed)

~~`badge_contacts` namespace~~ — eliminated. Server is authoritative.
~~`boop_cnt` key~~ — eliminated. Army size derived from `GET /api/v1/boops` response count.

### Unchanged

All existing keys in `badge` namespace: `paired`, `role`, `name`, `title`, `company`, `atype`, `qr`, `qrlen`, `bmp`, `bmplen`.
All existing keys in `badge_identity` namespace: `uuid`, `hmac_secret`, `enrolled`, `enroll_token`.

---

## Activity Types (string constants, defined in `BadgeAPI_types.h`)

| Constant | Value | Description |
|----------|-------|-------------|
| `PING_TYPE_EMOJI` | `"emoji"` | Messages feature |
| `PING_TYPE_CHALLENGE` | `"conquest_challenge"` | Conquest: initiate battle |
| `PING_TYPE_RESPONSE` | `"conquest_response"` | Conquest: auto-reply |

---

## Ephemeral App State

### Messages Screen State (C layer, heap or static)

| Variable | Type | Notes |
|----------|------|-------|
| `boops[]` | BoopRecord[50] | Fetched from server on enter |
| `partnerNames[]` | char[50][64] | Fetched per-boop from `/boops/{id}/partner` |
| `partnerCompanies[]` | char[50][64] | Same source |
| `partnerTickets[]` | char[50][37] | Resolved from `BoopRecord.ticket_uuids` using own ticket UUID |
| `boopCount` | int | Number of active boops |
| `selectedContact` | int | Index into boops[] |
| `selectedEmoji` | int | Index 0..7 |
| `lastPingId` | char[37] | Cursor: last seen ping ID |
| `lastPingTs` | char[32] | Cursor: last seen ping timestamp |
| `screenState` | enum | LOADING / CONTACT_LIST / EMOJI_PALETTE / SENDING / SENT / ERROR / INCOMING |
| `lastPollMs` | uint32_t | millis() of last GET /api/v1/pings |

### Conquest App State (Python layer)

```python
state = {
    "army_size": 0,       # len(non-revoked boops)
    "boops": [],           # list of dicts from badge.boops()
    "selected": 0,
    "screen": "LIST",      # LIST | LOADING | SENDING | WAITING | RESULT | EMPTY
    "challenge_sent_at": 0,
    "result": None
}
```
