#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgePairing.cpp"
// BadgePairing.cpp — WiFi connection, badge association, and re-pair flow

#include "BadgePairing.h"
#include "BadgeConfig.h"
#include "BadgeVersion.h"
#include "BadgeDisplay.h"
#include "BadgeStorage.h"
#include "BadgeAPI.h"
#include "BadgeQR.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ─── Externs — global state owned by the main sketch ─────────────────────────
extern BadgeState badgeState;
extern volatile int assignedRole;
extern uint8_t*   qrBits;
extern int        qrByteCount;
extern int        qrWidth;
extern int        qrHeight;
extern uint8_t*   badgeBits;
extern int        badgeByteCount;
extern char       uid_hex[];
extern char       badgeName[];
extern char       badgeTitle[];
extern char       badgeCompany[];
extern char       badgeAtType[];

// ─── Helpers ─────────────────────────────────────────────────────────────────

// spec-009: Fetch own ticket UUID from lookup-attendee and persist to NVS.
// Called at enrollment time (T006) and at boot as fallback (T007).
void fetchAndSaveTicketUUID(const char* badge_uuid) {
  char url[256];
  snprintf(url, sizeof(url), "%s/api/v1/lookup-attendee/%s", SERVER_URL, badge_uuid);
  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.begin(tls, url);
  http.setTimeout(8000);
  int code = http.GET();
  if (code == 200) {
    String resp = http.getString();
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, resp) == DeserializationError::Ok) {
      const char* ticket = doc["ticket_uuid"] | "";
      if (ticket[0] != '\0') {
        BadgeStorage::saveMyTicketUUID(ticket);
        Serial.printf("[spec-009] ticket_uuid saved: %s\n", ticket);
      }
    }
  } else {
    Serial.printf("[spec-009] lookup-attendee HTTP %d — ticket_uuid not saved\n", code);
  }
  http.end();
}

static void applyBadgeIdentity(const FetchBadgeXBMResult& xbm) {
  strncpy(badgeName,    xbm.name,         63); badgeName[63]    = '\0';
  strncpy(badgeTitle,   xbm.title,        63); badgeTitle[63]   = '\0';
  strncpy(badgeCompany, xbm.company,      63); badgeCompany[63] = '\0';
  strncpy(badgeAtType,  xbm.attendeeType, 15); badgeAtType[15]  = '\0';
}

// ─── Non-blocking QR pairing state ───────────────────────────────────────────

volatile bool qrPairingActive  = false;
volatile bool qrCheckRequested = false;
char          qrPollStatus[24] = "";

static volatile bool    probeInFlight    = false;
static volatile int     probeResultCode  = 0;
static volatile bool    probeResultReady = false;

struct ProbeTaskCtx { char uid[13]; };
static ProbeTaskCtx probeCtx;

static void probeTaskFn(void*) {
  ProbeResult r    = BadgeAPI::probeBadgeExistence(probeCtx.uid);
  probeResultCode  = r.httpCode;
  probeResultReady = true;
  probeInFlight    = false;
  vTaskDelete(nullptr);
}

// ─── pollQRPairing — called from loop() ──────────────────────────────────────

void pollQRPairing() {
  if (!qrPairingActive) return;

  // Process completed probe
  if (probeResultReady) {
    probeResultReady = false;
    int code = probeResultCode;
    Serial.printf("poll HTTP %d\n", code);

    if (code == 200) {
      FetchBadgeXBMResult xbm = BadgeAPI::fetchBadgeXBM(uid_hex);
      if (xbm.ok) {
        if (badgeBits) free(badgeBits);
        badgeBits      = xbm.buf;
        badgeByteCount = xbm.len;
        assignedRole   = xbm.assignedRole;
        applyBadgeIdentity(xbm);
        BadgeStorage::savePaired(assignedRole);
        BadgeStorage::saveBadgeBitmap(xbm.buf, xbm.len);
        BadgeStorage::saveBadgeInfo(xbm.name, xbm.title, xbm.company, xbm.attendeeType);
        // spec-009 T006: persist own ticket_uuid at enrollment time
        char existingTicket[BADGE_UUID_MAX] = "";
        if (!BadgeStorage::loadMyTicketUUID(existingTicket, sizeof(existingTicket))) {
          fetchAndSaveTicketUUID(uid_hex);
        }
      }
      badgeState      = BADGE_PAIRED;
      qrPairingActive = false;
      renderMode      = MODE_MENU;
      setScreenText("Paired!", "");
      Serial.println("Badge associated!");
      screenDirty = true;
    } else if (code == -1) {
      strncpy(qrPollStatus, "conn err", sizeof(qrPollStatus) - 1);
      if (!qrBits || qrByteCount == 0) screenDirty = true;  // only redraw if QR not showing
    } else {
      snprintf(qrPollStatus, sizeof(qrPollStatus), "HTTP %d", code);
      if (!qrBits || qrByteCount == 0) screenDirty = true;
    }
  }

  // Launch a probe when the user requests it (BTN_DOWN on QR screen)
  if (qrCheckRequested && !probeInFlight) {
    qrCheckRequested = false;
    strncpy(qrPollStatus, "checking...", sizeof(qrPollStatus) - 1);
    if (!qrBits || qrByteCount == 0) screenDirty = true;
    Serial.print("Checking badge endpoint... ");
    memcpy(probeCtx.uid, uid_hex, 13);
    probeInFlight    = true;
    probeResultReady = false;
    xTaskCreatePinnedToCore(probeTaskFn, "probe", 8192, nullptr, 1,
                            NULL, 0);
  }
}

// ─── wifiConnect ──────────────────────────────────────────────────────────────
static bool wifiConnect() {
  WiFi.disconnect(true);
  delay(100);
  WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);  // accept WPA/WPA2/WPA3 Personal

  for (int attempt = 1; attempt <= 3; attempt++) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
      delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
      WiFi.setSleep(false);  // keep modem awake — prevents TCP failures from loop()
      return true;
    }
    Serial.printf("WiFi attempt %d failed, retrying...\n", attempt);
    WiFi.disconnect(true);
    delay(500);
  }
  return false;
}

// ─── osConnectWiFi ────────────────────────────────────────────────────────────
static bool osConnectWiFi() {
  bootPrint("fw: " FIRMWARE_VERSION);
  delay(1500);

  char welcomeMsg[64];
  snprintf(welcomeMsg, sizeof(welcomeMsg), "Welcome! UID: %s", uid_hex);
  Serial.println(welcomeMsg);
  bootPrint(welcomeMsg);
  delay(1500);

  char connectMsg[64];
  snprintf(connectMsg, sizeof(connectMsg), "Connecting to %s...", WIFI_SSID);
  bootPrint(connectMsg);
  bool ok = wifiConnect();
  if (ok) {
    char connectedMsg[64];
    snprintf(connectedMsg, sizeof(connectedMsg), "Connected to %s", WIFI_SSID);
    bootPrint(connectedMsg);
    delay(1000);
  } else {
    bootPrint("WiFi failed. Entering demo mode.");
    delay(2000);
  }
  return ok;
}

// ─── generateQRLocal — generate QR on-device; shared by osRun and osUnpairedFlow
static bool generateQRLocal() {
  if (qrBits) { free(qrBits); qrBits = nullptr; qrByteCount = 0; }

  char url[128];
  snprintf(url, sizeof(url), "%s/b/%s", SERVER_URL, uid_hex);

  bootPrint("Generating QR...");
  LocalQRResult qr = BadgeQR::generate(url);
  if (qr.ok) {
    qrBits      = qr.buf;
    qrByteCount = qr.len;
    qrWidth     = qr.w;
    qrHeight    = qr.h;
    Serial.printf("[QR] local %dx%d ver=%d genMs=%lu\n",
                  qrWidth, qrHeight, qr.modules, qr.genMs);
    bootPrint("QR: ready!");
    delay(300);
    return true;
  }

  bootPrint("QR generation failed.");
  delay(2000);
  return false;
}

// ─── osUnpairedFlow ───────────────────────────────────────────────────────────
// Fetches or restores cached QR, then arms the non-blocking pairing poller.
// Returns immediately; loop() drives association via pollQRPairing().
static void osUnpairedFlow() {
  if (!generateQRLocal()) return;  // qrPairingActive stays false

  // Arm non-blocking association check (manual trigger via BTN_DOWN)
  strncpy(qrPollStatus, "DOWN to check", sizeof(qrPollStatus) - 1);
  qrCheckRequested = false;
  probeResultReady = false;
  probeInFlight    = false;
  qrPairingActive  = true;
  renderMode       = MODE_QR;
  screenDirty      = true;
  Serial.println("Polling for badge association (non-blocking)...");
}

// ─── osPairedFlow ─────────────────────────────────────────────────────────────
static void osPairedFlow() {
  // QR is generated locally on demand — no NVS load needed

  bootPrint("Refreshing badge...");
  FetchBadgeXBMResult xbm = BadgeAPI::fetchBadgeXBM(uid_hex);
  if (xbm.ok) {
    if (badgeBits) free(badgeBits);
    badgeBits      = xbm.buf;
    badgeByteCount = xbm.len;
    assignedRole   = xbm.assignedRole;
    applyBadgeIdentity(xbm);
    BadgeStorage::saveBadgeBitmap(xbm.buf, xbm.len);
    BadgeStorage::saveBadgeInfo(xbm.name, xbm.title, xbm.company, xbm.attendeeType);
    bootPrint("Badge updated!");
    delay(1000);
  } else {
    // Fall back to cached data if network unavailable
    int cachedLen = 0;
    uint8_t* cachedBuf = nullptr;
    if (BadgeStorage::loadBadgeBitmap(&cachedBuf, &cachedLen)) {
      if (badgeBits) free(badgeBits);
      badgeBits      = cachedBuf;
      badgeByteCount = cachedLen;
    }
    BadgeStorage::loadBadgeInfo(badgeName, 64, badgeTitle, 64,
                                badgeCompany, 64, badgeAtType, 16);
    bootPrint("Using cached badge.");
    delay(1000);
  }
}

// ─── rePair ───────────────────────────────────────────────────────────────────
void rePair() {
  renderMode = MODE_BOOT;

  if (WiFi.status() != WL_CONNECTED) {
    char reconnectMsg[64];
    snprintf(reconnectMsg, sizeof(reconnectMsg), "Reconnecting to %s...", WIFI_SSID);
    bootPrint(reconnectMsg);
    bool ok = wifiConnect();
    if (!ok) {
      bootPrint("WiFi failed. Check credentials.");
      delay(2500);
      renderMode = MODE_MENU;
      setScreenText("", "");
      renderScreen();
      return;
    }
    bootPrint("Connected!");
    delay(800);
  }

  if (badgeState == BADGE_PAIRED) {
    osPairedFlow();
    renderMode = MODE_MENU;
    setScreenText("", "");
    renderScreen();
  } else {
    osUnpairedFlow();
    // If QR pairing is armed, stay in MODE_QR — loop() handles it
    // If QR fetch failed, drop back to menu
    if (!qrPairingActive) {
      renderMode = MODE_MENU;
      setScreenText("", "");
      renderScreen();
    }
  }
}

// ─── osRun ────────────────────────────────────────────────────────────────────
void osRun() {
  renderMode = MODE_BOOT;

  // Load cached badge identity from NVS immediately — no WiFi needed
  if (badgeState == BADGE_PAIRED) {
    BadgeStorage::loadBadgeBitmap(&badgeBits, &badgeByteCount);
    BadgeStorage::loadBadgeInfo(badgeName, 64, badgeTitle, 64,
                                badgeCompany, 64, badgeAtType, 16);
  }

  bool wifiOk = osConnectWiFi();

  if (!wifiOk) {
    bootPrint("WiFi failed. Press > to retry.");
    delay(2000);
  } else {
    // Always query server — refreshes identity and detects unenrollment
    bootPrint("Checking badge...");
    FetchBadgeXBMResult xbm = BadgeAPI::fetchBadgeXBM(uid_hex);
    if (xbm.ok) {
      // Server knows this badge — update identity and ensure paired state
      if (badgeBits) free(badgeBits);
      badgeBits      = xbm.buf;
      badgeByteCount = xbm.len;
      assignedRole   = xbm.assignedRole;
      applyBadgeIdentity(xbm);
      BadgeStorage::savePaired(assignedRole);
      BadgeStorage::saveBadgeBitmap(xbm.buf, xbm.len);
      BadgeStorage::saveBadgeInfo(xbm.name, xbm.title, xbm.company, xbm.attendeeType);
      badgeState = BADGE_PAIRED;
      bootPrint("Badge updated!");
      delay(500);
    } else if (xbm.httpCode == 404 && badgeState == BADGE_PAIRED) {
      // Badge deleted from server — wipe NVS and re-enter pairing flow
      Serial.println("[osRun] 404 on paired badge — clearing NVS, re-pairing");
      BadgeStorage::clearPaired();
      badgeState     = BADGE_UNPAIRED;
      badgeName[0]   = '\0';
      badgeTitle[0]  = '\0';
      badgeCompany[0]= '\0';
      if (badgeBits) { free(badgeBits); badgeBits = nullptr; badgeByteCount = 0; }
      if (qrBits)    { free(qrBits);    qrBits    = nullptr; qrByteCount    = 0; }
      generateQRLocal();
    } else if (badgeState == BADGE_PAIRED) {
      // Network failure for paired badge — keep cached identity
      Serial.printf("[osRun] fetchBadgeXBM HTTP %d — using cache\n", xbm.httpCode);
    } else {
      // Unpaired and server doesn't know this badge — show QR for pairing
      generateQRLocal();
    }
  }

  // Generate QR locally if missing or stale (old 128×64 server format > 512 bytes)
  if (badgeState == BADGE_PAIRED && (!qrBits || qrByteCount == 0 || qrByteCount > 512)) {
    generateQRLocal();
  }

  // spec-009 T007: boot fallback — fetch own ticket_uuid if not yet in NVS.
  // Handles badges enrolled before spec-009 that never got ticket_uuid persisted.
  if (wifiOk && badgeState == BADGE_PAIRED) {
    char ticket[BADGE_UUID_MAX] = "";
    if (!BadgeStorage::loadMyTicketUUID(ticket, sizeof(ticket))) {
      Serial.println("[spec-009] ticket_uuid absent — fetching from lookup-attendee...");
      fetchAndSaveTicketUUID(uid_hex);
    } else {
      Serial.printf("[spec-009] ticket_uuid loaded: %s\n", ticket);
    }
  }

  renderMode = MODE_MENU;
  setScreenText("", "");
  renderScreen();

  Serial.print("Entering main loop. State: ");
  Serial.println(badgeState == BADGE_PAIRED ? "PAIRED" : "UNPAIRED");
}
