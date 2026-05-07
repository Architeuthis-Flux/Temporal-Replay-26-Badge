#include "Power.h"

#include "../api/WiFiService.h"
#include <Wire.h>

#ifdef BADGE_HAS_SLEEP_SERVICE
#include "IMU.h"
#include "Inputs.h"
#include "LEDmatrix.h"
#include "../BadgeGlobals.h"
#endif
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp32-hal-cpu.h"
#include "esp_bt.h"
#include "esp_sleep.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include "oled.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/usb_serial_jtag_reg.h"

// ═══════════════════════════════════════════════════════════════════════════════
//  Power namespace — radio defaults, loop pacing, CPU governor
// ═══════════════════════════════════════════════════════════════════════════════

namespace Power {

uint16_t Policy::ledServiceIntervalMs = 25;
uint16_t Policy::oledRefreshMs = 100;
uint16_t Policy::joystickPollMs = 20;
uint8_t  Policy::schedulerHighDivisor = 1;
uint8_t  Policy::schedulerNormalDivisor = 4;
uint8_t  Policy::schedulerLowDivisor = 30;
uint16_t Policy::loopDelayMs = 8;
uint32_t Policy::cpuIdleMhz = 80;
uint32_t Policy::cpuActiveMhz = 160;

namespace {
bool gPmActive = false;
uint32_t gLastActivityMs = 0;
uint32_t gLastCpuSwitchMs = 0;
uint32_t gCurrentCpuMhz = 0;
uint8_t gPerformanceDepth = 0;

#if defined(CHG_GOOD_PIN) && defined(CHG_STAT_PIN)
bool gChargerTelemetryPinsReady = false;
uint32_t gLastChargerTelemetryLogMs = 0;
#endif

bool configurePm(uint32_t minMhz, uint32_t maxMhz, bool lightSleep) {
  if (!gPmActive) return false;
  esp_pm_config_t pm = {};
  pm.max_freq_mhz = maxMhz;
  pm.min_freq_mhz = minMhz;
  pm.light_sleep_enable = lightSleep;
  const esp_err_t err = esp_pm_configure(&pm);
  if (err != ESP_OK) {
   //Serial.printf("PM: configure failed (0x%x)\n", err);
    return false;
  }
  return true;
}
}  // namespace

#if defined(CHG_GOOD_PIN) && defined(CHG_STAT_PIN)
void initChargerTelemetryPins() {
  if (gChargerTelemetryPinsReady) return;
  pinMode(CHG_GOOD_PIN, INPUT_PULLUP);
  pinMode(CHG_STAT_PIN, INPUT_PULLUP);
  gChargerTelemetryPinsReady = true;
}

ChargerTelemetry readChargerTelemetry() {
  initChargerTelemetryPins();

  ChargerTelemetry telemetry;
  telemetry.available = true;
  telemetry.pgoodLevel = digitalRead(CHG_GOOD_PIN) == LOW ? LOW : HIGH;
  telemetry.statLevel = digitalRead(CHG_STAT_PIN) == LOW ? LOW : HIGH;

  // BQ24079 status outputs are active-low/open-drain:
  // PGOOD low means external power is present; STAT low means charging.
  telemetry.externalPowerPresent = telemetry.pgoodLevel == LOW;
  telemetry.charging = telemetry.externalPowerPresent && telemetry.statLevel == LOW;
  if (!telemetry.externalPowerPresent) {
    telemetry.state = "battery";
  } else if (telemetry.charging) {
    telemetry.state = "charging";
  } else {
    telemetry.state = "external";
  }
  return telemetry;
}

void logChargerTelemetry(const ChargerTelemetry& telemetry) {
  if (!telemetry.available) {
    ////Serial.println("Power: charger telemetry unavailable");
    return;
  }
  // Serial.printf("Power: charger pgood=%u stat=%u external=%d charging=%d state=%s\n",
  //               (unsigned)telemetry.pgoodLevel,
  //               (unsigned)telemetry.statLevel,
  //               telemetry.externalPowerPresent ? 1 : 0,
  //               telemetry.charging ? 1 : 0,
  //               telemetry.state);
}

void logChargerTelemetryIfDue(uint32_t intervalMs) {
  const uint32_t now = millis();
  if (gLastChargerTelemetryLogMs != 0 &&
      now - gLastChargerTelemetryLogMs < intervalMs) {
    return;
  }
  gLastChargerTelemetryLogMs = now;
  logChargerTelemetry(readChargerTelemetry());
}
#endif

void applyBootRadioDefaults() {
  // WiFi stays available for explicit MicroPython networking helpers only.
  // BLE controller memory is INTENTIONALLY NOT released — IDF's
  // esp_bt_mem_release(BLE) permanently relinquishes the controller's
  // static .bss/.data regions to the heap, after which any later
  // esp_bt_controller_init() fails inside btdm_controller_init and
  // the cleanup path NULL-derefs in semphr_delete_wrapper. Keeping
  // BLE reserved costs ~30 KB internal DRAM but is the only way the
  // venue proximity scanner can come up on demand. Releasing only
  // the classic-BT footprint reclaims unused BR/EDR memory safely.
  esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT);
}

void applyLoopPacing() {
  delay(Policy::loopDelayMs);
}

void enterPerformanceMode() {
  noteActivity();
  if (gPerformanceDepth < 255) {
    gPerformanceDepth++;
  }
  if (gPerformanceDepth != 1) return;

  if (configurePm(240, 240, false)) {
   //Serial.println("PM: performance mode enabled (240 MHz, no light sleep)");
  }
}

void exitPerformanceMode() {
  if (gPerformanceDepth == 0) return;
  gPerformanceDepth--;
  if (gPerformanceDepth != 0) return;

  if (configurePm(Policy::cpuIdleMhz, Policy::cpuActiveMhz, true)) {
    //Serial.printf("PM: restored policy (%lu-%lu MHz, light sleep enabled)\n",
                  // (unsigned long)Policy::cpuIdleMhz,
                  // (unsigned long)Policy::cpuActiveMhz);
  }
  noteActivity();
}

void initCpuGovernor() {
  gLastActivityMs = millis();
  gLastCpuSwitchMs = 0;
  gCurrentCpuMhz = getCpuFrequencyMhz();

  // Prefer ESP-IDF power management with automatic light sleep.
  // Requires CONFIG_PM_ENABLE + CONFIG_FREERTOS_USE_TICKLESS_IDLE in the IDF
  // build; when they're missing, esp_pm_configure() returns
  // ESP_ERR_NOT_SUPPORTED and we must NOT fall through to manual CPU
  // scaling: setCpuFrequencyMhz() reprograms the APB clock, which can
  // corrupt in-flight I2C transactions on the new IDF 5.x driver (see
  // esp-idf#15734, arduino-esp32#11374). Pin the CPU at whatever f_cpu
  // selected at boot instead.
  esp_pm_config_t pm = {};
  pm.max_freq_mhz = Policy::cpuActiveMhz;
  pm.min_freq_mhz = Policy::cpuIdleMhz;
  pm.light_sleep_enable = true;
  esp_err_t err = esp_pm_configure(&pm);
  if (err == ESP_OK) {
    gPmActive = true;
   //Serial.printf("PM: auto light sleep enabled (%lu–%lu MHz)\n",
                  // (unsigned long)Policy::cpuIdleMhz,
                  // (unsigned long)Policy::cpuActiveMhz);
    return;
  }
 //Serial.printf("PM: unavailable (0x%x) — CPU pinned at %lu MHz\n",
                // err, (unsigned long)gCurrentCpuMhz);
}

void noteActivity() {
  gLastActivityMs = millis();
}

void updateCpuGovernor() {
  // No-op: either ESP-IDF PM is driving the frequency (when CONFIG_PM_ENABLE
  // is set), or we've chosen to leave the CPU at its boot frequency. Manual
  // setCpuFrequencyMhz() toggling has been removed because it reprograms the
  // APB clock and triggers ESP_ERR_INVALID_STATE on the I2C master driver
  // when a transaction is in flight.
}

// ── BrownoutSilencer ──────────────────────────────────────────────
// Save the master enable + interrupt enable + reset enable bits of
// RTC_CNTL_BROWN_OUT_REG and clear them; restore on destruct. Three
// bits, not the whole register — writing 0 to the entire register
// also zeroes the trip-level / wait / select fields, which on this
// SoC we observed leaves the detector in a state where the
// interrupt still fires immediately on the first WiFi TX burst.
// The IDF's brownout_hal_intr_enable(false) only touches INT_ENA;
// we additionally clear RST_ENA (no reset) and ENA (analog
// comparator off) for the duration of the silenced window.
//
// Also stamps the SPI-flash brownout-reset gate
// (RTC_CNTL_BROWN_OUT_CLOSE_FLASH_ENA) — the flash brownout reset
// is a separate path that bypasses the BOD ISR entirely, and was
// the residual offender in the field.
BrownoutSilencer::BrownoutSilencer() {
  saved_ = REG_READ(RTC_CNTL_BROWN_OUT_REG);
  REG_CLR_BIT(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_INT_ENA);
  REG_CLR_BIT(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_RST_ENA);
  REG_CLR_BIT(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_CLOSE_FLASH_ENA);
  REG_CLR_BIT(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_ENA);
  ////Serial.printf("[BOD] silenced (was 0x%08x, now 0x%08x)\n",
  //               static_cast<unsigned>(saved_),
  //               static_cast<unsigned>(REG_READ(RTC_CNTL_BROWN_OUT_REG)));
}

BrownoutSilencer::~BrownoutSilencer() {
  // Restore the exact prior register state — including any wait /
  // select fields the framework configured at boot.
  REG_WRITE(RTC_CNTL_BROWN_OUT_REG, saved_);
  ////Serial.println("[BOD] restored");
}

#if !(defined(BADGE_HAS_BATTERY_GAUGE) && defined(BADGE_ECHO) && defined(BADGE_HAS_SLEEP_SERVICE))
// Non-echo builds (or echo without sleep service) don't have the BQ24079
// brownout problem; the gate is unconditionally open.
bool wifiAllowed() { return true; }
#endif

}  // namespace Power

#ifdef BADGE_HAS_SLEEP_SERVICE

// ═══════════════════════════════════════════════════════════════════════════════
//  SleepService — light/deep sleep driven by IMU motion events
// ═══════════════════════════════════════════════════════════════════════════════

namespace {
bool isLineStableHigh(uint8_t pin, uint8_t samples = 6, uint8_t sampleDelayMs = 2) {
  for (uint8_t i = 0; i < samples; ++i) {
    if (digitalRead(pin) == LOW) {
      return false;
    }
    delay(sampleDelayMs);
  }
  return true;
}

// USB host sends SOF packets every 1ms, incrementing this counter.
// If the counter changed since last check, a USB host is connected.
bool isUsbConnected() {
  static uint32_t prevFrame = UINT32_MAX;
  uint32_t frame = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & 0x7FFU;
  bool connected = (frame != prevFrame);
  prevFrame = frame;
  return connected;
}
}  // namespace

void SleepService::bindInputs(Inputs* inputs) { inputs_ = inputs; }

void SleepService::begin(IMU* imu, LEDmatrix* matrix, oled* display, uint8_t wakePin,
                         uint32_t lightSleepAfterNoMotionMs, uint32_t deepSleepAfterNoMotionMs) {
  imu_ = imu;
  matrix_ = matrix;
  display_ = display;
  wakePin_ = wakePin;
  lightSleepAfterNoMotionMs_ = lightSleepAfterNoMotionMs;
  deepSleepAfterNoMotionMs_ = deepSleepAfterNoMotionMs;
  lastMotionMs_ = millis();
  caffeine = false;

  pinMode(wakePin_, INPUT_PULLUP);
 //Serial.printf("Wake cause: %d\n", esp_sleep_get_wakeup_cause());

  enabled_ = (imu_ != nullptr && imu_->isReady());
  if (!enabled_) {
   //Serial.println("SleepService disabled (no IMU)");
  }
}

void SleepService::service() {
  if (!enabled_) return;

  if (imu_->consumeMotionEvent()) {
    caffeine = true;
  }

  // Force deep sleep: hold UP for 5 seconds (skip if USB connected for dev safety)
  if (inputs_ && inputs_->heldMs(0) >= Power::Policy::kForceDeepSleepHoldMs
      && !isUsbConnected()) {
   //Serial.println("Force deep sleep (UP held 5s)");
    enterDeepSleep();
    return;
  }

  bool anyButton = inputs_ &&
      (inputs_->buttons().up || inputs_->buttons().down ||
       inputs_->buttons().left || inputs_->buttons().right);


  if (caffeine || anyButton || Serial || isUsbConnected()) {
    caffeine = false;
    lastMotionMs_ = millis();
    if (displayDimmed_) {
      if (matrix_) {
        matrix_->setBrightness(savedBrightness_);
      }
      displayDimmed_ = false;
    }
    return;
  }

  const uint32_t idleMs = millis() - lastMotionMs_;

  // Unpaired badges (QR screen) deep sleep much sooner to save battery
  // on flashed badges waiting to be handed out.
  const uint32_t effectiveDeepSleepMs =
      (badgeState == BADGE_UNPAIRED) ? Power::Policy::kUnpairedDeepSleepMs
                                    : deepSleepAfterNoMotionMs_;

  if (idleMs >= effectiveDeepSleepMs) {
    enterDeepSleep();
    return;
  }


  // Dim LED matrix after idle timeout; ESP PM auto-light-sleep handles
  // CPU sleep while WiFi modem sleep keeps the connection alive.
  if (idleMs >= lightSleepAfterNoMotionMs_ && !displayDimmed_) {
   //Serial.printf("Dimming LED matrix (%lu ms idle)\n", (unsigned long)idleMs);
    if (matrix_) {
      savedBrightness_ = matrix_->getBrightness();
      matrix_->setBrightness(Power::Policy::kLedMatrixDimBrightness);
    }
    displayDimmed_ = true;
  }
}

const char* SleepService::name() const { return "SleepService"; }


void SleepService::processDeferredWake() {}

void SleepService::preparePeripheralsForDeepSleep() {
  if (matrix_ != nullptr) {
    matrix_->blankHardware();
  }
  if (display_ != nullptr) {
    display_->clearDisplay();
    display_->display();
    display_->setPowerSave(true);
    // delay(2);
  }
}

void SleepService::enterDeepSleep() {
 //Serial.printf("Deep sleep idle reached (%lu ms)\n", (unsigned long)(millis() - lastMotionMs_));

  preparePeripheralsForDeepSleep();

  if (!isLineStableHigh(wakePin_)) {
   //Serial.printf("Deep sleep pre-arm warning; wake line GPIO %u not stably high, continuing\n", wakePin_);
  }

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

  rtc_gpio_pullup_en((gpio_num_t)wakePin_);
  rtc_gpio_pulldown_dis((gpio_num_t)wakePin_);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

  const esp_err_t wakeCfg = esp_sleep_enable_ext0_wakeup((gpio_num_t)wakePin_, 0);
  if (wakeCfg != ESP_OK) {
   //Serial.printf("Wake config failed on GPIO %u, err=%d\n", wakePin_, wakeCfg);
    return;
  }

  if (!isLineStableHigh(wakePin_, 4, 1)) {
   //Serial.printf("Deep sleep arm warning; GPIO %u dipped low during arm, continuing\n", wakePin_);
  }

  if (display_ != nullptr) {
    display_->clearDisplay();
    display_->display();
    display_->setPowerSave(true);
    delay(20);
  }

 //Serial.printf("Sleeping; wake on GPIO %u LOW\n", wakePin_);
 //Serial.flush();
  esp_deep_sleep_start();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  BatteryGauge
// ═══════════════════════════════════════════════════════════════════════════════

// #if defined(BADGE_HAS_BATTERY_GAUGE) && defined(BADGE_ECHO) 
// ── ADC voltage-divider gauge — main-CPU adapter for BatteryAlgorithm ───────
//
// All math (median, IIR, presence debounce, probe classification)
// lives in firmware/src/hardware/battery/BatteryAlgorithm.h and is shared
// with a future ULP-RISC-V port. This file owns only the four HAL primitives:
//   - ADC reads via analogRead(BATT_VOLTAGE_PIN)
//   - CE drive via digitalWrite(CE_PIN, ...)
//   - PGOOD / STAT reads via digitalRead
//   - micros() deadlines (no blocking delays in service())

bool BatteryGauge::begin() {
  analogSetAttenuation(ADC_11db);
  pinMode(BATT_VOLTAGE_PIN, INPUT);

  // CE drives the BQ24079 enable line during the active presence probe.
  // Idle low = charger enabled.
  pinMode(CE_PIN, OUTPUT);
  digitalWrite(CE_PIN, LOW);

#if defined(CHG_GOOD_PIN) && defined(CHG_STAT_PIN)
  Power::initChargerTelemetryPins();
#endif

  bat_presence_init(&presence_);
  bat_filter_zero(&filter_);
  flags_ = 0;
  probeTickCounter_ = 0;
  probePhase_ = kProbeIdle;
  probeSampleIdx_ = 0;

  ready_ = true;
  service();
  return true;
}

namespace {

// Inter-sample gap and BQ resettle window — were inline delays inside the
// previous blocking runProbe(). The phase machine in service() arms
// micros() deadlines from these.
static constexpr uint32_t kProbeInterSampleUs = BAT_PROBE_WINDOW_US / (BAT_PROBE_SAMPLES - 1);
static constexpr uint32_t kProbeResettleUs    = 2000;

// HAL: take one ADC read per service tick and expose a rolling BAT_SAMPLE_N
// window for median-of-N filtering. This keeps BatteryGauge::service()
// cooperative; adc_spread below reflects recent-window drift/noise rather
// than an instantaneous burst.
void collectMedianWindow(uint32_t* samples) {
  static uint32_t history[BAT_SAMPLE_N];
  static uint8_t  next_index   = 0;
  static bool     initialized  = false;

  const uint32_t sample = (uint32_t)analogRead(BATT_VOLTAGE_PIN);

  if (!initialized) {
    for (uint8_t i = 0; i < BAT_SAMPLE_N; i++) {
      history[i] = sample;
    }
    initialized = true;
    next_index  = 1 % BAT_SAMPLE_N;
  } else {
    history[next_index] = sample;
    next_index = (uint8_t)((next_index + 1) % BAT_SAMPLE_N);
  }

  for (uint8_t i = 0; i < BAT_SAMPLE_N; i++) {
    samples[i] = history[(next_index + i) % BAT_SAMPLE_N];
  }
}

}  // namespace

void BatteryGauge::service() {
  // ── Read charger status pins ──
  const bool pgood    = digitalRead(CHG_GOOD_PIN) == LOW;
  const bool stat_raw = digitalRead(CHG_STAT_PIN) == LOW;
  const bool stat_low = pgood && stat_raw;

  // ── Active probe schedule (only meaningful with USB present) ──
  // Phase machine driven by micros() deadlines. service() takes at most
  // one ADC read per call. A verdict is delivered ONLY on the tick that
  // drains the resettle phase; every other tick during the probe runs
  // with verdict=-1, have_probe_sample=false, which is identical to
  // "no probe scheduled this tick" — the presence/filter pipeline below
  // already handles that case correctly.
  int8_t   probe_verdict     = -1;
  uint32_t probe_sample      = 0;
  bool     have_probe_sample = false;

  if (!pgood) {
    // USB removed — abort any in-flight probe so we re-arm cleanly on
    // the next pgood edge. Make sure CE is back low (charger enabled).
    if (probePhase_ != kProbeIdle) {
      digitalWrite(CE_PIN, LOW);
      probePhase_     = kProbeIdle;
      probeSampleIdx_ = 0;
    }
    probeTickCounter_ = 0;
  } else {
    const uint32_t now_us = micros();
    switch (probePhase_) {
      case kProbeIdle:
        if (probeTickCounter_ == 0) {
          // Kick off a fresh probe: drive CE high, take sample 0
          // immediately, schedule sample 1.
          digitalWrite(CE_PIN, HIGH);
          probeSamples_[0]     = (uint32_t)analogRead(BATT_VOLTAGE_PIN);
          probeSampleIdx_      = 1;
          probeNextSampleAtUs_ = now_us + kProbeInterSampleUs;
          probePhase_          = kProbeSampling;
        }
        // Counter advances only on idle-visit ticks, so the probe period
        // is measured between probe *completions*, not starts. Net effect:
        // the BQ24079 charges more (lower CE-high duty cycle) than the
        // original synchronous design.
        probeTickCounter_ = (probeTickCounter_ + 1) % BAT_PROBE_PERIOD_TICKS;
        break;

      case kProbeSampling:
        // Signed compare against the deadline so micros() wraparound
        // (~71 minutes) doesn't strand us mid-probe.
        if ((int32_t)(now_us - probeNextSampleAtUs_) >= 0) {
          probeSamples_[probeSampleIdx_] = (uint32_t)analogRead(BATT_VOLTAGE_PIN);
          probeSampleIdx_++;
          if (probeSampleIdx_ >= BAT_PROBE_SAMPLES) {
            // Done sampling — release CE and start the resettle window.
            digitalWrite(CE_PIN, LOW);
            probeReadyAtUs_ = now_us + kProbeResettleUs;
            probePhase_     = kProbeResettle;
          } else {
            probeNextSampleAtUs_ = now_us + kProbeInterSampleUs;
          }
        }
        break;

      case kProbeResettle:
        if ((int32_t)(now_us - probeReadyAtUs_) >= 0) {
          // Probe complete — classify and feed the rest of the pipeline
          // exactly as the old synchronous path did.
          probe_verdict     = bat_classify_probe(probeSamples_,
                                                 BAT_PROBE_SAMPLES,
                                                 &probe_sample);
          have_probe_sample = true;
          probePhase_       = kProbeIdle;
          probeSampleIdx_   = 0;
        }
        break;
    }
  }

  // ── Sampling path ──
  uint32_t raw        = 0;
  uint32_t adc_spread = 0;
  bool     have_raw   = false;

  if (!pgood) {
    // BQ passes the cell straight through; analogRead IS cell voltage.
    uint32_t samples[BAT_SAMPLE_N];
    collectMedianWindow(samples);
    bat_sort_small(samples, BAT_SAMPLE_N);
    raw        = bat_median_pair(samples, BAT_SAMPLE_N);
    adc_spread = samples[BAT_SAMPLE_N - 1] - samples[0];
    have_raw   = true;
  } else if (have_probe_sample) {
    // BQ regulates VBAT to non-cell values between probes — only the probe
    // window is trustworthy. Use its median as the raw reading for this tick.
    raw        = probe_sample;
    have_raw   = true;
    adc_spread = 0;
  }

  // ── Presence state machine ──
  bat_presence_inputs_t in;
  in.pgood         = pgood;
  in.stat_raw      = stat_raw;
  in.have_raw      = have_raw;
  in.raw           = raw;
  in.adc_spread    = adc_spread;
  in.probe_verdict = probe_verdict;

  bat_presence_outputs_t out;
  bat_presence_update(&presence_, &in, &out);

  // ── Edge-triggered reset on present->absent ──
  // Without this the host would see stale "last known-good" raw/filtered/pct
  // long after a battery has been removed, with only the bat_present flag
  // indicating they're meaningless.
  if (out.edge_present_to_absent) {
    bat_filter_zero(&filter_);
  }

  // ── IIR + pct update (only on trustworthy samples) ──
  if (bat_sample_trustworthy(pgood,
                             have_raw,
                             have_probe_sample,
                             out.target_present,
                             probe_verdict)) {
    bat_filter_update(&filter_, raw);
    updateAdcStats(raw);

    uint32_t raw_pct = filter_.bat_pct;
    if (pgood && /* ce enabled */ raw_pct < last_reported_pct_) {
        filter_.bat_pct = last_reported_pct_;          // charging: don't drop
    } else if (!pgood && raw_pct > last_reported_pct_) {
        filter_.bat_pct = last_reported_pct_;          // discharging: don't rise
    }
    last_reported_pct_ = filter_.bat_pct;
  }

  flags_ = bat_compose_flags(pgood, stat_raw, stat_low, out.latched);

  // ── Threshold classification + WiFi power gate ──
  // Power owns the radio kill: when the cell drops to SUB and USB is absent,
  // disconnect WiFi to keep the rail above BOD on the next association burst.
  // When USB shows up after a SUB shutdown, re-arm WiFi so the next service
  // tick reconnects without a manual nudge.
  battery_threshold_t next = bat_classify_threshold(filter_.bat_pct);
  bool entering_sub_no_usb = (cached_threshold_ != BAT_THRESH_SUB &&
                              next == BAT_THRESH_SUB && !pgood);
  bool leaving_sub = (cached_threshold_ == BAT_THRESH_SUB &&
                      (next != BAT_THRESH_SUB || pgood));
  if (entering_sub_no_usb) {
    ////Serial.println("[Power] battery SUB and no USB — disconnecting WiFi");
    // wifiService.disconnect();
  } else if (leaving_sub) {
    ////Serial.println("[Power] battery recovered or USB present — re-arming WiFi");
    // wifiService.armForReconnect();
  }
  cached_threshold_ = next;

// #if defined(BADGE_POWER_TELEMETRY)
  logAdcTelemetryIfDue();
// #if defined(CHG_GOOD_PIN) && defined(CHG_STAT_PIN)
  Power::logChargerTelemetryIfDue();
// #endif
// #endif
}

void BatteryGauge::updateAdcStats(uint32_t raw) {
  if (sampleCount_ == 0) {
    rawAdcMin_ = raw;
    rawAdcMax_ = raw;
  } else {
    if (raw < rawAdcMin_) rawAdcMin_ = raw;
    if (raw > rawAdcMax_) rawAdcMax_ = raw;
  }
  if (sampleCount_ < UINT32_MAX) {
    sampleCount_++;
  }
}

void BatteryGauge::logAdcTelemetryIfDue(uint32_t intervalMs) {
  const uint32_t now = millis();
  if (lastAdcTelemetryLogMs_ != 0 &&
      now - lastAdcTelemetryLogMs_ < intervalMs) {
    return;
  }
  lastAdcTelemetryLogMs_ = now;
  const uint32_t raw_min = (sampleCount_ == 0) ? 0 : rawAdcMin_;
  const uint32_t raw_max = (sampleCount_ == 0) ? 0 : rawAdcMax_;
  ////Serial.printf("Power: battery raw=%lu filtered=%lu mv=%lu pct=%lu present=%d pgood=%d stat_raw=%d stat_low=%d raw_min=%lu raw_max=%lu samples=%lu\n",
  //               (unsigned long)filter_.adc_raw,
  //               (unsigned long)(filter_.filtered_raw >> BAT_IIR_SHIFT),
  //               (unsigned long)filter_.bat_mv,
  //               (unsigned long)filter_.bat_pct,
  //               batteryPresent() ? 1 : 0,
  //               (flags_ & MASK_PGOOD) ? 1 : 0,
  //               (flags_ & MASK_STAT_RAW) ? 1 : 0,
  //               (flags_ & MASK_STAT_LOW) ? 1 : 0,
  //               (unsigned long)raw_min,
  //               (unsigned long)raw_max,
  //               (unsigned long)sampleCount_);
}

const char* BatteryGauge::name() const { return "BatteryGauge"; }

battery_threshold_t BatteryGauge::threshold() {
  // Post-ready: the service tick already updated cached_threshold_.
  if (ready_) return cached_threshold_;

  // Pre-ready bootup path: no IIR, no probe history. Take BAT_SAMPLE_N raw
  // ADC reads in a tight loop, classify directly off the median. Speed
  // first; the only goal is "is the cell so low we'll brown out the rail
  // when the radio kicks on".
  analogSetAttenuation(ADC_11db);
  pinMode(BATT_VOLTAGE_PIN, INPUT);
  uint32_t samples[BAT_SAMPLE_N];
  for (uint8_t i = 0; i < BAT_SAMPLE_N; i++) {
    samples[i] = (uint32_t)analogRead(BATT_VOLTAGE_PIN);
  }
  return bat_bootup_threshold(samples, BAT_SAMPLE_N);
}

extern BatteryGauge batteryGauge;

namespace Power {
bool wifiAllowed() {
  // USB-present always wins — BQ24079 holds the 3V3 rail up regardless of
  // SOC, so the brownout-on-association concern goes away.
  initChargerTelemetryPins();
  if (digitalRead(CHG_GOOD_PIN) == LOW) return true;  // active-low pgood
  return batteryGauge.threshold() != BAT_THRESH_SUB;
}
}  // namespace Power

#elif defined(BADGE_HAS_BATTERY_GAUGE)
// ── MAX17048 I2C fuel gauge ──────────────────────────────────────────────────

bool BatteryGauge::begin() {
  ready_ = gauge_.begin(&Wire);
  if (!ready_) {
    socPercent_ = NAN;
    cellVoltage_ = NAN;
    return false;
  }

  gauge_.setResetVoltage(kBatteryVReset);
  gauge_.setAlertVoltages(kBatteryVEmpty, kBatteryVFull);
  gauge_.quickStart();

  delay(10);

  service();
  return true;
}

void BatteryGauge::service() {
  socPercent_ = gauge_.cellPercent();
  cellVoltage_ = gauge_.cellVoltage();
}

const char* BatteryGauge::name() const { return "BatteryGauge"; }
#endif  // BADGE_HAS_BATTERY_GAUGE
// #endif  // BADGE_HAS_SLEEP_SERVICE
