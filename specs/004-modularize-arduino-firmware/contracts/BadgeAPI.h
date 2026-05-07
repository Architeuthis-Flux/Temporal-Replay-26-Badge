// BadgeAPI.h — Public interface for the BadgeAPI module
// See FR-011 through FR-020 in spec.md
//
// All functions run synchronously on the calling task's stack.
// Callers on Core 0 (irTask, 8 KB): createBoop, getBoopStatus, cancelBoop
// Callers on Core 1 (setup / osUnpairedFlow): fetchQR, fetchBadgeXBM, getBadgeInfo
//
// Buffer ownership:
//   fetchQR and fetchBadgeXBM allocate via malloc().
//   Callers are responsible for free(result.buf).
//   On failure (result.ok == false), result.buf is nullptr.

#pragma once
#include <Arduino.h>

// ─── Result structs ───────────────────────────────────────────────────────────

struct APIResult {
    bool ok;         // false on network error, 404, or parse failure
    int  httpCode;   // raw HTTP response code; 0 if connection failed
};

struct BoopResult : public APIResult {
    String workflowId;   // non-empty on 202 (first badge); empty on 200 or error
    String status;       // "pending" | "confirmed"
    String partnerName;  // from partner_name field on 200 response; may be empty
};

struct BoopStatusResult : public APIResult {
    String status;       // "pending" | "confirmed" | "not_requested"
};

struct FetchQRResult : public APIResult {
    uint8_t* buf;        // heap-allocated XBM bytes; caller must free()
    int      len;        // number of bytes in buf
};

struct FetchBadgeXBMResult : public APIResult {
    uint8_t* buf;        // heap-allocated bitmap bytes; caller must free()
    int      len;
    int      assignedRole;  // ROLE_NUM_* constant from BadgeConfig.h
};

struct BadgeInfoResult : public APIResult {
    String name;
    String title;
    String company;
    String attendeeType;
};

// ─── Public API ───────────────────────────────────────────────────────────────

namespace BadgeAPI {

    // GET /api/v1/badge/{uid}/info
    // Returns name, title, company, attendee_type.
    // Does NOT return bitmap — use fetchBadgeXBM for that.
    BadgeInfoResult getBadgeInfo(const char* uid);

    // POST /api/v1/boops  { badge_uuids: [myUID, theirUID] }
    // HTTP 200 → confirmed immediately (second badge in). workflowId empty.
    // HTTP 202 → pending (first badge in). workflowId populated.
    BoopResult createBoop(const char* myUID, const char* theirUID);

    // GET /api/v1/boops/status/{workflowId}?badge_uuid={myUID}
    BoopStatusResult getBoopStatus(const char* workflowId, const char* myUID);

    // DELETE /api/v1/boops/pending  { badge_uuids: [myUID, theirUID] }
    APIResult cancelBoop(const char* myUID, const char* theirUID);

    // GET /api/v1/badge/{uid}/qr.xbm
    // Parses XBM hex byte array from response body.
    // On success: result.buf is malloc'd; result.len is byte count.
    FetchQRResult fetchQR(const char* uid);

    // GET /api/v1/badge/{uid}/info (bitmap + attendee_type fields)
    // Parses bitmap JSON array and attendee_type → assignedRole.
    // On success: result.buf is malloc'd bitmap; result.assignedRole is ROLE_NUM_*.
    FetchBadgeXBMResult fetchBadgeXBM(const char* uid);

}  // namespace BadgeAPI
