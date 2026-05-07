# Contract: Firmware-Facing Endpoint Subset

This document defines the interface contract between `BadgeAPI.h` (the firmware module)
and the backend API. It is derived from `specs/openapi.json` and serves as the human-readable
companion to that machine-readable spec.

**Scope**: Exactly the 7 functions named in Constitution Principle VII plus helper functions
`cancelBoop`, `fetchBadgeXBM`, and `probeBadgeExistence`.

---

## Function Signatures (BadgeAPI.h public surface)

```cpp
namespace BadgeAPI {

    // GET /api/v1/badge/{uid}/qr.xbm
    // Returns XBM bitmap bytes. result.buf is malloc'd; caller must free().
    FetchQRResult getQRBitmap(const char* uid);

    // GET /api/v1/badge/{uid}/info
    // Returns name, title, company, attendee_type as char arrays.
    BadgeInfoResult getBadgeInfo(const char* uid);

    // GET /api/v1/badge/{uid}/info (bitmap + role)
    // Returns bitmap bytes. result.buf is malloc'd; caller must free().
    FetchBadgeXBMResult fetchBadgeXBM(const char* uid);

    // GET /api/v1/badge/{uid}/info (status probe, no field parsing)
    // Returns only ok + httpCode. Used by osUnpairedFlow polling loop.
    ProbeResult probeBadgeExistence(const char* uid);

    // POST /api/v1/boops  { "badge_uuids": [myUID, theirUID] }
    // HTTP 202 → BOOP_STATUS_PENDING; workflowId populated.
    // HTTP 200 → BOOP_STATUS_CONFIRMED; pairingId populated; partner fields populated.
    BoopResult createBoop(const char* myUID, const char* theirUID);

    // GET /api/v1/boops/status/{workflowId}?badge_uuid={myUID}
    // Always HTTP 200 from backend. status field carries the semantic result.
    BoopStatusResult getBoopStatus(const char* workflowId, const char* myUID);

    // DELETE /api/v1/boops/pending  { "badge_uuids": [myUID, theirUID] }
    APIResult cancelBoop(const char* myUID, const char* theirUID);

    // GET /api/v1/boops/{pairingId}/partner?badge_uuid={myUID}
    // Consent-gated. Returns 403 if myUID is not a participant in the pairing.
    BoopPartnerResult getBoopPartner(int pairingId, const char* myUID);

    // POST /api/v1/pings  { source_badge_uuid, target_ticket_uuid, activity_type, data }
    // Requires active pairing between source and target. Returns 403 if no consent.
    SendPingResult sendPing(const char* sourceBadgeUuid,
                            const char* targetTicketUuid,
                            const char* activityType,
                            const char* dataJson);

    // GET /api/v1/pings?requester_badge_uuid=...&source=...&type=...&limit=...
    // Returns up to BADGE_PINGS_MAX_RECORDS records per page.
    // Pass next_cursor fields as before_ts + before_id for subsequent pages.
    GetPingsResult getPings(const char* requesterBadgeUuid,
                            const char* source,
                            const char* target,
                            const char* activityType,
                            int         limit,
                            const char* beforeTs,   // NULL for first page
                            const char* beforeId);  // NULL for first page

}  // namespace BadgeAPI
```

---

## Endpoint Details

### GET /api/v1/badge/{id}/qr.xbm

| Field | Value |
|-------|-------|
| Auth required | No |
| Success code | 200 |
| Content-Type | `image/x-xbitmap` |
| Response | Binary XBM data (128×64 = 1,024 bytes expected) |
| Failure codes | any non-200 → `ok=false` |

Firmware function: `getQRBitmap(uid)`

---

### GET /api/v1/badge/{id}/info

| Field | Value |
|-------|-------|
| Auth required | No |
| Success code | 200 |
| Response schema | `BadgeInfoResponse` |

```json
{
  "name": "string | null",
  "title": "string | null",
  "company": "string | null",
  "bitmap": "[int] | null",
  "attendee_type": "string | null"
}
```

Firmware functions: `getBadgeInfo(uid)`, `fetchBadgeXBM(uid)`, `probeBadgeExistence(uid)`

---

### POST /api/v1/boops

| Field | Value |
|-------|-------|
| Auth required | No (HMAC stub present, inactive) |
| Request body | `{ "badge_uuids": ["<requesting>", "<other>"] }` |
| Success codes | 202 (pending), 200 (confirmed) |
| Failure codes | 404 (badge not registered) |

**202 Response** (`PairingPendingResponse`):
```json
{ "status": "pending", "workflow_id": "badge-pairing-..." }
```

**200 Response** (`PairingConfirmedResponse`):
```json
{
  "ok": true,
  "status": "confirmed",
  "pairing_id": 42,
  "partner_name": "string | null",
  "partner_title": "string | null",
  "partner_company": "string | null",
  "partner_attendee_type": "string | null"
}
```

Firmware function: `createBoop(myUID, theirUID)`

---

### GET /api/v1/boops/status/{workflow_id}

| Field | Value |
|-------|-------|
| Auth required | No |
| Query params | `badge_uuid` (optional — enables partner info on confirmed response) |
| Always HTTP | 200 |

**Response** (`PairingStatusResponse`):
```json
{
  "status": "pending | confirmed | not_requested",
  "workflow_id": "string (present when pending)",
  "pairing_id": "int (present when confirmed)",
  "partner_name": "string | null (present when confirmed + badge_uuid supplied)",
  "partner_title": "...",
  "partner_company": "...",
  "partner_attendee_type": "..."
}
```

Firmware function: `getBoopStatus(workflowId, myUID)`

---

### DELETE /api/v1/boops/pending

| Field | Value |
|-------|-------|
| Auth required | No |
| Request body | `{ "badge_uuids": ["<uuid1>", "<uuid2>"] }` |
| Success codes | 200 |
| Failure codes | 404 (workflow not found) |

Firmware function: `cancelBoop(myUID, theirUID)`

---

### GET /api/v1/boops/{pairing_id}/partner

| Field | Value |
|-------|-------|
| Auth required | No (consent-gated by badge_uuid query param) |
| Query params | `badge_uuid` (required) |
| Success codes | 200 |
| Failure codes | 403 (not a participant), 404 (pairing not found) |

**Response** (`PartnerInfoResponse`):
```json
{
  "pairing_id": 42,
  "partner_name": "string | null",
  "partner_title": "string | null",
  "partner_company": "string | null",
  "partner_attendee_type": "string | null"
}
```

Firmware function: `getBoopPartner(pairingId, myUID)`

---

### POST /api/v1/pings

| Field | Value |
|-------|-------|
| Auth required | No (HMAC stub present, inactive) |
| Request body | `SendPingBody` |
| Success codes | 200 |
| Failure codes | 403 (no consent), 422 (self-ping), 503 (Temporal unavailable) |

**Request body**:
```json
{
  "source_badge_uuid": "string",
  "target_ticket_uuid": "string",
  "activity_type": "string",
  "data": {}
}
```

**Response** (`PingRecord`):
```json
{
  "id": "uuid",
  "source_ticket_uuid": "uuid",
  "target_ticket_uuid": "uuid",
  "activity_type": "string",
  "data": {},
  "created_at": "ISO 8601",
  "updated_at": "ISO 8601"
}
```

Firmware function: `sendPing(sourceBadgeUuid, targetTicketUuid, activityType, dataJson)`

---

### GET /api/v1/pings

| Field | Value |
|-------|-------|
| Auth required | No (HMAC stub present, inactive) |
| Query params | `requester_badge_uuid` (required), `source` or `target` (one required), `type` (optional), `limit` (default 20, max 100), `before_ts` + `before_id` (both or neither) |
| Success codes | 200 |
| Failure codes | 403 (no consent), 422 (missing source/target or incomplete cursor) |

**Response** (`PingListResponse`):
```json
{
  "events": [PingRecord, ...],
  "next_cursor": "base64-encoded-cursor | null"
}
```

Firmware function: `getPings(requesterBadgeUuid, source, target, activityType, limit, beforeTs, beforeId)`

---

## Auth Stub Contract

The `_request()` transport in `BadgeAPI.cpp` accepts a `bool needsAuth` parameter.

```cpp
// AUTH STUB: when needsAuth == true, inject HMAC headers here.
// X-Badge-ID:    <badge_uuid>
// X-Timestamp:  <unix_seconds>
// X-Signature:  <lowercase_hex_HMAC>
// Currently a no-op. Auth will be added in a future feature.
if (needsAuth) {
    // TODO: implement HMAC header injection
}
```

Functions that will require auth once implemented: `createBoop`, `sendPing`, `getPings`.
All others are unauthenticated endpoints per the backend contract.

---

## Buffer Overflow Safety Contract

All `BadgeAPI` implementations MUST:
1. Use `strncpy(dest, src, sizeof(dest) - 1)` and explicitly null-terminate `dest`.
2. Never write past the declared size of any char array field.
3. For `PingRecord.data`, serialize the JSON body to a temp buffer, copy at most
   `BADGE_PING_DATA_MAX - 1` bytes, and null-terminate.
4. For `GetPingsResult.records`, iterate at most `BADGE_PINGS_MAX_RECORDS` array elements
   regardless of how many records the backend returned.
