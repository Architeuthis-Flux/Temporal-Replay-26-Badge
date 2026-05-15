#include "OTAService.h"

#include <Arduino.h>

#include "../api/WiFiService.h"
#include "../infra/DebugLog.h"
#include "AssetRegistry.h"
#include "BadgeOTA.h"

namespace ota {

OTAService otaService;

namespace {

constexpr uint32_t kHealthyBootMs = 30000;         // 30 s post-setup before
                                                   // we mark the running app
                                                   // valid (rollback gate)

uint32_t sBootMs = 0;
bool sRollbackHandled = false;
// WiFi-up edge detector. The OTA + community-apps cooldowns are gone
// (see plan): we now fire a check exactly once per WiFi-up edge so a
// freshly-connected badge picks up new releases / registry entries
// within seconds, and reconnect cycles re-trigger the check.
bool sWifiWasConnected = false;
bool sCheckedSinceConnect = false;

}  // namespace

void OTAService::service() {
  const uint32_t now = millis();
  if (sBootMs == 0) sBootMs = now;

  // Rollback gate. If we booted into ESP_OTA_IMG_PENDING_VERIFY and
  // we've been ticking for 30 s without a panic/reset, mark the
  // running app as valid so the bootloader stops considering rollback.
  if (!sRollbackHandled && (now - sBootMs) >= kHealthyBootMs) {
    if (runningPendingVerify()) {
      markCurrentAppValidIfPending();
    }
    sRollbackHandled = true;
  }

  // WiFi-up edge: rising edge arms a one-shot check; a disconnect
  // re-arms it for the next reconnect.
  const bool wifiUp = wifiService.isConnected();
  if (!wifiUp) {
    sWifiWasConnected = false;
    sCheckedSinceConnect = false;
    return;
  }
  if (!sWifiWasConnected) {
    sWifiWasConnected = true;
    sCheckedSinceConnect = false;
  }
  if (sCheckedSinceConnect) return;
  // The clock isn't strictly required for either call (the cooldown
  // gate that depended on it has been removed) but checkNow's HTTPS
  // call to api.github.com needs working DNS+TLS, which is reliable
  // by the time WiFiService reports connected. Fire once and latch.
  DBG("[ota-svc] WiFi up — firing OTA check + registry refresh\n");
  ota::checkNow(true);
  registry::beginRefreshAsync(true);
  sCheckedSinceConnect = true;
}

}  // namespace ota
