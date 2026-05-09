#include "AssetRegistry.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "lib/oofatfs/ff.h"
FATFS* replay_get_fatfs(void);
}

#include "OTAHttp.h"
#include "../api/WiFiService.h"
#include "../infra/BadgeConfig.h"
#include "../infra/Filesystem.h"

namespace ota::registry {

namespace {

constexpr const char* kNvsNamespace = "badge_assets";
constexpr const char* kNvsLastEpoch = "last_epoch";
constexpr uint32_t kRefreshCooldownSec = 24 * 60 * 60;
constexpr size_t kRegistryJsonMax = 12 * 1024;

AssetEntry sAssets[kMaxRegistryAssets];
uint8_t sAssetCount = 0;
char sLastError[96] = "";
time_t sLastRefreshEpoch = 0;
bool sBegun = false;

void setError(const char* msg) {
  if (!msg) msg = "";
  std::strncpy(sLastError, msg, sizeof(sLastError) - 1);
  sLastError[sizeof(sLastError) - 1] = '\0';
}

void copyField(char* dst, size_t cap, const char* src) {
  if (!src) src = "";
  std::strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

bool nvsKey(char* out, size_t cap, const char* prefix, const char* id) {
  // Preferences keys are limited to 15 chars. We trim aggressively.
  if (!out || !prefix || !id) return false;
  size_t plen = std::strlen(prefix);
  if (plen >= cap) return false;
  std::memcpy(out, prefix, plen);
  size_t room = cap - plen - 1;
  size_t ilen = std::strlen(id);
  if (ilen > room) ilen = room;
  std::memcpy(out + plen, id, ilen);
  out[plen + ilen] = '\0';
  return true;
}

// Loads the persisted "installed version" string for an id. Empty
// string if never installed.
void loadInstalledVersion(const char* id, char* out, size_t cap) {
  out[0] = '\0';
  Preferences p;
  if (!p.begin(kNvsNamespace, true)) return;
  char key[16];
  if (nvsKey(key, sizeof(key), "v_", id)) {
    p.getString(key, out, cap);
  }
  p.end();
}

void persistInstalledVersion(const char* id, const char* version) {
  Preferences p;
  if (!p.begin(kNvsNamespace, false)) return;
  char key[16];
  if (nvsKey(key, sizeof(key), "v_", id)) {
    if (version && version[0]) p.putString(key, version);
    else p.remove(key);
  }
  p.end();
}

void persistRefreshEpoch(time_t epoch) {
  Preferences p;
  if (!p.begin(kNvsNamespace, false)) return;
  p.putULong(kNvsLastEpoch, static_cast<uint32_t>(epoch));
  p.end();
}

void loadRefreshEpoch() {
  Preferences p;
  if (!p.begin(kNvsNamespace, true)) return;
  sLastRefreshEpoch =
      static_cast<time_t>(p.getULong(kNvsLastEpoch, 0));
  p.end();
}

// Convert binary digest to lowercase hex.
void hexEncode(const uint8_t* in, size_t len, char* out, size_t cap) {
  static const char hex[] = "0123456789abcdef";
  size_t need = len * 2 + 1;
  if (cap < need) {
    if (cap > 0) out[0] = '\0';
    return;
  }
  for (size_t i = 0; i < len; ++i) {
    out[i * 2] = hex[in[i] >> 4];
    out[i * 2 + 1] = hex[in[i] & 0x0f];
  }
  out[len * 2] = '\0';
}

bool hexEqualsCaseInsensitive(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
    if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
    if (ca != cb) return false;
    ++a; ++b;
  }
  return *a == '\0' && *b == '\0';
}

bool ensureFreeSpace(size_t needed) {
  if (needed == 0) return true;
  FATFS* fs = replay_get_fatfs();
  if (!fs) return true;  // FS not mounted yet — defer to write failure
  DWORD freeClusters = 0;
  if (f_getfree(fs, &freeClusters) != FR_OK) return true;
  // Cluster size = sectors-per-cluster * 512 (oofatfs uses 512B sectors).
  uint32_t clusterBytes = static_cast<uint32_t>(fs->csize) * 512u;
  uint64_t freeBytes = static_cast<uint64_t>(freeClusters) * clusterBytes;
  return freeBytes >= needed;
}

}  // namespace

void begin() {
  if (sBegun) return;
  sBegun = true;
  loadRefreshEpoch();
  Serial.printf("[registry] cache loaded: last_epoch=%lu\n",
                (unsigned long)sLastRefreshEpoch);
}

void tick() {
  if (!sBegun) return;
  if (!badgeConfig.assetRegistryUrl()[0]) return;
  if (!wifiService.isConnected()) return;
  if (!wifiService.clockReady()) return;
  const time_t now = time(nullptr);
  if (now <= 0) return;
  if (sLastRefreshEpoch != 0 &&
      (uint32_t)(now - sLastRefreshEpoch) < kRefreshCooldownSec) {
    return;
  }
  Serial.println("[registry] daily refresh fired");
  refresh(false);
}

RegistryRefresh refresh(bool ignoreCooldown) {
  if (!sBegun) begin();
  setError("");

  const char* url = badgeConfig.assetRegistryUrl();
  if (!url || !url[0]) {
    setError("asset_registry_url not configured");
    return RegistryRefresh::kNotConfigured;
  }

  if (!ignoreCooldown && sLastRefreshEpoch != 0 &&
      wifiService.clockReady()) {
    const time_t now = time(nullptr);
    if (now > 0 && (uint32_t)(now - sLastRefreshEpoch) < kRefreshCooldownSec) {
      return RegistryRefresh::kCooldownActive;
    }
  }

  char* body = nullptr;
  size_t bodyLen = 0;
  HttpResult httpRes =
      getJson(url, &body, &bodyLen, kRegistryJsonMax, 20000);
  if (!httpRes.ok) {
    setError(httpRes.error);
    if (body) std::free(body);
    return RegistryRefresh::kNetworkError;
  }

  DynamicJsonDocument doc(kRegistryJsonMax);
  DeserializationError err = deserializeJson(doc, body);
  std::free(body);
  if (err) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "json parse: %s", err.c_str());
    setError(buf);
    return RegistryRefresh::kParseError;
  }

  JsonArray assets = doc["assets"].as<JsonArray>();
  sAssetCount = 0;
  for (JsonObject a : assets) {
    if (sAssetCount >= kMaxRegistryAssets) break;
    AssetEntry& e = sAssets[sAssetCount];
    std::memset(&e, 0, sizeof(e));
    copyField(e.id, sizeof(e.id), a["id"] | "");
    if (e.id[0] == '\0') continue;
    copyField(e.name, sizeof(e.name), a["name"] | e.id);
    copyField(e.version, sizeof(e.version), a["version"] | "0");
    copyField(e.url, sizeof(e.url), a["url"] | "");
    copyField(e.sha256, sizeof(e.sha256), a["sha256"] | "");
    copyField(e.dest_path, sizeof(e.dest_path), a["dest_path"] | "");
    copyField(e.description, sizeof(e.description),
              a["description"] | "");
    e.size = a["size"] | 0u;
    e.min_free_bytes = a["min_free_bytes"] | 0u;
    if (e.url[0] == '\0' || e.dest_path[0] == '\0') continue;
    sAssetCount++;
  }

  sLastRefreshEpoch = wifiService.clockReady() ? time(nullptr) : 1;
  persistRefreshEpoch(sLastRefreshEpoch);
  Serial.printf("[registry] parsed %u assets\n",
                static_cast<unsigned>(sAssetCount));
  return RegistryRefresh::kOk;
}

size_t count() { return sAssetCount; }

const AssetEntry* at(size_t index) {
  if (index >= sAssetCount) return nullptr;
  return &sAssets[index];
}

const AssetEntry* findById(const char* id) {
  if (!id) return nullptr;
  for (size_t i = 0; i < sAssetCount; ++i) {
    if (std::strcmp(sAssets[i].id, id) == 0) return &sAssets[i];
  }
  return nullptr;
}

AssetStatus statusOf(const AssetEntry& entry) {
  char installed[kAssetVersionMax] = "";
  loadInstalledVersion(entry.id, installed, sizeof(installed));
  // File on disk must also exist; otherwise treat as not-installed
  // even if NVS says we did install once (user could have wiped FS).
  if (installed[0] == '\0') return AssetStatus::kNotInstalled;
  if (!Filesystem::fileExists(entry.dest_path)) {
    return AssetStatus::kNotInstalled;
  }
  if (std::strcmp(installed, entry.version) != 0) {
    return AssetStatus::kUpdateAvailable;
  }
  return AssetStatus::kInstalled;
}

const char* installedVersionOf(const AssetEntry& entry) {
  static char sBuf[kAssetVersionMax];
  loadInstalledVersion(entry.id, sBuf, sizeof(sBuf));
  return sBuf;
}

bool install(const AssetEntry& entry, AssetProgressCb cb, void* user) {
  setError("");
  if (entry.dest_path[0] != '/') {
    setError("dest_path must be absolute");
    if (cb) {
      AssetProgress p{0, 0, true, AssetInstallResult::kDestPathInvalid};
      cb(p, user);
    }
    return false;
  }
  size_t needed = entry.min_free_bytes;
  if (needed == 0) needed = entry.size + 1024;  // small slack
  if (!ensureFreeSpace(needed)) {
    char buf[80];
    std::snprintf(buf, sizeof(buf),
                  "need %u bytes free", (unsigned)needed);
    setError(buf);
    if (cb) {
      AssetProgress p{0, 0, true, AssetInstallResult::kInsufficientSpace};
      cb(p, user);
    }
    return false;
  }

  char tmpPath[kAssetPathMax + 8];
  std::snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", entry.dest_path);

  FATFS* fs = replay_get_fatfs();
  if (!fs) {
    setError("filesystem not mounted");
    if (cb) {
      AssetProgress p{0, 0, true, AssetInstallResult::kFsWriteError};
      cb(p, user);
    }
    return false;
  }

  Stream s;
  if (!s.open(entry.url, 30000)) {
    setError(s.lastError());
    if (cb) {
      AssetProgress p{0, 0, true, AssetInstallResult::kHttpError};
      cb(p, user);
    }
    return false;
  }
  size_t total = s.contentLength();
  if (total == 0 && entry.size > 0) total = entry.size;

  // Open the .tmp file under the FATFS lock for the whole download.
  // We hold the lock the whole time so other writers can't race
  // through directory cluster bookkeeping while we extend the file.
  Filesystem::IOLock lock;
  f_unlink(fs, tmpPath);  // best-effort cleanup of stale .tmp

  FIL fil;
  FRESULT ores = f_open(fs, &fil, tmpPath, FA_WRITE | FA_CREATE_NEW);
  if (ores != FR_OK) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "open tmp fr=%d", (int)ores);
    setError(buf);
    if (cb) {
      AssetProgress p{0, 0, true, AssetInstallResult::kFsWriteError};
      cb(p, user);
    }
    return false;
  }

  mbedtls_sha256_context shaCtx;
  bool wantSha = entry.sha256[0] != '\0';
  if (wantSha) {
    mbedtls_sha256_init(&shaCtx);
    mbedtls_sha256_starts(&shaCtx, 0);
  }

  uint8_t chunk[1024];
  size_t written = 0;
  uint32_t lastReport = 0;
  bool ok = true;
  while (true) {
    int got = s.read(chunk, sizeof(chunk));
    if (got < 0) { setError("stream read failed"); ok = false; break; }
    if (got == 0) {
      if (total > 0 && written < total) {
        char buf[80];
        std::snprintf(buf, sizeof(buf),
                      "stream short %u/%u",
                      (unsigned)written, (unsigned)total);
        setError(buf);
        ok = false;
      }
      break;
    }
    UINT wrote = 0;
    FRESULT wres = f_write(&fil, chunk, (UINT)got, &wrote);
    if (wres != FR_OK || wrote != (UINT)got) {
      char buf[64];
      std::snprintf(buf, sizeof(buf),
                    "f_write fr=%d %u/%d", (int)wres, (unsigned)wrote, got);
      setError(buf);
      ok = false;
      break;
    }
    if (wantSha) mbedtls_sha256_update(&shaCtx, chunk, got);
    written += wrote;
    if (cb && (millis() - lastReport > 250 ||
               (total > 0 && written >= total))) {
      lastReport = millis();
      AssetProgress p{written, total, false, AssetInstallResult::kOk};
      cb(p, user);
    }
    if (total > 0 && written >= total) break;
  }
  f_sync(&fil);
  f_close(&fil);

  if (!ok) {
    if (wantSha) mbedtls_sha256_free(&shaCtx);
    f_unlink(fs, tmpPath);
    if (cb) {
      AssetProgress p{written, total, true, AssetInstallResult::kFsWriteError};
      cb(p, user);
    }
    return false;
  }

  if (wantSha) {
    uint8_t digest[32];
    mbedtls_sha256_finish(&shaCtx, digest);
    mbedtls_sha256_free(&shaCtx);
    char hex[65];
    hexEncode(digest, sizeof(digest), hex, sizeof(hex));
    if (!hexEqualsCaseInsensitive(hex, entry.sha256)) {
      char buf[80];
      std::snprintf(buf, sizeof(buf),
                    "sha256 mismatch (got %.16s...)", hex);
      setError(buf);
      f_unlink(fs, tmpPath);
      if (cb) {
        AssetProgress p{written, total, true,
                        AssetInstallResult::kSha256Mismatch};
        cb(p, user);
      }
      return false;
    }
  }

  // Atomic rename: drop the live file, promote the .tmp.
  f_unlink(fs, entry.dest_path);
  FRESULT rres = f_rename(fs, tmpPath, entry.dest_path);
  if (rres != FR_OK) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "rename fr=%d", (int)rres);
    setError(buf);
    f_unlink(fs, tmpPath);
    if (cb) {
      AssetProgress p{written, total, true, AssetInstallResult::kFsWriteError};
      cb(p, user);
    }
    return false;
  }

  persistInstalledVersion(entry.id, entry.version);
  Serial.printf("[registry] installed %s -> %s (%u bytes)\n",
                entry.id, entry.dest_path, (unsigned)written);
  if (cb) {
    AssetProgress p{written, total, true, AssetInstallResult::kOk};
    cb(p, user);
  }
  return true;
}

bool remove(const AssetEntry& entry) {
  setError("");
  Filesystem::removeFile(entry.dest_path);
  persistInstalledVersion(entry.id, "");
  return true;
}

const char* lastErrorMessage() { return sLastError; }
time_t lastRefreshEpoch() { return sLastRefreshEpoch; }

}  // namespace ota::registry
