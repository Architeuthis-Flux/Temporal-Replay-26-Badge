#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeAPI.h"
// BadgeAPI.h — Public interface for the BadgeAPI module
// All types are defined in BadgeAPI_types.h (generated from specs/openapi.json).
//
// All functions run synchronously on the calling task's stack.
// Callers on Core 0 (irTask, 8 KB): createBoop
// Callers on Core 1 (setup / loop): fetchBadgeXBM, probeBadgeExistence
//
// Buffer ownership:
//   fetchBadgeXBM allocates via ps_malloc() (PSRAM-preferred).
//   Callers are responsible for free(result.buf).
//   On failure (result.ok == false), result.buf is nullptr.

#pragma once
#include <Arduino.h>
#include "BadgeAPI_types.h"

namespace BadgeAPI {

    // GET /api/v1/badge/{uid}/info (bitmap + attendee_type fields)
    // Parses bitmap JSON array and attendee_type -> assignedRole.
    // On success: result.buf is malloc'd bitmap; result.assignedRole is ROLE_NUM_*.
    FetchBadgeXBMResult fetchBadgeXBM(const char* uid);

    // GET /api/v1/badge/{uid}/info (status probe, no field parsing)
    // Returns only ok + httpCode. Used by osUnpairedFlow() polling loop.
    ProbeResult probeBadgeExistence(const char* uid);

    // POST /api/v1/boops  { badge_uuids: [myUID, theirUID] }
    // HTTP 200 -> BOOP_STATUS_CONFIRMED (second badge). pairingId + partner fields populated.
    // HTTP 202 -> BOOP_STATUS_PENDING (first badge). workflowId populated.
    BoopResult createBoop(const char* myUID, const char* theirUID);

    // spec-009: GET /api/v1/boops?badge_uuid=<badge_uuid>
    // Returns active (non-revoked) pairings. No auth required.
    GetBoopsResult getBoops(const char* badge_uuid);

    // spec-009: GET /api/v1/boops/{id}/partner?badge_uuid=<badge_uuid>
    // Returns partner_name and partner_company for a pairing.
    BoopPartnerResult getBoopPartner(int pairing_id, const char* badge_uuid);

    // spec-009: POST /api/v1/pings  {source_badge_uuid, target_ticket_uuid, activity_type, data}
    // HMAC auth attached internally. Returns ok=false + httpCode on any error.
    SendPingResult sendPing(const char* source_badge_uuid,
                            const char* target_ticket_uuid,
                            const char* activity_type,
                            const char* data_json);

    // spec-009: GET /api/v1/pings with requester + target filter.
    // target_ticket_uuid required. activity_type may be NULL. HMAC auth attached.
    // Pass before_ts/before_id from a prior GetPingsResult for pagination; NULL for first page.
    GetPingsResult getPings(const char* requester_badge_uuid,
                            const char* target_ticket_uuid,
                            const char* activity_type,
                            int         limit,
                            const char* before_ts,
                            const char* before_id);

}  // namespace BadgeAPI
