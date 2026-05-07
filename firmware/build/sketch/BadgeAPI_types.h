#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeAPI_types.h"
#pragma once
// GENERATED — do not edit by hand.
// Regenerate with: python scripts/gen_badge_api_types.py
// Source: specs/openapi.json
// See data-model.md for field rationale and size decisions.

#include <stdint.h>
#include <stdbool.h>

// ─── Size constants ────────────────────────────────────────────────────────────

// String field maximums — sized to fit the largest expected value plus null terminator.
// See Decision 2 in specs/006-openapi-badge-api/research.md for sizing rationale.
#define BADGE_FIELD_NAME_MAX      64   // name, title, company, partner_name, etc.
#define BADGE_FIELD_TYPE_MAX      16   // attendee_type: 'Attendee', 'Staff', etc. (8 chars max)
#define BADGE_UUID_MAX            37   // UUID v4 canonical form: 36 chars + null terminator
#define BADGE_WORKFLOW_ID_MAX     96   // 'badge-pairing-<uuid>-<uuid>' ~82 chars + margin
#define BADGE_PING_ACTIVITY_MAX   32   // activity_type stream name (e.g. 'room_checkin' = 12)
#define BADGE_PING_DATA_MAX      256   // raw JSON payload char buffer; truncation documented
#define BADGE_PING_CURSOR_MAX    128   // base64-encoded keyset cursor (before_ts + before_id)
#define BADGE_PING_TIMESTAMP_MAX  32   // ISO 8601: '2026-03-13T00:00:00.000Z' = 24 chars
#define BADGE_PINGS_MAX_RECORDS    8   // fixed-capacity page size for GetPingsResult
#define BADGE_BOOPS_MAX_RECORDS   50   // fixed-capacity page size for GetBoopsResult

// ─── Status enum ───────────────────────────────────────────────────────────────

// Pairing workflow state — replaces raw 'pending'/'confirmed'/'not_requested' strings.
// UNKNOWN maps to any parse failure so callers always have a defined value.
typedef enum {
    BOOP_STATUS_UNKNOWN       = 0,  // parse failure or unrecognized value
    BOOP_STATUS_PENDING       = 1,  // workflow running; poll again
    BOOP_STATUS_CONFIRMED     = 2,  // mutual consent complete
    BOOP_STATUS_NOT_REQUESTED = 3   // expired, cancelled, or never started
} BadgeBoopStatus;

// ─── Base result struct ────────────────────────────────────────────────────────

// All API result structs embed these two fields as their first members.
// Invariant: ok == false — all other fields are zero / empty strings.
typedef struct {
    bool ok;        // false on network error, non-2xx, or parse failure
    int  httpCode;  // raw HTTP response code; 0 if connection failed
} BadgeAPIResult;

// ─── Badge info result structs  (schema: BadgeInfoResponse) ────────────────────

// GET /api/v1/badge/{uid}/info — attendee nametag fields.
typedef struct {
    bool ok;
    int  httpCode;
    char name[BADGE_FIELD_NAME_MAX];
    char title[BADGE_FIELD_NAME_MAX];
    char company[BADGE_FIELD_NAME_MAX];
    char attendee_type[BADGE_FIELD_TYPE_MAX];
} BadgeInfoResult;

// GET /api/v1/badge/{uid}/qr.xbm — raw XBM bitmap.
// buf is heap-allocated; caller must free(). buf == NULL on failure.
// XBM bitmaps are 1024 bytes — too large for the 8 KB Core-0 stack.
typedef struct {
    bool     ok;
    int      httpCode;
    uint8_t* buf;   // heap-allocated XBM bytes; caller must free()
    int      len;   // byte count; 0 on failure
} FetchQRResult;

// GET /api/v1/badge/{uid}/info — bitmap bytes + role parsed from JSON.
// buf is heap-allocated; caller must free(). buf == NULL on failure.
typedef struct {
    bool     ok;
    int      httpCode;
    uint8_t* buf;          // heap-allocated bitmap bytes; caller must free()
    int      len;
    int      assignedRole; // ROLE_NUM_* constant from BadgeConfig.h
    char     name[BADGE_FIELD_NAME_MAX];
    char     title[BADGE_FIELD_NAME_MAX];
    char     company[BADGE_FIELD_NAME_MAX];
    char     attendeeType[BADGE_FIELD_TYPE_MAX];
} FetchBadgeXBMResult;

// GET /api/v1/badge/{uid}/info — status-only probe, fields not parsed.
// Used by osUnpairedFlow() polling loop to check registration status.
typedef struct {
    bool ok;        // true if HTTP 200; false otherwise
    int  httpCode;
} ProbeResult;

// ─── Boop (pairing) result structs ─────────────────────────────────────────────

// POST /api/v1/boops — create pairing consent.
// HTTP 202 -> BOOP_STATUS_PENDING + workflowId populated.
// HTTP 200 -> BOOP_STATUS_CONFIRMED + pairingId + partner fields populated.
typedef struct {
    bool            ok;
    int             httpCode;
    BadgeBoopStatus status;
    char            workflowId[BADGE_WORKFLOW_ID_MAX];  // non-empty when PENDING
    int             pairingId;                          // non-zero when CONFIRMED
    char            partnerName[BADGE_FIELD_NAME_MAX];
    char            partnerTitle[BADGE_FIELD_NAME_MAX];
    char            partnerCompany[BADGE_FIELD_NAME_MAX];
    char            partnerAttendeeType[BADGE_FIELD_TYPE_MAX];
} BoopResult;

// GET /api/v1/boops/status/{workflow_id} — poll pairing workflow.
// schema: PairingStatusResponse
typedef struct {
    bool            ok;
    int             httpCode;
    BadgeBoopStatus status;
    char            workflowId[BADGE_WORKFLOW_ID_MAX];  // present when PENDING
    int             pairingId;                          // present when CONFIRMED
    char            partnerName[BADGE_FIELD_NAME_MAX];
    char            partnerTitle[BADGE_FIELD_NAME_MAX];
    char            partnerCompany[BADGE_FIELD_NAME_MAX];
    char            partnerAttendeeType[BADGE_FIELD_TYPE_MAX];
} BoopStatusResult;

// GET /api/v1/boops/{pairing_id}/partner — consent-gated partner nametag.
// schema: PartnerInfoResponse
typedef struct {
    bool ok;
    int  httpCode;
    int  pairingId;
    char partnerName[BADGE_FIELD_NAME_MAX];
    char partnerTitle[BADGE_FIELD_NAME_MAX];
    char partnerCompany[BADGE_FIELD_NAME_MAX];
    char partnerAttendeeType[BADGE_FIELD_TYPE_MAX];
} BoopPartnerResult;

// ─── Ping result structs ───────────────────────────────────────────────────────

// A single directed ping record — shared by sendPing and getPings.
// schema: PingRecord
typedef struct {
    char id[BADGE_UUID_MAX];
    char source_ticket_uuid[BADGE_UUID_MAX];
    char target_ticket_uuid[BADGE_UUID_MAX];
    char activity_type[BADGE_PING_ACTIVITY_MAX];
    char data[BADGE_PING_DATA_MAX];         // raw JSON object, null-terminated
    char created_at[BADGE_PING_TIMESTAMP_MAX];
    char updated_at[BADGE_PING_TIMESTAMP_MAX];
} PingRecord;

// POST /api/v1/pings — send a directed ping.
typedef struct {
    bool       ok;
    int        httpCode;
    PingRecord record;  // populated on ok == true
} SendPingResult;

// GET /api/v1/pings — paginated ping stream.
// schema: PingListResponse
// records[] is fixed-capacity; excess backend records are silently discarded.
// Use next_cursor (before_ts + before_id) to page.
typedef struct {
    bool       ok;
    int        httpCode;
    PingRecord records[BADGE_PINGS_MAX_RECORDS];
    int        count;                           // actual records in this page
    char       next_cursor[BADGE_PING_CURSOR_MAX]; // empty string if no further pages
} GetPingsResult;

// ─── spec-009 additions (not in OpenAPI allowlist — hand-maintained) ──────────

// Activity type string constants for ping endpoints (spec-009)
#define PING_TYPE_EMOJI     "emoji"              // spec-009: Messages feature
#define PING_TYPE_CHALLENGE "conquest_challenge" // spec-009: Conquest initiate
#define PING_TYPE_RESPONSE  "conquest_response"  // spec-009: Conquest auto-reply
#define PING_FETCH_MAX      BADGE_PINGS_MAX_RECORDS  // spec-009: alias for max page size

// spec-009: Single active pairing from GET /api/v1/boops?badge_uuid=
typedef struct {
    int  id;
    char ticket_uuids[2][BADGE_UUID_MAX];  // both participants (sorted)
    int  boop_count;
    char revoked_at[BADGE_PING_TIMESTAMP_MAX];  // empty = active; ISO ts = revoked
} BoopRecord;

// spec-009: Result of GET /api/v1/boops — active (non-revoked) pairings only.
typedef struct {
    bool       ok;
    int        httpCode;
    int        count;
    BoopRecord boops[BADGE_BOOPS_MAX_RECORDS];
} GetBoopsResult;
