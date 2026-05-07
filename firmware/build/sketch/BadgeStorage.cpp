#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeStorage.cpp"
// BadgeStorage.cpp — NVS persistence via Preferences library

#include "BadgeStorage.h"
#include <Preferences.h>

// File-scope Preferences instance; all NVS keys are private to this file.
static Preferences prefs;

static const char* NS        = "badge";
static const char* NS_ID     = "badge_identity"; // spec-009: shared with HMAC enrollment
static const char* K_PAIR    = "paired";
static const char* K_ROLE    = "role";
static const char* K_BMP     = "bmp";
static const char* K_BMPLEN  = "bmplen";
static const char* K_NAME    = "name";
static const char* K_TITLE   = "title";
static const char* K_COMPANY = "company";
static const char* K_ATYPE   = "atype";

namespace BadgeStorage {

void savePaired(int role) {
  prefs.begin(NS, false);
  prefs.putBool(K_PAIR, true);
  prefs.putInt(K_ROLE, role);
  prefs.end();
}

bool loadState(int* outRole) {
  prefs.begin(NS, true);
  bool wasPaired = prefs.getBool(K_PAIR, false);
  *outRole = prefs.getInt(K_ROLE, 1); // 1 = ROLE_NUM_ATTENDEE
  prefs.end();
  return wasPaired;
}

void saveBadgeInfo(const char* name, const char* title,
                   const char* company, const char* attendeeType) {
  prefs.begin(NS, false);
  prefs.putString(K_NAME,    name);
  prefs.putString(K_TITLE,   title);
  prefs.putString(K_COMPANY, company);
  prefs.putString(K_ATYPE,   attendeeType);
  prefs.end();
}

bool loadBadgeInfo(char* name, int nameLen,
                   char* title, int titleLen,
                   char* company, int companyLen,
                   char* attendeeType, int typeLen) {
  prefs.begin(NS, true);
  bool hasData = prefs.isKey(K_NAME);
  if (hasData) {
    prefs.getString(K_NAME,    name,        nameLen);
    prefs.getString(K_TITLE,   title,       titleLen);
    prefs.getString(K_COMPANY, company,     companyLen);
    prefs.getString(K_ATYPE,   attendeeType, typeLen);
  }
  prefs.end();
  return hasData;
}

void saveBadgeBitmap(const uint8_t* bits, int len) {
  prefs.begin(NS, false);
  prefs.putBytes(K_BMP, bits, len);
  prefs.putInt(K_BMPLEN, len);
  prefs.end();
}

bool loadBadgeBitmap(uint8_t** outBuf, int* outLen) {
  *outBuf = nullptr;
  *outLen = 0;

  prefs.begin(NS, true);
  int len = prefs.getInt(K_BMPLEN, 0);
  if (len == 0) { prefs.end(); return false; }

  uint8_t* buf = (uint8_t*)ps_malloc(len);
  if (!buf) { prefs.end(); return false; }

  prefs.getBytes(K_BMP, buf, len);
  prefs.end();

  *outBuf = buf;
  *outLen = len;
  return true;
}

// spec-009 ─── Own ticket UUID ─────────────────────────────────────────────────

void saveMyTicketUUID(const char* ticket_uuid) {
  prefs.begin(NS_ID, false);
  prefs.putString("ticket_uuid", ticket_uuid);
  prefs.end();
}

bool loadMyTicketUUID(char* out, size_t maxLen) {
  prefs.begin(NS_ID, true);
  bool present = prefs.isKey("ticket_uuid");
  if (present) {
    prefs.getString("ticket_uuid", out, (unsigned int)maxLen);
  }
  prefs.end();
  return present;
}

void clearPaired() {
  prefs.begin(NS, false);
  prefs.clear();   // wipe all keys in "badge" namespace (QR, bitmap, identity, paired flag)
  prefs.end();
  prefs.begin(NS_ID, false);
  prefs.clear();   // wipe "badge_identity" namespace (ticket_uuid, hmac_secret, etc.)
  prefs.end();
}

} // namespace BadgeStorage
