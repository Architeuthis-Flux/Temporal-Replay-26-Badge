#include "WiFiService.h"

#include "../identity/BadgeUID.h"
#include "../infra/BadgeConfig.h"
#include "../hardware/Power.h"

#include <WiFi.h>
#include <cstring>
#include <time.h>

#include "esp32-hal-cpu.h"
#include "esp_pm.h"

WiFiService wifiService;

namespace {
esp_pm_lock_handle_t s_wifiPmLock = nullptr;
bool s_timeConfigured = false;

constexpr const char* kNtpPrimary = "216.239.35.0";
constexpr const char* kNtpSecondary = "129.6.15.28";
constexpr time_t kValidUnixTime = 1700000000;

void configureClockOnce() {
  if (s_timeConfigured) return;
  badgeConfig.applyTimezone();
  configTzTime(badgeConfig.timezone(), kNtpPrimary, kNtpSecondary);
  s_timeConfigured = true;
}

bool clockLooksReady() {
  return time(nullptr) > kValidUnixTime;
}

}  // namespace

namespace {
void wifiAutoConnectTask(void* arg) {
  auto* svc = static_cast<WiFiService*>(arg);
  // Let setup() and the GUI settle before we monopolise the radio.
  vTaskDelay(pdMS_TO_TICKS(2500));
  if (svc != nullptr && badgeConfig.wifiEnabled()) {
    Serial.println("[WiFi] boot auto-connect attempt");
    svc->connect();
  }
  vTaskDelete(nullptr);
}
}  // namespace

void WiFiService::begin() {
  WiFi.mode(WIFI_OFF);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  if (!s_wifiPmLock) {
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "wifi", &s_wifiPmLock);
  }
  if (badgeConfig.wifiEnabled()) {
    Serial.println("[WiFi] credentials present; scheduling boot auto-connect");
    xTaskCreatePinnedToCore(wifiAutoConnectTask, "wifi_auto", 4096, this,
                            tskIDLE_PRIORITY + 1, nullptr, 0);
  } else {
    Serial.println("[WiFi] explicit networking ready; auto-connect disabled");
  }
}

namespace {
// Per-attempt timeout. When several networks are saved we don't want
// the boot path to block 15 s × N — keep individual attempts short
// enough that the worst-case full sweep still finishes within the
// historic single-attempt budget. WPA3 SAE on a known-good network
// usually completes well under 8 s; un-attended boots can wait the
// full WIFI_TIMEOUT_MS only for the *last* candidate.
constexpr uint32_t kPerAttemptTimeoutMs = 8000;

bool tryConnectOnce(const char* ssid, const char* pass, uint32_t timeoutMs) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);

  Serial.printf("[WiFi] try ssid='%s' (len=%u, pwd_len=%u)\n",
                ssid,
                static_cast<unsigned>(strlen(ssid)),
                static_cast<unsigned>(strlen(pass)));
  WiFi.begin(ssid, pass);

  const uint32_t startMs = millis();
  while (millis() - startMs < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    const IPAddress ip = WiFi.localIP();
    if (static_cast<uint32_t>(ip) != 0) return true;
    // Hard-fail status codes — bail out early so the next saved
    // network gets a real chance instead of waiting the full timeout.
    const wl_status_t st = WiFi.status();
    if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
      Serial.printf("[WiFi] short-circuit fail status=%d\n", st);
      return false;
    }
    delay(100);
  }
  Serial.printf("[WiFi] attempt timed out status=%d\n", WiFi.status());
  return false;
}
}  // namespace

bool WiFiService::connect() {
  if (isConnected()) {
    noteConnectionOk();
    return true;
  }
  if (!badgeConfig.wifiEnabled()) {
    Serial.println("[WiFi] disabled in settings or not configured; connect skipped");
    noteConnectionFailed();
    return false;
  }
  if (!Power::wifiAllowed()) {
    Serial.println("[WiFi] blocked by power policy");
    noteConnectionFailed();
    return false;
  }

  const uint32_t prevMhz = getCpuFrequencyMhz();
  if (s_wifiPmLock) {
    esp_pm_lock_acquire(s_wifiPmLock);
  } else if (prevMhz < 160) {
    setCpuFrequencyMhz(160);
  }
  auto restoreCpu = [&]() {
    if (s_wifiPmLock) esp_pm_lock_release(s_wifiPmLock);
    else if (prevMhz < 160) setCpuFrequencyMhz(prevMhz);
  };

  char hostname[32];
  snprintf(hostname, sizeof(hostname), "badge-%s", uid_hex);
  WiFi.setHostname(hostname);

  // Walk saved networks in slot order. Slot 0 is "preferred" so a
  // user who only ever fills in one network gets the same single-
  // attempt behaviour as before; multiple slots fall through on
  // per-attempt timeout / hard-fail until one works or every slot
  // has been tried.
  const uint8_t total = badgeConfig.wifiNetworkCount();
  if (total == 0) {
    Serial.println("[WiFi] no saved networks configured");
    noteConnectionFailed();
    restoreCpu();
    return false;
  }

  bool ok = false;
  uint8_t attempted = 0;
  for (uint8_t i = 0; i < Config::kMaxWifiNetworks && !ok; ++i) {
    const char* ssid = badgeConfig.wifiSsidAt(i);
    const char* pass = badgeConfig.wifiPassAt(i);
    if (!ssid || !ssid[0]) continue;
    ++attempted;
    // Last candidate gets the full timeout so a slow but valid AP
    // still has a chance even when earlier saved networks were
    // tried first.
    const uint32_t timeoutMs =
        (attempted >= total) ? WIFI_TIMEOUT_MS : kPerAttemptTimeoutMs;
    Serial.printf("[WiFi] explicit connect to slot %u (timeout=%u ms)\n",
                  static_cast<unsigned>(i),
                  static_cast<unsigned>(timeoutMs));
    ok = tryConnectOnce(ssid, pass ? pass : "", timeoutMs);
  }

  if (ok) {
    WiFi.setSleep(true);
    configureClockOnce();
    noteConnectionOk();
    Serial.printf("[WiFi] connected ip=%s\n",
                  WiFi.localIP().toString().c_str());
  } else {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    noteConnectionFailed();
    Serial.printf("[WiFi] connect failed after %u attempt%s\n",
                  static_cast<unsigned>(attempted),
                  attempted == 1 ? "" : "s");
  }

  restoreCpu();
  return ok;
}

void WiFiService::disconnect() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  networkIndicatorActive_ = false;
}

bool WiFiService::isConnected() const {
  return WiFi.status() == WL_CONNECTED ||
         static_cast<uint32_t>(WiFi.localIP()) != 0;
}

void WiFiService::refreshClockState() const {
  const time_t now = time(nullptr);
  if (now <= kValidUnixTime) return;

  clockEverReady_ = true;
  lastClockEpoch_ = static_cast<uint32_t>(now);
  lastClockSampleMs_ = millis();
}

void WiFiService::noteConnectionOk() {
  networkIndicatorActive_ = true;
  lastNetworkOkMs_ = millis();
  refreshClockState();
}

void WiFiService::noteConnectionFailed() {
  networkIndicatorActive_ = false;
  lastNetworkFailMs_ = millis();
}

void WiFiService::noteRequestOk() {
  noteConnectionOk();
}

void WiFiService::noteRequestFailed() {
  networkIndicatorActive_ = false;
  lastNetworkFailMs_ = millis();
}

bool WiFiService::clockReady() const {
  if (clockLooksReady()) refreshClockState();
  return clockEverReady_;
}

bool WiFiService::currentTime(time_t* out) const {
  if (clockLooksReady()) {
    const time_t now = time(nullptr);
    refreshClockState();
    if (out) *out = now;
    return true;
  }

  if (!clockEverReady_ || lastClockEpoch_ == 0) return false;
  if (out) {
    *out = static_cast<time_t>(
        lastClockEpoch_ + ((millis() - lastClockSampleMs_) / 1000UL));
  }
  return true;
}
