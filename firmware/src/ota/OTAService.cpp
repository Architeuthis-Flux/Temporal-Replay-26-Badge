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

// Fresh-connect HTTPS cadence. Firing api.github.com + the community
// registry fetch in the same scheduler tick was tripping
// mbedtls/ESP-IDF TLS with "esp-aes: Failed to allocate memory" on
// badges where the WiFi bring-up + GUI left little contiguous internal
// heap — the two handshakes overlapped and peak allocation spiked.
// We defer after L2 association, run OTA first, then wait for the TLS
// stack to tear down before kicking the async registry worker.
constexpr uint32_t kWifiOtaDeferMs = 2500;
constexpr uint32_t kAfterOtaBeforeRegistryMs = 800;
// Conservative floor for WiFiClientSecure + mbedTLS crypto on S3.
constexpr uint32_t kMinHeapForTls = 48 * 1024;

uint32_t sBootMs = 0;
bool sRollbackHandled = false;
bool sWifiWasConnected = false;
// 0 = disconnected / idle; 1 = waiting kWifiOtaDeferMs; 2 = run OTA
// check (once); 3 = waiting gap before registry; 4 = kicked registry;
// 5 = done until disconnect.
uint8_t sPostConnectPhase = 0;
uint32_t sPostConnectMarkMs = 0;

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

  const bool wifiUp = wifiService.isConnected();
  if (!wifiUp) {
    sWifiWasConnected = false;
    sPostConnectPhase = 0;
    return;
  }
  if (!sWifiWasConnected) {
    sWifiWasConnected = true;
    sPostConnectPhase = 1;
    sPostConnectMarkMs = now;
  }
  if (sPostConnectPhase >= 5) return;

  switch (sPostConnectPhase) {
    case 1: {
      if ((uint32_t)(now - sPostConnectMarkMs) < kWifiOtaDeferMs) return;
      if (ESP.getFreeHeap() < kMinHeapForTls) return;
      DBG("[ota-svc] WiFi up — OTA check (deferred)\n");
      ota::checkNow(true);
      sPostConnectPhase = 2;
      sPostConnectMarkMs = now;
      break;
    }
    case 2: {
      if ((uint32_t)(now - sPostConnectMarkMs) < kAfterOtaBeforeRegistryMs) {
        return;
      }
      if (ESP.getFreeHeap() < kMinHeapForTls) return;
      DBG("[ota-svc] registry refresh (after OTA)\n");
      if (!registry::beginRefreshAsync(true)) {
        sPostConnectMarkMs = now;
        return;
      }
      sPostConnectPhase = 3;
      sPostConnectMarkMs = now;
      break;
    }
    case 3: {
      // Registry worker clears isRefreshing() when done; latch "done"
      // so we do not respawn if the user stays on WiFi for hours.
      if (!registry::isRefreshing()) {
        sPostConnectPhase = 5;
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace ota
