// BadgeConfig.h — Compatibility shim for modular conference-badge code.
// Maps compile-time constants from the original BadgeConfig.h to runtime
// Config getters (NVS / settings.txt) and CharlieDefines.h pin numbers.

#pragma once
#include <Arduino.h>
#include <stdint.h>

#include "hardware/HardwareConfig.h"
#include "Scheduler.h"

// Optional WiFi credentials. The badge never auto-connects; these are used
// only by explicit networking calls such as MicroPython badge.http_get/post.
#define WIFI_SSID      badgeConfig.wifiSsid()
#define WIFI_PASS      badgeConfig.wifiPass()

// Timing
static const int WIFI_TIMEOUT_MS    = 15000;
static const int PAIRING_TIMEOUT_MS = 15000;
static const int POLL_INTERVAL_MS   = 2000;

// Pin aliases — map modular XIAO names to Charlie board GPIOs
#define BTN_UP     BUTTON_UP
#define BTN_DOWN   BUTTON_DOWN
#define BTN_LEFT   BUTTON_LEFT
#define BTN_RIGHT  BUTTON_RIGHT

// Joystick / display tuning
static const float JOY_DEADBAND        = 0.08f;
static const float MENU_NAV_THRESHOLD  = 0.5f;
static const bool  TILT_SHOWS_BADGE    = true;

// Messaging poll interval
#define MSG_POLL_INTERVAL_MS 5000

// Role number constants kept for nametag/UI compatibility.
#define ROLE_NUM_ATTENDEE  1
#define ROLE_NUM_STAFF     2
#define ROLE_NUM_VENDOR    3
#define ROLE_NUM_SPEAKER   4
#define ROLE_NUM_DEITY     5 // this is god mode, remove this from the production build


// ---------------------------------------------------------------------------
//  Setting indices and font-family constants shared by Config + FileBrowser.
// ---------------------------------------------------------------------------

enum SettingIndex : uint8_t {
  kLedBrightness,
  kHapticEnabled,
  kHapticStrength,
  kHapticFreqHz,
  kHapticPulseMs,
  kJoySensitivity,
  kJoyDeadzone,
  kFontFamily,
  kFontSize,
  kLightSleepSec,
  kDeepSleepSec,
  kAutoFlipEnable,
  kFlipUpThreshold,
  kFlipDownThreshold,
  kFlipDelayMs,
  kFlipButtons,
  kFlipJoystick,
  kOledOsc,
  kOledDiv,
  kOledMux,
  kOledPrecharge1,
  kOledPrecharge2,
  kOledContrast,

  // Dev-tuning settings (remove for production)
  kImuSmoothing,
  kImuInt1Threshold,
  kImuInt1Duration,
  kBtnDebounceMs,
  kRptInitialDelayMs,
  kRptFirstIntervalMs,
  kRptSecondDelayMs,
  kRptSecondIntervalMs,
  kLedServiceMs,
  kOledRefreshMs,
  kJoyPollMs,
  kSchHighDiv,
  kSchNormDiv,
  kSchLowDiv,
  kLoopDelayMs,
  kCpuIdleMhz,
  kCpuActiveMhz,
  kWifiCheckMs,

  kBoopIrInfo,
  kBoopInfoFields,
  kIrTxPowerPct,

  kNotifyIrEnable,

  // Confirm/cancel grammar. 0 = Xbox-style A confirm / B cancel.
  // 1 = Nintendo-style B confirm / A cancel (default).
  kSwapConfirmCancel,

  // Serial log gates — controlled via `DebugLog.h` macros
  // (LOG_IR / LOG_BOOP / LOG_NOTIFY / LOG_ZIGMOJI / LOG_IMU).
  // Defaults are OFF so the
  // serial console is quiet under normal use; flip any category
  // to 1 in settings.txt to re-enable its per-event spam while
  // debugging. Boot / init / error logs stay unconditional.
  kLogIr,       // BadgeIR frame-level events
  kLogBoop,     // BadgeBoops + BoopFeedback pairing-protocol events
  kLogNotify,   // Reserved legacy notification/debug category
  kLogZigmoji,  // Reserved legacy zigmoji/debug category
  kLogImu,      // IMU samples, orientation thresholds, flip transitions

  // Header-clock "artificial horizon" effect — when enabled, the centered
  // time pill drifts/tilts with the IMU. Off = static text rendering.
  kHorizonClock,
};

extern const uint8_t kFontFamilyCount;
extern const char* const kFontFamilyNames[];
int8_t fontFamilyFromName(const char* name);

// ---------------------------------------------------------------------------
//  Config — persistent badge settings.
//  Primary storage: human-readable INI file on the FAT filesystem.
//  Fallback: NVS (Preferences) when the filesystem is not yet mounted.
//  On boot: loadFromNvs() gives early defaults, then loadFromFile() is
//  called after MicroPython mounts the FAT partition.  applyAll() pushes
//  values to hardware.  The FileBrowser edits values live and calls
//  saveToFile() on exit.
// ---------------------------------------------------------------------------

class Config {
    public:
     struct SettingDef {
       const char* key;
       const char* label;
       int32_t defaultValue;
       int32_t minValue;
       int32_t maxValue;
       int32_t step;
     };

     static const SettingDef kDefs[];
     static const uint8_t kCount;
     // Editable runtime settings. Not provisioned by StartupFiles; created on
     // first boot and then left as a normal user-editable FatFS file.
     static constexpr const char* kSettingsPath = "settings.txt";
     static constexpr const char* kDefaultTimezone =
         "PST8PDT,M3.2.0,M11.1.0";

     // Headroom for future settings.  MUST stay >= kCount (enforced by
     // static_assert in BadgeConfig.cpp) — a too-small array silently
     // truncates writes from set() / saveToNvs() and OOB-reads in
     // get() / apply() corrupt adjacent Config members (which is what
     // caused the "log_notify shows a huge number and crashes the
     // badge" bug in April 2026).
     static constexpr uint8_t kMaxSettings = 64;

     // ESP32 supports a discrete set of CPU clock frequencies. Settings indices
     // kCpuIdleMhz / kCpuActiveMhz are snapped to one of these values.
     static const int32_t kCpuValidMhz[];
     static const uint8_t kCpuValidMhzCount;

     Config();

     bool loadFromNvs();
     bool saveToNvs();

     bool loadFromFile();
     bool saveToFile();

     bool load();
     bool save();

     int32_t get(uint8_t index) const;
     void set(uint8_t index, int32_t value);

     // Compute the next value for `index` given current value and a direction.
     // Most settings step by `kDefs[index].step * magnitude` (clamped to range);
     // CPU frequency settings step through kCpuValidMhz[] by `magnitude` slots.
     static int32_t nextValue(uint8_t index, int32_t current, int8_t dir,
                              uint8_t magnitude = 1);

     void applyAll();
     void apply(uint8_t index);

     bool checkFileChanged();
     void snapshotFileStat();

     const char* wifiSsid() const { return wifiSsid_; }
     const char* wifiPass() const { return wifiPass_; }
     bool wifiConfigured() const;
     // UI-entered WiFi credentials. Stored under separate NVS keys
     // (`ui_wifi_ssid`/`ui_wifi_pass`) so the legacy-secret cleanup pass
     // doesn't wipe them. When non-empty they take precedence over the
     // build-baked values from `BuildWifiConfig`.
     void setWifiCredentials(const char* ssid, const char* pass);
     // Set the wall clock to today's date (or 2026-01-01 if no date is
     // available yet) at HH:MM:00 local time. Returns true on success.
     bool setManualTime(int hour24, int minute);
     const char* timezone() const { return timezone_; }
     void setTimezone(const char* value);
     void applyTimezone() const;

     /// `[nametag] nametag = default | <8-char hex id> | …/info.json`
     const char* nametagSetting() const { return nametagSetting_; }
     void setNametagSetting(const char* value);
     /// Load/clear `gNametagDoc` from `[nametag] nametag =` in settings.txt.
     void applyNametagSetting();

   private:
    int32_t values_[kMaxSettings];

     static constexpr uint8_t kStringMaxLen = 128;
     char wifiSsid_[kStringMaxLen] = "";
     char wifiPass_[kStringMaxLen] = "";
     char timezone_[64] = "PST8PDT,M3.2.0,M11.1.0";
     char nametagSetting_[64] = "default";

     uint32_t lastFileSize_ = 0;
     uint16_t lastFileDate_ = 0;
     uint16_t lastFileTime_ = 0;

     bool parseSettingsFile(const char* buf, uint16_t len);
     uint16_t formatSettingsFile(char* buf, uint16_t bufSize) const;
     void loadNetworkFromBuild();
     void loadStringsFromNvs();
     void saveStringsToNvs();
     void clearLegacyNetworkFromNvs();
   };

   extern Config badgeConfig;

   // ---------------------------------------------------------------------------
   //  ConfigWatcher — scheduler service that polls settings.txt for changes.
   //  When the file is modified externally (e.g. via USB/MicroPython), the
   //  watcher reloads and applies settings automatically.
   // ---------------------------------------------------------------------------

   class ConfigWatcher : public IService {
    public:
     void begin(Config* config);
     void service() override;
     const char* name() const override;

    private:
     Config* config_ = nullptr;
     uint32_t lastCheckMs_ = 0;
     static constexpr uint32_t kPollIntervalMs = 2000;
   };

   extern ConfigWatcher configWatcher;
