#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeAPI.cpp"
// BadgeAPI.cpp — HTTP transport + all public API functions
// Types defined in BadgeAPI_types.h (generated from specs/openapi.json).

#include "BadgeAPI.h"
#include "BadgeConfig.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include "esp_hmac.h"

// ─── spec-009: HMAC auth helper ───────────────────────────────────────────────

// attachHMACHeaders — file-scope static; not part of BadgeAPI public ABI.
// Reads hmac_secret presence from NVS badge_identity namespace as enrollment sentinel.
// Computes HMAC-SHA256 over "<badge_uuid><unix_timestamp>" using eFuse HMAC_KEY4.
// Sets X-Badge-ID, X-Timestamp, X-Signature headers on client.
// No-op (and no header set) if hmac_secret is absent (badge not enrolled).
static void attachHMACHeaders(HTTPClient& client, const char* badge_uuid) {
  // Enrollment check: hmac_secret must be present in NVS
  Preferences prefs;
  prefs.begin("badge_identity", true);
  size_t secretLen = prefs.getBytesLength("hmac_secret");
  prefs.end();
  if (secretLen == 0) return;  // no-op — badge not enrolled

  // Build HMAC message: badge_uuid + unix_timestamp (no separator)
  time_t now = time(nullptr);
  char tsStr[20];
  snprintf(tsStr, sizeof(tsStr), "%lld", (long long)now);

  char msg[64];
  snprintf(msg, sizeof(msg), "%s%s", badge_uuid, tsStr);

  // Compute HMAC-SHA256 via ESP32 eFuse HMAC hardware peripheral (HMAC_KEY4 = BLK_KEY4)
  uint8_t hmac[32] = {};
  esp_err_t err = esp_hmac_calculate(HMAC_KEY4,
                                     (const void*)msg, strlen(msg),
                                     hmac);
  if (err != ESP_OK) {
    Serial.printf("[HMAC] esp_hmac_calculate error: %d\n", (int)err);
    return;
  }

  // Format as lowercase hex
  char sigHex[65];
  for (int i = 0; i < 32; i++) {
    snprintf(&sigHex[i * 2], 3, "%02x", hmac[i]);
  }

  client.addHeader("X-Badge-ID",   badge_uuid);
  client.addHeader("X-Timestamp",  tsStr);
  client.addHeader("X-Signature",  sigHex);
}

// ─── Private transport ────────────────────────────────────────────────────────

// Single WiFiClientSecure shared across all _request() calls.
// setInsecure() skips cert verification (no CA store on badge).
// _tls.stop() is called before every request to prevent mbedTLS hangs
// when the static client is reused across FreeRTOS task contexts (Core 0/1).
static WiFiClientSecure _tls;
static void _ensureTLS() { _tls.setInsecure(); }

// Execute one HTTP request. Returns true if a response was received (any code).
// url:         full URL including SERVER_URL prefix
// method:      "GET", "POST", "DELETE"
// body:        request body (empty string for GET)
// responseOut: populated with response body when a response is received
// httpCodeOut: set to raw HTTP code (0 on connection failure)
// needsAuth:   when true, attaches HMAC headers using badgeUuid
// badgeUuid:   required when needsAuth=true; the badge's hardware UID string
static void _badgeInfoUrl(char* buf, size_t len, const char* uid) {
  snprintf(buf, len, "%s%s/%s/info", SERVER_URL, EP_BADGE, uid);
}

static bool _request(const char* method,
                     const char* url,
                     const char* body,
                     String&     responseOut,
                     int&        httpCodeOut,
                     bool        needsAuth  = false,
                     int         timeoutMs  = 8000,
                     const char* badgeUuid  = nullptr) {
  if (WiFi.status() != WL_CONNECTED) {
    httpCodeOut = -1;
    return false;
  }
  _ensureTLS();
  // Always reset the TLS connection before use. The static _tls is shared
  // across call sites on Core 0 (irTask) and Core 1 (loop); reusing a live
  // connection from a different FreeRTOS task context causes mbedTLS hangs.
  // Stopping upfront ensures every request starts from a clean TCP/TLS state.
  _tls.stop();
  HTTPClient http;
  http.begin(_tls, url);
  http.setTimeout(timeoutMs);

  if (needsAuth && badgeUuid) {
    attachHMACHeaders(http, badgeUuid);
  }

  int code = 0;
  if (strcmp(method, "GET") == 0) {
    code = http.GET();
  } else if (strcmp(method, "POST") == 0) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST((uint8_t*)body, strlen(body));
  } else if (strcmp(method, "DELETE") == 0) {
    http.addHeader("Content-Type", "application/json");
    code = http.sendRequest("DELETE", (uint8_t*)body, strlen(body));
  }

  httpCodeOut = code;
  if (code > 0) {
    responseOut = http.getString();
  } else if (code < 0) {
    // Retry once on connection-level failure
    http.end();
    _tls.stop();
    delay(100);
    _ensureTLS();
    http.begin(_tls, url);
    http.setTimeout(timeoutMs);
    if (needsAuth && badgeUuid) {
      attachHMACHeaders(http, badgeUuid);
    }
    if (strcmp(method, "GET") == 0) {
      code = http.GET();
    } else if (strcmp(method, "POST") == 0) {
      http.addHeader("Content-Type", "application/json");
      code = http.POST((uint8_t*)body, strlen(body));
    } else if (strcmp(method, "DELETE") == 0) {
      http.addHeader("Content-Type", "application/json");
      code = http.sendRequest("DELETE", (uint8_t*)body, strlen(body));
    }
    httpCodeOut = code;
    if (code > 0) responseOut = http.getString();
  }
  http.end();
  return (code > 0);
}

// ─── Public API ───────────────────────────────────────────────────────────────

namespace BadgeAPI {


FetchBadgeXBMResult fetchBadgeXBM(const char* uid) {
  FetchBadgeXBMResult r = {};
  r.buf          = nullptr;
  r.len          = 0;
  r.assignedRole = 1; // ROLE_NUM_ATTENDEE default

  char url[256];
  _badgeInfoUrl(url, sizeof(url), uid);

  String resp;
  int code;
  if (!_request("GET", url, "", resp, code)) { r.httpCode = code; r.ok = false; return r; }
  r.httpCode = code;
  if (code != 200) { r.ok = false; return r; }

  DynamicJsonDocument doc(20480);
  if (deserializeJson(doc, resp) != DeserializationError::Ok) { r.ok = false; return r; }

  // Parse identity fields
  const char* attendeeType = doc["attendee_type"] | "";
  strncpy(r.attendeeType, attendeeType, sizeof(r.attendeeType) - 1);
  if      (strcmp(attendeeType, "Staff")   == 0) r.assignedRole = 2;
  else if (strcmp(attendeeType, "Vendor")  == 0) r.assignedRole = 3;
  else if (strcmp(attendeeType, "Speaker") == 0) r.assignedRole = 4;
  else                                           r.assignedRole = 1;

  const char* v;
  v = doc["name"]    | ""; strncpy(r.name,    v, sizeof(r.name)    - 1);
  v = doc["title"]   | ""; strncpy(r.title,   v, sizeof(r.title)   - 1);
  v = doc["company"] | ""; strncpy(r.company, v, sizeof(r.company) - 1);

  // Parse bitmap array
  JsonArray bitmap = doc["bitmap"];
  int bitmapLen = bitmap.size();
  if (bitmapLen == 0) { r.ok = false; return r; }

  r.buf = (uint8_t*)ps_malloc(bitmapLen);
  if (!r.buf) { r.ok = false; return r; }

  for (int i = 0; i < bitmapLen; i++) {
    r.buf[i] = (uint8_t)(int)bitmap[i];
  }
  r.len = bitmapLen;
  r.ok  = true;
  return r;
}

ProbeResult probeBadgeExistence(const char* uid) {
  ProbeResult r = {};
  char url[256];
  _badgeInfoUrl(url, sizeof(url), uid);

  String resp;
  int code;
  _request("GET", url, "", resp, code);
  r.httpCode = code;
  r.ok = (code == 200);
  return r;
}

// ── Boop (pairing) ──────────────────────────────────────────────────────────

BoopResult createBoop(const char* myUID, const char* theirUID) {
  BoopResult r = {};
  char url[256];
  snprintf(url, sizeof(url), "%s%s", SERVER_URL, EP_BOOPS);

  char body[256];
  snprintf(body, sizeof(body),
           "{\"badge_uuids\":[\"%s\",\"%s\"]}", myUID, theirUID);

  String resp;
  int code;
  if (!_request("POST", url, body, resp, code, /*needsAuth=*/true, 8000, myUID)) {
    r.httpCode = code; r.ok = false; return r;
  }
  r.httpCode = code;

  if (code == 200) {
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, resp) == DeserializationError::Ok) {
      const char* v;
      v = doc["partner_name"]          | ""; strncpy(r.partnerName,         v, BADGE_FIELD_NAME_MAX - 1); r.partnerName[BADGE_FIELD_NAME_MAX - 1]         = '\0';
      v = doc["partner_title"]         | ""; strncpy(r.partnerTitle,        v, BADGE_FIELD_NAME_MAX - 1); r.partnerTitle[BADGE_FIELD_NAME_MAX - 1]        = '\0';
      v = doc["partner_company"]       | ""; strncpy(r.partnerCompany,      v, BADGE_FIELD_NAME_MAX - 1); r.partnerCompany[BADGE_FIELD_NAME_MAX - 1]      = '\0';
      v = doc["partner_attendee_type"] | ""; strncpy(r.partnerAttendeeType, v, BADGE_FIELD_TYPE_MAX - 1); r.partnerAttendeeType[BADGE_FIELD_TYPE_MAX - 1] = '\0';
      r.pairingId = doc["pairing_id"] | 0;
    }
    r.status = BOOP_STATUS_CONFIRMED;
    r.ok = true;
  } else if (code == 202) {
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, resp) == DeserializationError::Ok) {
      const char* wfId = doc["workflow_id"] | "";
      strncpy(r.workflowId, wfId, BADGE_WORKFLOW_ID_MAX - 1);
      r.workflowId[BADGE_WORKFLOW_ID_MAX - 1] = '\0';
    }
    r.status = BOOP_STATUS_PENDING;
    r.ok = true;
  } else {
    r.status = BOOP_STATUS_UNKNOWN;
    r.ok = false;
  }
  return r;
}

// ── spec-009: getBoops ───────────────────────────────────────────────────────

GetBoopsResult getBoops(const char* badge_uuid) {
  GetBoopsResult r = {};

  char url[256];
  snprintf(url, sizeof(url), "%s/api/v1/boops?badge_uuid=%s", SERVER_URL, badge_uuid);

  Serial.printf("[getBoops] url=%s\n", url);
  String resp;
  int code;
  if (!_request("GET", url, "", resp, code)) {
    r.httpCode = code; r.ok = false; return r;
  }
  r.httpCode = code;
  if (code != 200) { r.ok = false; return r; }

  // Parse: {"pairings": [...]}
  StaticJsonDocument<8192> doc;
  if (deserializeJson(doc, resp) != DeserializationError::Ok) {
    r.ok = false; return r;
  }

  JsonArray pairings = doc["pairings"];
  int count = 0;
  for (JsonObject p : pairings) {
    if (count >= 50) break;
    // Filter revoked pairings: revoked_at is null or empty string = active
    const char* revokedAt = p["revoked_at"] | "";
    if (revokedAt != nullptr && revokedAt[0] != '\0') continue;

    BoopRecord& b = r.boops[count];
    b.id         = p["id"] | 0;
    b.boop_count = p["boop_count"] | 0;

    // ticket_uuids is a 2-element JSON array
    JsonArray tids = p["ticket_uuids"];
    for (int i = 0; i < 2 && i < (int)tids.size(); i++) {
      const char* t = tids[i] | "";
      strncpy(b.ticket_uuids[i], t, BADGE_UUID_MAX - 1);
      b.ticket_uuids[i][BADGE_UUID_MAX - 1] = '\0';
    }

    b.revoked_at[0] = '\0';  // active — revoked_at is null/empty
    count++;
  }

  r.count = count;
  r.ok    = true;
  return r;
}

// ── spec-009: getBoopPartner ─────────────────────────────────────────────────

BoopPartnerResult getBoopPartner(int pairing_id, const char* badge_uuid) {
  BoopPartnerResult r = {};

  char url[256];
  snprintf(url, sizeof(url), "%s/api/v1/boops/%d/partner?badge_uuid=%s",
           SERVER_URL, pairing_id, badge_uuid);

  String resp;
  int code;
  if (!_request("GET", url, "", resp, code, false, 6000)) {
    r.httpCode = code; r.ok = false; return r;
  }
  r.httpCode = code;
  if (code != 200) { r.ok = false; return r; }

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, resp) == DeserializationError::Ok) {
    const char* n = doc["partner_name"]    | "";
    const char* c = doc["partner_company"] | "";
    strncpy(r.partnerName,    n, BADGE_FIELD_NAME_MAX - 1);
    strncpy(r.partnerCompany, c, BADGE_FIELD_NAME_MAX - 1);
    r.ok = true;
  }
  return r;
}

// ── spec-009: sendPing ───────────────────────────────────────────────────────

SendPingResult sendPing(const char* source_badge_uuid,
                        const char* target_ticket_uuid,
                        const char* activity_type,
                        const char* data_json) {
  SendPingResult r = {};

  char url[256];
  snprintf(url, sizeof(url), "%s/api/v1/pings", SERVER_URL);

  // Build JSON body with data field embedded verbatim
  char body[512];
  snprintf(body, sizeof(body),
           "{\"source_badge_uuid\":\"%s\","
           "\"target_ticket_uuid\":\"%s\","
           "\"activity_type\":\"%s\","
           "\"data\":%s}",
           source_badge_uuid, target_ticket_uuid, activity_type,
           (data_json && data_json[0] != '\0') ? data_json : "{}");

  _ensureTLS();
  HTTPClient http;
  http.begin(_tls, url);
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  attachHMACHeaders(http, source_badge_uuid);

  int code = http.POST((uint8_t*)body, strlen(body));
  r.httpCode = code;

  if (code == 200) {
    String resp = http.getString();
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, resp) == DeserializationError::Ok) {
      const char* pid = doc["id"] | "";
      strncpy(r.record.id, pid, BADGE_UUID_MAX - 1);
      r.record.id[BADGE_UUID_MAX - 1] = '\0';
    }
    r.ok = true;
  } else {
    r.ok = false;
  }
  http.end();
  return r;
}

// ── spec-009: getPings ───────────────────────────────────────────────────────

GetPingsResult getPings(const char* requester_badge_uuid,
                        const char* target_ticket_uuid,
                        const char* activity_type,
                        int         limit,
                        const char* before_ts,
                        const char* before_id) {
  GetPingsResult r = {};

  // Cap limit at PING_FETCH_MAX
  if (limit <= 0 || limit > PING_FETCH_MAX) limit = PING_FETCH_MAX;

  // Build query string
  char url[512];
  int pos = snprintf(url, sizeof(url),
                     "%s/api/v1/pings?requester_badge_uuid=%s&target=%s&limit=%d",
                     SERVER_URL, requester_badge_uuid, target_ticket_uuid, limit);

  if (activity_type && activity_type[0] != '\0') {
    pos += snprintf(url + pos, sizeof(url) - pos, "&type=%s", activity_type);
  }
  if (before_ts && before_ts[0] != '\0') {
    pos += snprintf(url + pos, sizeof(url) - pos, "&before_ts=%s", before_ts);
  }
  if (before_id && before_id[0] != '\0') {
    pos += snprintf(url + pos, sizeof(url) - pos, "&before_id=%s", before_id);
  }

  _ensureTLS();
  HTTPClient http;
  http.begin(_tls, url);
  http.setTimeout(8000);
  attachHMACHeaders(http, requester_badge_uuid);

  int code = http.GET();
  r.httpCode = code;

  if (code == 200) {
    String resp = http.getString();
    StaticJsonDocument<4096> doc;
    if (deserializeJson(doc, resp) == DeserializationError::Ok) {
      JsonArray events = doc["events"];
      int count = 0;
      for (JsonObject ev : events) {
        if (count >= BADGE_PINGS_MAX_RECORDS) break;
        PingRecord& p = r.records[count];

        const char* v;
        v = ev["id"]                  | ""; strncpy(p.id,                  v, BADGE_UUID_MAX - 1); p.id[BADGE_UUID_MAX - 1]                  = '\0';
        v = ev["source_ticket_uuid"]  | ""; strncpy(p.source_ticket_uuid,  v, BADGE_UUID_MAX - 1); p.source_ticket_uuid[BADGE_UUID_MAX - 1]  = '\0';
        v = ev["target_ticket_uuid"]  | ""; strncpy(p.target_ticket_uuid,  v, BADGE_UUID_MAX - 1); p.target_ticket_uuid[BADGE_UUID_MAX - 1]  = '\0';
        v = ev["activity_type"]       | ""; strncpy(p.activity_type,       v, BADGE_PING_ACTIVITY_MAX - 1); p.activity_type[BADGE_PING_ACTIVITY_MAX - 1] = '\0';
        v = ev["created_at"]          | ""; strncpy(p.created_at,          v, BADGE_PING_TIMESTAMP_MAX - 1); p.created_at[BADGE_PING_TIMESTAMP_MAX - 1]  = '\0';

        // Serialize data object back to string
        String dataStr;
        serializeJson(ev["data"], dataStr);
        strncpy(p.data, dataStr.c_str(), sizeof(p.data) - 1);
        p.data[sizeof(p.data) - 1] = '\0';

        count++;
      }
      r.count = count;

      // Populate next_cursor from newest record id (first in reverse-chrono order)
      if (count > 0) {
        strncpy(r.next_cursor, r.records[0].id, sizeof(r.next_cursor) - 1);
        r.next_cursor[sizeof(r.next_cursor) - 1] = '\0';
      }
      r.ok = true;
    }
  } else {
    r.ok = false;
  }
  http.end();
  return r;
}

} // namespace BadgeAPI
