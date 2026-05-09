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

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  WiFi.setHostname(hostname);

  Serial.printf("[WiFi] explicit connect to ssid_len=%u\n",
                static_cast<unsigned>(strlen(WIFI_SSID)));
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  const uint32_t startMs = millis();
  while (millis() - startMs < WIFI_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) break;
    const IPAddress ip = WiFi.localIP();
    if (static_cast<uint32_t>(ip) != 0) break;
    delay(100);
  }

  const bool ok = WiFi.status() == WL_CONNECTED ||
                  static_cast<uint32_t>(WiFi.localIP()) != 0;
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
    Serial.printf("[WiFi] connect failed status=%d\n", WiFi.status());
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
