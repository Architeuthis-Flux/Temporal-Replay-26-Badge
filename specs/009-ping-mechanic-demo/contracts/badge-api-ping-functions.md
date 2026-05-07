# Contract: BadgeAPI Ping Functions (spec-009)

## Overview

`BadgeAPI` gains two new functions: `sendPing` and `getPings`. HMAC auth is added as an internal helper — not part of the public ABI. Both functions are defined in `BadgeAPI.h` and implemented in `BadgeAPI.cpp`.

---

## New Types (add to `BadgeAPI_types.h`)

### BoopRecord
Single pairing returned by `GET /api/v1/boops`.

| Field | Type | Notes |
|-------|------|-------|
| `id` | int | Pairing ID |
| `ticket_uuids` | char[2][37] | Both participants' ticket UUIDs (sorted) |
| `boop_count` | int | `len(connected_at)` pre-computed by server |
| `revoked_at` | char[32] | Empty string = active; ISO timestamp = revoked |

### GetBoopsResult
| Field | Type | Notes |
|-------|------|-------|
| `ok` | bool | true on HTTP 200 |
| `httpCode` | int | |
| `count` | int | Number of active (non-revoked) boops in `boops[]` |
| `boops` | BoopRecord[50] | Active pairings only |

---

### Activity Type Constants
```
PING_TYPE_EMOJI       = "emoji"
PING_TYPE_CHALLENGE   = "conquest_challenge"
PING_TYPE_RESPONSE    = "conquest_response"
PING_FETCH_MAX        = 10
```

### PingRecord
Single event returned by GET /api/v1/pings.

| Field | Type | Notes |
|-------|------|-------|
| `id` | char[37] | UUID v4 |
| `source_ticket_uuid` | char[37] | Sender's ticket UUID |
| `target_ticket_uuid` | char[37] | Recipient's ticket UUID |
| `activity_type` | char[32] | Stream name |
| `data` | char[256] | Raw JSON object string |
| `created_at` | char[32] | ISO 8601 — pagination cursor |

### SendPingResult
| Field | Type | Notes |
|-------|------|-------|
| `ok` | bool | true on HTTP 200 |
| `httpCode` | int | Raw HTTP status |
| `pingId` | char[37] | UUID of created ping; empty on failure |

### GetPingsResult
| Field | Type | Notes |
|-------|------|-------|
| `ok` | bool | true on HTTP 200 |
| `httpCode` | int | Raw HTTP status |
| `count` | int | Populated entries in `pings[]` |
| `pings` | PingRecord[10] | Fetched records |
| `nextCursorTs` | char[32] | Empty when no next page |
| `nextCursorId` | char[37] | Empty when no next page |

---

## New Functions (namespace `BadgeAPI`)

### `getBoops(badge_uuid)` → GetBoopsResult

- Maps to `GET /api/v1/boops?badge_uuid=<id>`.
- Returns all confirmed, non-revoked pairings for the badge.
- Filters `revoked_at != ""` client-side before populating `boops[]`.
- No auth required (endpoint is unauthenticated per server spec).
- Used by: Messages (contact list), Conquest (contact list + army size).

**URL**: `GET /api/v1/boops?badge_uuid=<badge_uuid>`

### `sendPing(source_badge_uuid, target_ticket_uuid, activity_type, data_json)` → SendPingResult

- Maps to `POST /api/v1/pings`.
- `source_badge_uuid` — badge hardware UUID from `BadgeUID::getUID()`.
- `target_ticket_uuid` — partner ticket UUID from `BoopRecord.ticket_uuids` (the element ≠ own ticket UUID).
- `activity_type` — use `PING_TYPE_*` constants.
- `data_json` — JSON object string, e.g. `{"emoji":"♥"}`.
- HMAC auth headers attached internally.
- Returns `ok=false` + `httpCode` on any error (network, 403, 422, 503).

### `getPings(requester_badge_uuid, target_ticket_uuid, activity_type, limit, before_ts, before_id)` → GetPingsResult

- Maps to `GET /api/v1/pings`.
- `target_ticket_uuid` — **required** (`source` or `target` must be present; endpoint returns 422 if both absent). Pass own ticket UUID to fetch pings directed at this badge.
- `activity_type` — filter by type; `NULL` to fetch all.
- `limit` — capped at `PING_FETCH_MAX` (10).
- `before_ts`, `before_id` — `NULL` for first page; populate from previous `GetPingsResult.nextCursor*` for pagination.
- HMAC auth headers attached internally.

---

## Internal HMAC Helper (private to `BadgeAPI.cpp`)

**Function**: `attachHMACHeaders(HTTPClient& client, const char* badge_uuid)`

**Not exposed** in `BadgeAPI.h` — implementation detail.

**Behavior**:
1. Reads `hmac_secret` (32-byte blob) from NVS namespace `badge_identity`.
2. Reads current unix timestamp via `time(nullptr)` (requires NTP sync from boot sequence).
3. Computes HMAC-SHA256 over `badge_uuid_str + timestamp_str` using `esp_hmac_calculate()`.
4. Sets request headers: `X-Badge-ID`, `X-Timestamp`, `X-Signature` (lowercase hex).
5. **No-op** if `hmac_secret` is absent — allows unrolled/dev badges to proceed without auth (graceful degradation; server will reject with 401 but badge won't crash).

---

## Backend Endpoint Reference

### POST /api/v1/pings

**Request body:**
```json
{
  "source_badge_uuid": "<hardware UUID>",
  "target_ticket_uuid": "<contact ticket UUID>",
  "activity_type": "emoji",
  "data": { "emoji": "♥" }
}
```

**Response (200):** PingRecord JSON
**Error codes:** 403 (no active boop pairing), 422 (malformed), 503 (Temporal down)

### GET /api/v1/pings

**Query parameters:**
- `requester_badge_uuid` (required)
- `type` (optional filter)
- `limit` (optional, default 20, max 100)
- `before_ts` + `before_id` (optional cursor pair)

**Response (200):** `{ "events": [PingRecord...], "next_cursor": "..." | null }`
**Error codes:** 403, 422, 503
