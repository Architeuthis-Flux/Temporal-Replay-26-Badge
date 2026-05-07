/**
 * BadgeAPI additions for spec-009: Ping Mechanic Demo
 *
 * These declarations belong in BadgeAPI.h alongside existing functions.
 * Implementation goes in BadgeAPI.cpp.
 * Types go in BadgeAPI_types.h.
 *
 * spec-009 adds: sendPing, getPings
 * Also adds internal HMAC auth helper (not exposed externally).
 */

/* ──────────────────────────────────────────────────────────
   New types — add to BadgeAPI_types.h
   ────────────────────────────────────────────────────────── */

/* Activity type constants */
#define PING_TYPE_EMOJI      "emoji"
#define PING_TYPE_CHALLENGE  "conquest_challenge"
#define PING_TYPE_RESPONSE   "conquest_response"

/* Max pings returned per GET call (embedded-safe) */
#define PING_FETCH_MAX       10

/* PingRecord — matches GET /api/v1/pings response schema */
typedef struct {
    char id[37];                    /* UUID v4 */
    char source_ticket_uuid[37];
    char target_ticket_uuid[37];
    char activity_type[32];
    char data[256];                 /* raw JSON object string */
    char created_at[32];            /* ISO 8601 — used as pagination cursor */
} PingRecord;

/* Result of POST /api/v1/pings */
typedef struct {
    bool ok;
    int  httpCode;
    char pingId[37];                /* UUID of created ping; empty on failure */
} SendPingResult;

/* Result of GET /api/v1/pings */
typedef struct {
    bool       ok;
    int        httpCode;
    int        count;               /* number of valid entries in pings[] */
    PingRecord pings[PING_FETCH_MAX];
    char       nextCursorTs[32];    /* empty string when no next page */
    char       nextCursorId[37];    /* empty string when no next page */
} GetPingsResult;

/* ──────────────────────────────────────────────────────────
   New functions — add to BadgeAPI.h (namespace BadgeAPI)
   ────────────────────────────────────────────────────────── */

namespace BadgeAPI {

    /**
     * POST /api/v1/pings
     *
     * Sends a directed async ping to target_ticket_uuid.
     * HMAC auth headers are attached internally using the badge's NVS key.
     *
     * @param source_badge_uuid  Badge hardware UUID (from BadgeUID::getUID())
     * @param target_ticket_uuid Recipient's ticket UUID (from ContactEntry.ticket)
     * @param activity_type      Stream name; use PING_TYPE_* constants
     * @param data_json          JSON object string for the data field; e.g. {"emoji":"♥"}
     * @return SendPingResult    .ok true on HTTP 200; false otherwise
     */
    SendPingResult sendPing(const char* source_badge_uuid,
                            const char* target_ticket_uuid,
                            const char* activity_type,
                            const char* data_json);

    /**
     * GET /api/v1/pings
     *
     * Fetches recent pings for this badge (as source or target).
     * Pass NULL for cursor fields on the first call.
     * Pass nextCursorTs + nextCursorId from the previous result for pagination.
     * HMAC auth headers are attached internally.
     *
     * @param requester_badge_uuid  Badge hardware UUID
     * @param activity_type         Filter by type; NULL to fetch all types
     * @param limit                 Max records (capped at PING_FETCH_MAX)
     * @param before_ts             Cursor timestamp; NULL for first page
     * @param before_id             Cursor ping ID; NULL for first page
     * @return GetPingsResult       .ok true on HTTP 200
     */
    GetPingsResult getPings(const char* requester_badge_uuid,
                            const char* activity_type,
                            uint8_t     limit,
                            const char* before_ts,
                            const char* before_id);

} /* namespace BadgeAPI */

/* ──────────────────────────────────────────────────────────
   Internal HMAC helper — private to BadgeAPI.cpp
   NOT declared in BadgeAPI.h (internal linkage)
   ────────────────────────────────────────────────────────── */

/*
 * static void attachHMACHeaders(HTTPClient& client,
 *                                const char* badge_uuid);
 *
 * Reads hmac_secret (32-byte blob) from NVS namespace "badge_identity".
 * Reads current unix time via time(nullptr) (NTP must be synced).
 * Computes HMAC-SHA256 over (badge_uuid + timestamp_string).
 * Sets headers: X-Badge-ID, X-Timestamp, X-Signature.
 * No-op if hmac_secret is absent (unrolled badge, dev bypass).
 */
