#ifndef WIFISERVICE_H
#define WIFISERVICE_H

#include <Arduino.h>
#include <time.h>

#include "../infra/Scheduler.h"

class WiFiService : public IService {
 public:
  void begin();

  // Explicit-only networking with one exception: if WiFi is enabled in
  // settings AND credentials are configured, begin() schedules a
  // single auto-connect attempt shortly after boot. Subsequent
  // connect() calls are still on-demand (e.g. badge.http_get/post).
  bool connect();
  void disconnect();
  bool isConnected() const;

  bool networkIndicatorActive() const { return networkIndicatorActive_; }
  bool hasEverConnected() const { return lastNetworkOkMs_ != 0 || clockEverReady_; }
  bool clockReady() const;
  bool currentTime(time_t* out) const;
  bool isAutoSyncInProgress() const { return false; }
  void requestImmediateSync() {}

  void noteConnectionOk();
  void noteConnectionFailed();
  void noteRequestOk();
  void noteRequestFailed();

  void service() override {}
  const char* name() const override { return "Network"; }

  void setCheckIntervalMs(uint32_t ms) { checkIntervalMs_ = ms; }
  void resetConnectionBackoff() {}
  void armForReconnect() {}

  void pushForeground() {}
  void popForeground() {}
  bool requestMessageFetch() { return false; }
  bool requestZigmojiFetch() { return false; }
  bool foregroundFetchInProgress() const { return false; }

 private:
  uint32_t checkIntervalMs_ = 10000;
  volatile bool networkIndicatorActive_ = false;
  volatile uint32_t lastNetworkOkMs_ = 0;
  volatile uint32_t lastNetworkFailMs_ = 0;
  mutable volatile bool clockEverReady_ = false;
  mutable volatile uint32_t lastClockEpoch_ = 0;
  mutable volatile uint32_t lastClockSampleMs_ = 0;
  void refreshClockState() const;
};

extern WiFiService wifiService;

#endif
