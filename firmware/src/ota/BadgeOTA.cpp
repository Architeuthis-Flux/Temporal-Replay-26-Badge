#include "BadgeOTA.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_flash.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_heap_caps.h>

// We need to write into the partition-table sector at 0x8000, which
// arduino-esp32 (default sdkconfig: CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS=1)
// treats as a fatal programmer error in esp_flash_erase_region /
// esp_flash_write. The first attempt — routing through
// bootloader_flash_erase_range / bootloader_flash_write — also
// panics, because in app context those are literally single-line
// tail calls to esp_flash_* (confirmed by disassembling
// libbootloader_support.a / bootloader_flash.c.obj). The
// IDF-supplied runtime escape hatch is
// esp_flash_set_dangerous_write_protection(chip, false), declared
// in esp_flash_internal.h. We re-enable the protection after the
// write window closes (or before rollback completes) so unrelated
// future bugs still trip the abort and we don't leave the rest of
// the system in a permanently-unguarded state.
extern "C" {
esp_err_t bootloader_flash_read(size_t src_addr, void* dest, size_t size,
                                bool allow_decrypt);
esp_err_t bootloader_flash_write(size_t dest_addr, void* src, size_t size,
                                 bool write_encrypted);
esp_err_t bootloader_flash_erase_range(uint32_t start_addr, uint32_t size);
esp_err_t esp_flash_set_dangerous_write_protection(esp_flash_t* chip,
                                                   const bool protect);
}

// Embedded `_ver2` partition table blob. Generated at build time by
// scripts/gen_expanded_partitions.py and folded into the link via
// `board_build.embed_files` (see platformio.ini). The symbol short
// names are produced by scripts/normalize_bundle_symbols.py.
extern "C" {
extern const uint8_t kPartitionTableVer2Start[]
    asm("_binary_partitions_ver2_bin_start");
extern const uint8_t kPartitionTableVer2End[]
    asm("_binary_partitions_ver2_bin_end");
}

#include "AssetRegistry.h"
#include "OTAHttp.h"
#include "../api/WiFiService.h"
#include "../hardware/Power.h"
#include "../infra/DebugLog.h"
#include "../infra/PsramAllocator.h"
#include "../identity/BadgeVersion.h"

#if __has_include(<micropython_embed.h>)
extern "C" void mpy_collect(void);
#define BADGEOTA_HAS_MPY_COLLECT 1
#else
#define BADGEOTA_HAS_MPY_COLLECT 0
#endif

extern "C" {
#include "lib/oofatfs/ff.h"
FATFS* replay_get_fatfs(void);
int replay_bdev_reformat_and_reboot(void);
}

extern BatteryGauge batteryGauge;

namespace {

void logOtaCheckHeap(const char* phase) {
  // Match Arduino ESP.getFreeHeap() / getMinFreeHeap() / getMaxAllocHeap()
  // (internal DRAM only). Do not use esp_get_minimum_free_heap_size() here —
  // it uses MALLOC_CAP_DEFAULT and can include SPIRAM, so it misleadingly
  // stays ~MB while internal free is ~100 KiB.
  const uint32_t intFree = static_cast<uint32_t>(ESP.getFreeHeap());
  const uint32_t intMinEver = static_cast<uint32_t>(ESP.getMinFreeHeap());
  const uint32_t intLargest = static_cast<uint32_t>(ESP.getMaxAllocHeap());
  const size_t psramTotal =
      heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  uint32_t psramFree = 0;
  uint32_t psramLargest = 0;
  if (psramTotal > 0) {
    psramFree = static_cast<uint32_t>(
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    psramLargest = static_cast<uint32_t>(
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  Serial.printf(
      "[ota-check] %s: int_free=%u int_min_ever=%u int_largest=%u "
      "psram_total=%u psram_free=%u psram_largest=%u\n",
      phase ? phase : "?", intFree, intMinEver, intLargest,
      static_cast<unsigned>(psramTotal), psramFree, psramLargest);
}

uint8_t* otaInstallChunkScratch() {
  static uint8_t* p = nullptr;
  if (!p) {
    p = static_cast<uint8_t*>(BadgeMemory::allocPreferPsram(4096));
  }
  return p;
}

}  // namespace

namespace ota {

void prepareFirmwareInstallHeap();

namespace {

constexpr const char* kNvsNamespace = "badge_ota";
constexpr const char* kNvsLastEpoch = "last_epoch";
constexpr const char* kNvsLatestTag = "latest_tag";
constexpr const char* kNvsAssetUrl  = "asset_url";
constexpr const char* kNvsAssetSize = "asset_size";
constexpr const char* kNvsLastLayout = "last_layout";  // "doom" | "ver2"

constexpr size_t kTagMax = 32;
constexpr size_t kUrlMax = 1536;

// Partition table bases for the 16 MB layouts (see OTA-MAINTAINER.md).
// `_doom`: ffat @ 0x7D0000  (6.0 MB)
// `_ver2`: ffat @ 0x910000  (6.875 MB, opt-in)
constexpr uint32_t kFfatExpandedPartitionBase = 0x910000u;

// Flash offset of the partition table sector. ESP-IDF reserves a
// single 4 KB sector here for the table itself; bootloader / nvs /
// app0 / etc are addressed via the entries inside it.
constexpr uint32_t kPartitionTableFlashOffset = 0x8000u;

// Both `_doom` and `_ver2` place app0 at the same physical offset.
// This is the safety pre-condition for the in-place migration: as
// long as we're running from app0 at 0x10000 when we swap tables,
// the bootloader will find the same running image under the new
// layout. app1 offsets differ between the two layouts (0x3F0000
// vs 0x490000), so migrating from app1 would mean booting into an
// unprogrammed region of flash.
constexpr uint32_t kApp0FlashOffset = 0x10000u;

// Migration requires this fraction of charge unless the badge is on
// USB power. Higher than the OTA install threshold (kMinBatteryPct
// = 30) because a power cut during the partition write window is
// the only way to actually brick the badge.
constexpr float kMigrationMinBatteryPct = 50.0f;

char sLatestTag[kTagMax] = "";
char sAssetUrl[kUrlMax] = "";
char sResolvedAssetUrl[kUrlMax] = "";

// ── Migration-reboot announce signal ──────────────────────────────────────
//
// RTC noinit memory survives a software reset (ESP.restart, panic,
// task WDT) but is *cleared* on a power-on / brownout / RTC reset.
// That's exactly the lifetime we want for "tell the user about THIS
// specific reboot": if the badge dies mid-migration and the user
// power-cycles to recover, we don't want the next boot to claim the
// migration succeeded.
//
// Set to `kMagic` right before ESP.restart() in
// migrateToExpandedLayout(). Re-read in begin() and cross-check
// against `esp_reset_reason() == ESP_RST_SW` — the magic + soft
// reset combo is a deterministic "we just rebooted because of OUR
// partition swap" signal that's invariant under unrelated NVS
// editing or USB reflashing.
//
// The magic itself is a fixed 32-bit value chosen so that an
// uninitialised RTC slow region (typically 0x00000000 or
// 0xFFFFFFFF on a fresh power-on) does not alias it.
constexpr uint32_t kMigrationBootMagic = 0xCAFEFEEDu;
RTC_NOINIT_ATTR uint32_t sMigrationBootMagic;
bool sJustMigratedReboot = false;
bool sMigrationBootAcknowledged = false;
size_t sAssetSize = 0;
time_t sLastCheckEpoch = 0;
char sLastError[80] = "";
bool sBegun = false;
bool sPendingVerify = false;
bool sValidated = false;
bool sLayoutChanged = false;
bool sLayoutAcknowledged = false;

void setError(const char* msg) {
  if (!msg) msg = "";
  std::strncpy(sLastError, msg, sizeof(sLastError) - 1);
  sLastError[sizeof(sLastError) - 1] = '\0';
}

void persistCache() {
  Preferences p;
  if (!p.begin(kNvsNamespace, false)) return;
  p.putString(kNvsLatestTag, sLatestTag);
  p.putString(kNvsAssetUrl, sAssetUrl);
  p.putULong(kNvsAssetSize, static_cast<uint32_t>(sAssetSize));
  p.putULong(kNvsLastEpoch, static_cast<uint32_t>(sLastCheckEpoch));
  p.end();
}

void loadCache() {
  Preferences p;
  if (!p.begin(kNvsNamespace, true)) return;
  p.getString(kNvsLatestTag, sLatestTag, sizeof(sLatestTag));
  p.getString(kNvsAssetUrl, sAssetUrl, sizeof(sAssetUrl));
  sAssetSize = p.getULong(kNvsAssetSize, 0);
  sLastCheckEpoch = static_cast<time_t>(p.getULong(kNvsLastEpoch, 0));
  p.end();
}

// Strip a leading 'v' / 'V' from a tag string. Many GitHub releases
// tag as "v1.2.3" — semver compare wants the numeric part.
const char* stripV(const char* s) {
  if (!s) return "";
  if (*s == 'v' || *s == 'V') return s + 1;
  return s;
}

// Parse up to three dotted numeric components (a.b.c). Missing
// components default to 0. Trailing pre-release tags ("-rc1") are
// ignored — they sort earlier than the same numeric version, but
// we treat them as equal for "newer?" purposes (close enough for a
// conference badge OTA).
void parseSemver(const char* s, int* a, int* b, int* c) {
  *a = *b = *c = 0;
  if (!s) return;
  s = stripV(s);
  *a = atoi(s);
  const char* dot1 = std::strchr(s, '.');
  if (!dot1) return;
  *b = atoi(dot1 + 1);
  const char* dot2 = std::strchr(dot1 + 1, '.');
  if (!dot2) return;
  *c = atoi(dot2 + 1);
}

int compareSemver(const char* a, const char* b) {
  int aa, ab, ac, ba, bb, bc;
  parseSemver(a, &aa, &ab, &ac);
  parseSemver(b, &ba, &bb, &bc);
  if (aa != ba) return (aa < ba) ? -1 : 1;
  if (ab != bb) return (ab < bb) ? -1 : 1;
  if (ac != bc) return (ac < bc) ? -1 : 1;
  return 0;
}

bool isExpandedPartitionLayout() {
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, nullptr);
  if (!part) return false;
  return part->address >= kFfatExpandedPartitionBase;
}

const char* layoutTag() {
  return isExpandedPartitionLayout() ? "ver2" : "doom";
}

void recordCurrentLayout() {
  // Diff the running layout against the last boot's. A change means
  // the user just USB-flashed `erase_and_flash_expanded.sh` (or its
  // inverse). Latch the result so the UI can surface a one-shot panel.
  Preferences p;
  if (!p.begin(kNvsNamespace, false)) return;
  char prev[8] = "";
  p.getString(kNvsLastLayout, prev, sizeof(prev));
  const char* now = layoutTag();
  if (prev[0] && std::strcmp(prev, now) != 0) {
    sLayoutChanged = true;
    DBG("[ota] partition layout changed: %s -> %s\n", prev, now);
  }
  p.putString(kNvsLastLayout, now);
  p.end();
}

bool batteryAllowsInstall() {
#ifdef BADGE_HAS_BATTERY_GAUGE
  if (!batteryGauge.isReady()) return true;
  // Allow install if charger is plugged in regardless of charge level
  // — the worst-case brownout is mitigated and the bootloader will
  // rollback if the new image fails.
  if (batteryGauge.usbPresent()) return true;
  return batteryGauge.stateOfChargePercent() >=
         static_cast<float>(kMinBatteryPct);
#else
  return true;
#endif
}

bool urlIsHttps(const char* url) {
  return url && std::strncmp(url, "https://", 8) == 0;
}

InstallResult installFromUrl(const char* url, size_t expectedSize,
                             InstallProgressCb cb, void* user) {
  if (!url || url[0] == '\0') return InstallResult::kNoAssetCached;
  if (!batteryAllowsInstall()) {
    setError("battery too low — plug in to update");
    return InstallResult::kBatteryTooLow;
  }
  if (!wifiService.connect()) {
    setError("wifi unavailable");
    return InstallResult::kWifiUnavailable;
  }

  prepareFirmwareInstallHeap();

  // Hold WiFi awake + CPU at 240 MHz across the multi-MB firmware
  // pull. Same rationale as the AssetRegistry installer — modem sleep
  // and CPU scaling between chunks cap throughput at single-digit
  // percentages of the link rate.
  ThroughputBoost boost;
  const char* installUrl = url;
  if (urlIsHttps(url) && std::strstr(url, "github.com/") != nullptr) {
    sResolvedAssetUrl[0] = '\0';
    if (resolveRedirect(url, sResolvedAssetUrl, sizeof(sResolvedAssetUrl), 30000)) {
      installUrl = sResolvedAssetUrl;
    } else {
      DBG("[ota] redirect resolve failed; streaming original URL\n");
    }
  }

  Stream s;
  if (!s.open(installUrl, 45000)) {
    setError(s.lastError());
    return InstallResult::kHttpError;
  }
  size_t total = s.contentLength();
  if (total == 0 && expectedSize > 0) total = expectedSize;

  if (!Update.begin(total > 0 ? total : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "Update.begin failed: %s",
                  Update.errorString());
    setError(buf);
    return InstallResult::kUpdateBeginFailed;
  }

  uint8_t* chunk = otaInstallChunkScratch();
  if (!chunk) {
    Update.abort();
    setError("install chunk alloc failed");
    return InstallResult::kHttpError;
  }

  // 4 KiB read buffer — PSRAM-backed scratch so this path does not consume
  // 4 KiB of the Arduino loop stack during a multi-MB flash write loop.
  size_t written = 0;
  uint32_t lastReport = 0;
  while (true) {
    int got = s.read(chunk, 4096);
    if (got < 0) {
      Update.abort();
      setError("stream read failed");
      return InstallResult::kHttpError;
    }
    if (got == 0) {
      // EOF or stream stopped — accept if we got everything.
      if (total > 0 && written < total) {
        Update.abort();
        char buf[80];
        std::snprintf(buf, sizeof(buf),
                      "stream short: %u/%u",
                      (unsigned)written, (unsigned)total);
        setError(buf);
        return InstallResult::kHttpError;
      }
      break;
    }
    size_t w = Update.write(chunk, got);
    if (w != static_cast<size_t>(got)) {
      Update.abort();
      char buf[80];
      std::snprintf(buf, sizeof(buf),
                    "Update.write %u/%d", (unsigned)w, got);
      setError(buf);
      return InstallResult::kWriteFailed;
    }
    written += w;
    if (cb && (millis() - lastReport > 250 ||
               (total > 0 && written >= total))) {
      lastReport = millis();
      InstallProgress prog{written, total, false, InstallResult::kOk};
      cb(prog, user);
    }
    if (total > 0 && written >= total) break;
  }

  if (!Update.end(true)) {
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "Update.end failed: %s",
                  Update.errorString());
    setError(buf);
    return InstallResult::kEndFailed;
  }

  if (cb) {
    InstallProgress done{written, total, true, InstallResult::kOk};
    cb(done, user);
  }

  DBG("[ota] install complete (%u bytes); rebooting\n",
                (unsigned)written);
  return InstallResult::kOk;
}

}  // namespace

void prepareFirmwareInstallHeap() {
  constexpr uint32_t kRegistryWaitMs = 120000;
  const uint32_t t0 = millis();
  while (registry::isRefreshing()) {
    if ((uint32_t)(millis() - t0) >= kRegistryWaitMs) {
      DBG("[ota] prepareInstall: registry refresh still running after %u ms\n",
          (unsigned)kRegistryWaitMs);
      break;
    }
    delay(20);
    yield();
  }
#if BADGEOTA_HAS_MPY_COLLECT
  mpy_collect();
#endif
}

void begin() {
  if (sBegun) return;
  sBegun = true;

  // Migration-reboot detection. RTC noinit memory is wiped on a real
  // power cycle (POR / brownout / external reset / RTC wake) but
  // survives every flavour of warm reset (clean ESP.restart, panic,
  // task WDT, interrupt WDT). That makes the magic a one-way signal:
  // its presence proves we've rebooted from a system that successfully
  // completed migrate AND the user hasn't yanked power since.
  //
  // We DO NOT clear the magic here. The migration UI may not get a
  // chance to render on this boot — e.g. the first post-migration
  // boot has to format a fresh ffat, and any panic in that path
  // (we hit one in oofatfs `f_mount(NULL)` and fixed it, but the
  // class of bug remains) would otherwise wipe our only evidence
  // that the migration succeeded. Clearing is deferred to
  // acknowledgeMigrationBoot(), which the UI calls only after the
  // user has actually dismissed the panel. A power cycle in between
  // still wipes everything, which is the correct semantics.
  //
  // Belt-and-suspenders: also accept the magic when the reset
  // reason is consistent with "we rebooted from a system that had
  // the migration code path active". Reject obvious power-up
  // reasons defensively even though RTC would already have been
  // wiped in those cases.
  const esp_reset_reason_t resetReason = esp_reset_reason();
  const uint32_t observedMagic = sMigrationBootMagic;
  const bool warmReset =
      resetReason == ESP_RST_SW       ||
      resetReason == ESP_RST_PANIC    ||
      resetReason == ESP_RST_INT_WDT  ||
      resetReason == ESP_RST_TASK_WDT ||
      resetReason == ESP_RST_WDT;
  if (observedMagic == kMigrationBootMagic && warmReset) {
    sJustMigratedReboot = true;
    DBG("[ota] migrate: post-migration boot detected "
        "(reset_reason=%d) — UI will show one-shot welcome\n",
        (int)resetReason);
  } else if (observedMagic == kMigrationBootMagic) {
    // Magic survives only across warm resets; seeing it together
    // with a cold-reset reason means either (a) the rule above
    // missed a case we should accept, or (b) something corrupted
    // RTC slow. Clear it defensively so we don't loop forever.
    sMigrationBootMagic = 0;
    DBG("[ota] migrate: magic present with cold reset_reason=%d — "
        "clearing without announce\n", (int)resetReason);
  }

  loadCache();
  recordCurrentLayout();

  // Detect "we just OTA-installed and haven't been validated yet".
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running) {
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
      sPendingVerify = (state == ESP_OTA_IMG_PENDING_VERIFY);
      DBG("[ota] running partition state=%d pending_verify=%d\n",
                    (int)state, sPendingVerify ? 1 : 0);
    }
  }
  DBG("[ota] cache loaded: tag='%s' size=%u last_epoch=%lu\n",
                sLatestTag, (unsigned)sAssetSize,
                (unsigned long)sLastCheckEpoch);
}

void tick() {
  // No-op since the cooldown was removed. OTAService now drives a
  // one-shot check on each WiFi-up edge and the Update screen drives
  // an explicit check on entry / D-pad Up. Kept as a stable symbol
  // for any external callers that haven't been updated.
}

CheckResult checkNow(bool ignoreCooldown) {
  (void)ignoreCooldown;
  if (!sBegun) begin();
  setError("");

  char url[160];
  std::snprintf(url, sizeof(url),
                "https://api.github.com/repos/%s/releases/latest",
                OTA_GITHUB_REPO);

  logOtaCheckHeap("pre-github-http");

  char* body = nullptr;
  size_t bodyLen = 0;
  HttpResult httpRes = getJson(url, &body, &bodyLen, kJsonMaxBytes, 20000);
  if (!httpRes.ok) {
    logOtaCheckHeap("post-github-http-fail");
    setError(httpRes.error);
    if (body) std::free(body);
    return CheckResult::kNetworkError;
  }

  logOtaCheckHeap("post-github-http-ok");

  // GitHub release JSON. We only care about `tag_name` + the asset
  // whose `name` matches OTA_ASSET_NAME. The same .bin works on both
  // partition layouts (see BadgeOTA.h comment).
  BadgeMemory::PsramJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    logOtaCheckHeap("post-json-parse-fail");
    char buf[64];
    std::snprintf(buf, sizeof(buf), "json parse: %s", err.c_str());
    setError(buf);
    std::free(body);
    return CheckResult::kParseError;
  }

  const char* tag = doc["tag_name"] | "";
  if (!tag[0]) {
    setError("release missing tag_name");
    std::free(body);
    return CheckResult::kParseError;
  }

  // Find matching asset.
  const char* assetUrl = nullptr;
  size_t assetSize = 0;
  JsonArray assets = doc["assets"].as<JsonArray>();
  for (JsonObject a : assets) {
    const char* name = a["name"] | "";
    if (std::strcmp(name, OTA_ASSET_NAME) == 0) {
      assetUrl = a["browser_download_url"] | "";
      assetSize = a["size"] | 0u;
      break;
    }
  }

  if (!assetUrl || !assetUrl[0]) {
    // Cache the tag (so we can show "v0.1.4 has no asset") but mark
    // result as no-asset.
    std::strncpy(sLatestTag, tag, sizeof(sLatestTag) - 1);
    sLatestTag[sizeof(sLatestTag) - 1] = '\0';
    sAssetUrl[0] = '\0';
    sAssetSize = 0;
    sLastCheckEpoch = wifiService.clockReady() ? time(nullptr) : sLastCheckEpoch;
    persistCache();
    char buf[80];
    std::snprintf(buf, sizeof(buf),
                  "release %s has no '%s' asset", tag, OTA_ASSET_NAME);
    setError(buf);
    std::free(body);
    return CheckResult::kNoMatchingAsset;
  }

  std::strncpy(sLatestTag, tag, sizeof(sLatestTag) - 1);
  sLatestTag[sizeof(sLatestTag) - 1] = '\0';
  std::strncpy(sAssetUrl, assetUrl, sizeof(sAssetUrl) - 1);
  sAssetUrl[sizeof(sAssetUrl) - 1] = '\0';
  sAssetSize = assetSize;
  sLastCheckEpoch = wifiService.clockReady() ? time(nullptr) : 1;  // 1 = "checked once, no clock"
  persistCache();
  std::free(body);

  logOtaCheckHeap("check-complete");

  const int cmp = compareSemver(sLatestTag, FIRMWARE_VERSION);
  DBG("[ota] latest=%s current=%s cmp=%d size=%u layout=%s\n",
                sLatestTag, FIRMWARE_VERSION, cmp,
                (unsigned)sAssetSize, layoutTag());

  if (cmp > 0) return CheckResult::kOkNewerAvailable;
  if (cmp == 0) return CheckResult::kOkUpToDate;
  return CheckResult::kOkOlder;
}

bool updateAvailable() {
  if (!sBegun) return false;
  if (sLatestTag[0] == '\0') return false;
  if (sAssetUrl[0] == '\0') return false;
  return compareSemver(sLatestTag, FIRMWARE_VERSION) > 0;
}

const char* latestKnownTag() { return sLatestTag; }
const char* latestKnownAssetUrl() { return sAssetUrl; }
size_t latestKnownAssetSize() { return sAssetSize; }
time_t lastCheckEpoch() { return sLastCheckEpoch; }
const char* lastErrorMessage() { return sLastError; }

InstallResult installCached(InstallProgressCb cb, void* user) {
  setError("");
  if (sAssetUrl[0] == '\0') return InstallResult::kNoAssetCached;
  return installFromUrl(sAssetUrl, sAssetSize, cb, user);
}

void markCurrentAppValidIfPending() {
  if (!sPendingVerify || sValidated) return;
  esp_err_t rc = esp_ota_mark_app_valid_cancel_rollback();
  if (rc == ESP_OK) {
    sValidated = true;
    DBG("[ota] running app marked valid; rollback cancelled\n");
  } else {
    DBG("[ota] mark_valid failed rc=%d\n", (int)rc);
  }
}

bool runningPendingVerify() {
  return sPendingVerify && !sValidated;
}

// ── Storage helpers ───────────────────────────────────────────────────────

size_t ffatPartitionBytes() {
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, nullptr);
  return part ? static_cast<size_t>(part->size) : 0;
}

size_t ffatVolumeBytes() {
  FATFS* fs = replay_get_fatfs();
  if (!fs) return 0;
  // Force FATFS to populate n_fatent / csize / ssize. The fields may
  // be stale until f_getfree walks the FAT (this matches what
  // MicroPython's `os.statvfs` does — see vfs_fat.c).
  DWORD freeClusters = 0;
  if (f_getfree(fs, &freeClusters) != FR_OK) return 0;
  if (fs->n_fatent < 2) return 0;
  // Sector size is dynamic on this build — `MICROPY_FATFS_MAX_SS`
  // is 4096, so FATFS exposes the per-volume `ssize` field instead
  // of the compile-time FF_MIN_SS=512. Hardcoding 512 underestimates
  // total bytes by 8× and rounds the storage line to 0 MB.
  const uint32_t sectorBytes = static_cast<uint32_t>(fs->ssize);
  uint64_t totalClusters = static_cast<uint64_t>(fs->n_fatent - 2);
  uint64_t bytes = totalClusters *
                   static_cast<uint64_t>(fs->csize) *
                   static_cast<uint64_t>(sectorBytes);
  return static_cast<size_t>(bytes);
}

bool ffatUsesExpandedPartitionLayout() {
  return isExpandedPartitionLayout();
}

bool canOfferLayoutMigration() {
  return !isExpandedPartitionLayout();
}

bool layoutJustChanged() {
  return sLayoutChanged && !sLayoutAcknowledged;
}

void acknowledgeLayoutChange() {
  sLayoutAcknowledged = true;
}

bool ffatExpansionAvailable() {
  size_t partBytes = ffatPartitionBytes();
  size_t volBytes = ffatVolumeBytes();
  if (partBytes == 0 || volBytes == 0) return false;
  // Only surface the option when the gap is meaningful — 256 KB
  // accounts for FAT metadata overhead so a freshly-formatted volume
  // doesn't trigger a false "expand" prompt.
  if (partBytes <= volBytes) return false;
  return (partBytes - volBytes) >= (256u * 1024u);
}

void reformatFfatAndReboot() {
  DBG("[ota] reformatting ffat — all user data will be lost\n");
  // The replay_bdev helper unmounts, mkfs's, and ESP.restart()s. It
  // does not return on success.
  int rc = replay_bdev_reformat_and_reboot();
  // If we get here, something went wrong. Reboot anyway to recover.
  DBG("[ota] reformat helper returned rc=%d; rebooting\n", rc);
  delay(300);
  ESP.restart();
}

// ── Partition table migration ─────────────────────────────────────────────

namespace {

size_t embeddedPartitionTableLen() {
  const ptrdiff_t n = kPartitionTableVer2End - kPartitionTableVer2Start;
  if (n <= 0) return 0;
  return static_cast<size_t>(n);
}

bool migrationBatteryOk() {
#ifdef BADGE_HAS_BATTERY_GAUGE
  if (!batteryGauge.isReady()) {
    // No gauge → no signal either way; let the user proceed. They
    // had to triple-confirm to get here anyway.
    return true;
  }
  if (batteryGauge.usbPresent()) return true;
  return batteryGauge.stateOfChargePercent() >= kMigrationMinBatteryPct;
#else
  return true;
#endif
}

bool runningFromApp0() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  return running && running->address == kApp0FlashOffset;
}

}  // namespace

bool migrationAssetPresent() {
  const size_t n = embeddedPartitionTableLen();
  return n > 0 && n <= kPartitionTableSectorBytes;
}

MigrationResult migrateToExpandedLayout() {
  setError("");
  if (isExpandedPartitionLayout()) {
    return MigrationResult::kAlreadyExpanded;
  }
  if (!migrationBatteryOk()) {
    setError("battery <50% — charge first");
    return MigrationResult::kBatteryTooLow;
  }
  if (!runningFromApp0()) {
    // Booting from app1 means the bootloader's "current image" lives
    // at 0x3F0000. The `_ver2` table has nothing at that address, so
    // the bootloader would fall back / fail to find a valid image on
    // the next boot. Refuse and tell the user to OTA-install first
    // (every OTA install writes to the *inactive* slot, so two
    // installs in a row would land them back on app0).
    setError("run from app0 — reinstall OTA first");
    return MigrationResult::kNotRunningFromApp0;
  }
  const size_t newLen = embeddedPartitionTableLen();
  if (newLen == 0 || newLen > kPartitionTableSectorBytes) {
    setError("partition blob missing from firmware");
    return MigrationResult::kEmbedMissing;
  }

  // Snapshot the OLD table so we can roll back on any failure short
  // of a literal power cut. Heap rather than stack: the partition
  // table sector is only 4 KB but the loop stack is also where the
  // GUI render path lives, and adding 4 KB to it would be tight.
  // MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA: bootloader_flash_* expects
  // DRAM-backed buffers (the flash driver may DMA from them, and
  // PSRAM cache may be disabled during the write window).
  uint8_t* oldTable = static_cast<uint8_t*>(
      heap_caps_malloc(kPartitionTableSectorBytes,
                       MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA |
                           MALLOC_CAP_8BIT));
  if (!oldTable) {
    setError("alloc failed (DRAM)");
    return MigrationResult::kFlashReadFailed;
  }
  std::memset(oldTable, 0xFF, kPartitionTableSectorBytes);
  esp_err_t rc = bootloader_flash_read(kPartitionTableFlashOffset,
                                       oldTable,
                                       kPartitionTableSectorBytes,
                                       /*allow_decrypt=*/false);
  if (rc != ESP_OK) {
    heap_caps_free(oldTable);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "flash read rc=%d", (int)rc);
    setError(buf);
    return MigrationResult::kFlashReadFailed;
  }

  // The dangerous window starts here. In arduino-esp32 (sdkconfig:
  // CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS=1) any esp_flash_erase /
  // esp_flash_write whose range overlaps the bootloader, partition
  // table, or NVS region panics via abort(). bootloader_flash_* in
  // app context is a one-line tail-call to esp_flash_* (verified by
  // disassembling libbootloader_support.a), so it hits the same trap.
  // The IDF-blessed runtime bypass is
  // esp_flash_set_dangerous_write_protection(chip, false).
  // We re-enable protection after the write closes — both on the
  // success path (before reboot, defensive) and on every rollback
  // exit — so unrelated future bugs still abort cleanly.
  if (!esp_flash_default_chip) {
    heap_caps_free(oldTable);
    setError("no default flash chip");
    return MigrationResult::kFlashEraseFailed;
  }
  esp_flash_set_dangerous_write_protection(esp_flash_default_chip, false);

  DBG("[ota] migrate: erasing partition table sector @ 0x%08X (%u bytes)\n",
      (unsigned)kPartitionTableFlashOffset,
      (unsigned)kPartitionTableSectorBytes);
  rc = bootloader_flash_erase_range(kPartitionTableFlashOffset,
                                    kPartitionTableSectorBytes);
  if (rc != ESP_OK) {
    esp_flash_set_dangerous_write_protection(esp_flash_default_chip, true);
    heap_caps_free(oldTable);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "flash erase rc=%d", (int)rc);
    setError(buf);
    return MigrationResult::kFlashEraseFailed;
  }

  // bootloader_flash_write requires a non-const src buffer; the
  // embedded blob is `const uint8_t*` in flash. Stage it through an
  // internal-RAM DMA-capable buffer so the driver can pull from it.
  uint8_t* writeBuf = static_cast<uint8_t*>(
      heap_caps_malloc(kPartitionTableSectorBytes,
                       MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA |
                           MALLOC_CAP_8BIT));
  if (!writeBuf) {
    // Rollback: re-write the snapshot we already have. The sector
    // is erased to 0xFF right now; an alloc failure here would
    // otherwise leave the badge unbootable.
    bootloader_flash_write(kPartitionTableFlashOffset, oldTable,
                           kPartitionTableSectorBytes, false);
    esp_flash_set_dangerous_write_protection(esp_flash_default_chip, true);
    heap_caps_free(oldTable);
    setError("alloc failed (write buf)");
    return MigrationResult::kFlashWriteFailed;
  }
  std::memset(writeBuf, 0xFF, kPartitionTableSectorBytes);
  std::memcpy(writeBuf, kPartitionTableVer2Start, newLen);

  rc = bootloader_flash_write(kPartitionTableFlashOffset, writeBuf,
                              kPartitionTableSectorBytes,
                              /*write_encrypted=*/false);
  heap_caps_free(writeBuf);
  if (rc != ESP_OK) {
    // Best-effort rollback. If THIS fails the badge is bricked, but
    // that scenario requires the flash to be misbehaving in two
    // separate operations back-to-back.
    DBG("[ota] migrate: write failed rc=%d; rolling back\n", (int)rc);
    bootloader_flash_erase_range(kPartitionTableFlashOffset,
                                 kPartitionTableSectorBytes);
    bootloader_flash_write(kPartitionTableFlashOffset, oldTable,
                           kPartitionTableSectorBytes, false);
    esp_flash_set_dangerous_write_protection(esp_flash_default_chip, true);
    heap_caps_free(oldTable);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "flash write rc=%d", (int)rc);
    setError(buf);
    return MigrationResult::kFlashWriteFailed;
  }

  // Read it back and compare. If verify fails we still have the old
  // table bytes in `oldTable`; rewrite them and bail.
  uint8_t* verify = static_cast<uint8_t*>(
      heap_caps_malloc(kPartitionTableSectorBytes,
                       MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA |
                           MALLOC_CAP_8BIT));
  bool verifyOk = false;
  if (verify) {
    std::memset(verify, 0, kPartitionTableSectorBytes);
    if (bootloader_flash_read(kPartitionTableFlashOffset, verify,
                              kPartitionTableSectorBytes,
                              /*allow_decrypt=*/false) == ESP_OK &&
        std::memcmp(verify, kPartitionTableVer2Start, newLen) == 0) {
      verifyOk = true;
    }
    heap_caps_free(verify);
  }
  if (!verifyOk) {
    DBG("[ota] migrate: verify failed; rolling back\n");
    bootloader_flash_erase_range(kPartitionTableFlashOffset,
                                 kPartitionTableSectorBytes);
    bootloader_flash_write(kPartitionTableFlashOffset, oldTable,
                           kPartitionTableSectorBytes, false);
    esp_flash_set_dangerous_write_protection(esp_flash_default_chip, true);
    heap_caps_free(oldTable);
    setError("verify failed; rolled back");
    return MigrationResult::kVerifyFailed;
  }

  // Success path: re-arm the guard before reboot. This is purely
  // defensive — we're about to ESP.restart() — but it's the right
  // shape for code reviewers and for any future change that might
  // not immediately reboot after the swap.
  esp_flash_set_dangerous_write_protection(esp_flash_default_chip, true);
  heap_caps_free(oldTable);

  // Arm the post-migration announce signal *before* ESP.restart().
  // RTC noinit memory survives the soft reset, so begin() on the
  // next boot will see the magic + ESP_RST_SW combo and latch
  // sJustMigratedReboot. See the kMigrationBootMagic block at the
  // top of this file for the full rationale.
  sMigrationBootMagic = kMigrationBootMagic;

  DBG("[ota] migrate: partition table swapped to _ver2 (%u bytes); "
      "rebooting (RTC magic armed)\n", (unsigned)newLen);
  // Brief settle so the DBG line + any UI-side "rebooting…" paint
  // makes it out before we cycle.
  delay(200);
  ESP.restart();
  return MigrationResult::kOk;  // unreachable
}

bool justRebootedFromLayoutMigration() {
  return sJustMigratedReboot && !sMigrationBootAcknowledged;
}

void acknowledgeMigrationBoot() {
  sMigrationBootAcknowledged = true;
  // Now — and only now — clear the RTC magic. Until this point the
  // magic was the durable "user hasn't seen the announce yet" flag;
  // a panic in the post-migration boot would otherwise have lost
  // the signal entirely (see the matching comment in begin()).
  sMigrationBootMagic = 0;
}

}  // namespace ota
