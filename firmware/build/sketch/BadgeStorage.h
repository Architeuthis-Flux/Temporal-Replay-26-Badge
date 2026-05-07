#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeStorage.h"
// BadgeStorage.h — Public interface for the BadgeStorage module
// See FR-007 in spec.md
//
// Owns the Preferences (NVS) instance. All NVS key names are private to BadgeStorage.cpp.
// Callers never see NVS key strings.
//
// Buffer rules:
//   loadBadgeBitmap allocates via ps_malloc() — caller must free() the returned buffer.

#pragma once
#include <Arduino.h>

namespace BadgeStorage {

    // Persist paired state + role to NVS. role: ROLE_NUM_* constant.
    void savePaired(int role);

    // Load badge state from NVS.
    // Returns true if badge was previously paired; false if never paired.
    // Sets *outRole to the persisted role (defaults to ROLE_NUM_ATTENDEE if never paired).
    bool loadState(int* outRole);

    // Save nametag bitmap (XBM bytes) to NVS.
    void saveBadgeBitmap(const uint8_t* bits, int len);

    // Load nametag bitmap from NVS into a fresh malloc'd buffer.
    // On success: sets *outBuf and *outLen. Returns true. Caller must free().
    // On failure: *outBuf = nullptr, *outLen = 0. Returns false.
    bool loadBadgeBitmap(uint8_t** outBuf, int* outLen);

    // Save badge identity text fields (name, title, company, attendeeType) to NVS.
    void saveBadgeInfo(const char* name, const char* title,
                       const char* company, const char* attendeeType);

    // Load badge identity text fields from NVS.
    // Returns true if any data was previously saved.
    bool loadBadgeInfo(char* name, int nameLen,
                       char* title, int titleLen,
                       char* company, int companyLen,
                       char* attendeeType, int typeLen);

    // spec-009: Save badge's own ticket UUID to NVS (badge_identity namespace).
    // ticket_uuid: 36-char UUID string (null-terminated).
    void saveMyTicketUUID(const char* ticket_uuid);

    // spec-009: Load badge's own ticket UUID from NVS into out (maxLen bytes).
    // Returns false if not yet stored (badge not enrolled or pre-spec-009 firmware).
    bool loadMyTicketUUID(char* out, size_t maxLen);

    // Clear all persisted badge state (pairing, identity, QR, bitmap, ticket_uuid).
    // Used by the "clear-state" serial command and when server returns 404 on boot.
    void clearPaired();

}  // namespace BadgeStorage
