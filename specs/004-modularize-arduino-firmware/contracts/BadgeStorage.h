// BadgeStorage.h — Public interface for the BadgeStorage module
// See FR-007 in spec.md
//
// Owns the Preferences (NVS) instance. All NVS key names are private to BadgeStorage.cpp.
// Callers never see NVS key strings.
//
// Buffer rules:
//   nvsSaveQR takes bits by const pointer — does not take ownership.
//   nvsLoadQR allocates via malloc() — caller must free() the returned buffer
//   (or accept that it is stored in the global qrBits pointer in main sketch).

#pragma once
#include <Arduino.h>

namespace BadgeStorage {

    // Save QR XBM bytes to NVS.
    // bits: pointer to byte array; len: number of bytes.
    void saveQR(const uint8_t* bits, int len);

    // Load QR XBM bytes from NVS into a fresh malloc'd buffer.
    // On success: sets *outBuf to allocated buffer and *outLen to byte count. Returns true.
    // On failure (nothing cached): *outBuf = nullptr, *outLen = 0. Returns false.
    bool loadQR(uint8_t** outBuf, int* outLen);

    // Persist paired state + role to NVS.
    // uid: uid_hex string (stored for reference); role: ROLE_NUM_* constant.
    void savePaired(const char* uid, int role);

    // Load badge state from NVS.
    // Returns true if badge was previously paired; false if never paired.
    // Sets *outRole to the persisted role (defaults to ROLE_NUM_ATTENDEE if never paired).
    bool loadState(int* outRole);

}  // namespace BadgeStorage
