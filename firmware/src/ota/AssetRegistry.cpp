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
//
// We `isKey()`-guard the read because Preferences::getString prints a
// loud `nvs_get_str len fail: ... NOT_FOUND` to Serial on every miss,
// and AssetLibraryScreen::formatItem() calls into here once per row
// per frame. For the common "asset not installed yet" case that turns
// into a flood of logs and pushes the GUI service over its frame
// budget. isKey is silent when the key is absent.
void loadInstalledVersion(const char* id, char* out, size_t cap) {
  out[0] = '\0';
  Preferences p;
  if (!p.begin(kNvsNamespace, true)) return;
  char key[16];
  if (nvsKey(key, sizeof(key), "v_", id) && p.isKey(key)) {
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

// Returns the FATFS volume's free byte count, or 0 if it cannot be
// queried. Uses fs->ssize (actual sector size) — the ESP32
// wear-levelling layer presents 4096-byte sectors, not the 512-byte
// default that oofatfs supports. Hardcoding 512 underreports free
// space by 8x and made every install fail the min_free_bytes gate
// even on a near-empty partition.
uint64_t getFreeBytes() {
  FATFS* fs = replay_get_fatfs();
  if (!fs) return 0;
  DWORD freeClusters = 0;
  if (f_getfree(fs, &freeClusters) != FR_OK) return 0;
  uint32_t clusterBytes =
      static_cast<uint32_t>(fs->csize) * static_cast<uint32_t>(fs->ssize);
  return static_cast<uint64_t>(freeClusters) * clusterBytes;
}

bool ensureFreeSpace(size_t needed, uint64_t* outFreeBytes = nullptr) {
  uint64_t freeBytes = getFreeBytes();
  if (outFreeBytes) *outFreeBytes = freeBytes;
  if (needed == 0) return true;
  if (freeBytes == 0) return true;  // unknown — defer to write failure
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
  if (!badgeConfig.communityAppsUrl()[0]) return;
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

  const char* url = badgeConfig.communityAppsUrl();
  if (!url || !url[0]) {
    setError("community_apps_url not configured");
    return RegistryRefresh::kNotConfigured;
  }

  if (!ignoreCooldown && sLastRefreshEpoch != 0 &&
      wifiService.clockReady()) {
    const time_t now = time(nullptr);
    if (now > 0 && (uint32_t)(now - sLastRefreshEpoch) < kRefreshCooldownSec) {
      return RegistryRefresh::kCooldownActive;
    }
  }

  Serial.printf("[registry] GET %s\n", url);
  char* body = nullptr;
  size_t bodyLen = 0;
  HttpResult httpRes =
      getJson(url, &body, &bodyLen, kRegistryJsonMax, 20000);
  if (!httpRes.ok) {
    Serial.printf("[registry] refresh failed: code=%d %s\n",
                  httpRes.httpCode, httpRes.error);
    setError(httpRes.error);
    if (body) std::free(body);
    return RegistryRefresh::kNetworkError;
  }

  Serial.printf("[registry] body len=%u\n", (unsigned)bodyLen);
  auto dumpEscaped = [](const char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      char c = p[i];
      if (c >= 0x20 && c < 0x7f) Serial.write(c);
      else Serial.printf("\\x%02x", (uint8_t)c);
    }
  };
  Serial.print("[registry] first64=");
  dumpEscaped(body, bodyLen < 64 ? bodyLen : 64);
  Serial.println();
  if (bodyLen > 64) {
    Serial.print("[registry] last64=");
    const size_t off = bodyLen > 64 ? bodyLen - 64 : 0;
    dumpEscaped(body + off, bodyLen - off);
    Serial.println();
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
  uint64_t freeBytes = 0;
  if (!ensureFreeSpace(needed, &freeBytes)) {
    Serial.printf("[registry] insufficient space: need=%u have=%llu\n",
                  (unsigned)needed, (unsigned long long)freeBytes);
    char buf[80];
    std::snprintf(buf, sizeof(buf),
                  "need %u, have %u KB",
                  (unsigned)(needed / 1024),
                  (unsigned)(freeBytes / 1024));
    setError(buf);
    if (cb) {
      AssetProgress p{0, 0, true, AssetInstallResult::kInsufficientSpace};
      cb(p, user);
    }
    return false;
  }
  Serial.printf("[registry] space ok: need=%u have=%llu\n",
                (unsigned)needed, (unsigned long long)freeBytes);

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

  Serial.printf("[registry] install GET %s -> %s\n",
                entry.url, entry.dest_path);
  // Hold the radio awake + CPU at 240 MHz for the whole download.
  // Without this, WiFi modem sleep + dynamic CPU scaling between
  // chunks easily collapses throughput from ~250 KB/s to ~30 KB/s
  // on a TLS stream.
  ThroughputBoost boost;
  Stream s;
  if (!s.open(entry.url, 30000)) {
    Serial.printf("[registry] install open failed: %s\n", s.lastError());
    setError(s.lastError());
    if (cb) {
      AssetProgress p{0, 0, true, AssetInstallResult::kHttpError};
      cb(p, user);
    }
    return false;
  }
  // Canonical total: prefer the registry-declared size (we can trust it
  // across resumes), fall back to the server's Content-Length on the
  // initial 200. After a Range-resumed 206 the server only knows the
  // *remaining* slice size, so caching `total` from the first response
  // here matters — we use it for progress + short-stream detection
  // throughout the retry loop.
  size_t total = entry.size;
  if (total == 0) total = s.contentLength();

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

  // Retry budget: a 4 MB asset over conference WiFi typically completes
  // on the first attempt, but a single transient stall (5-10 s of dead
  // air, AP roaming, neighbour interference) used to abort the whole
  // download. Resume-with-Range lets each retry pick up at `written`
  // bytes instead of restarting the multi-megabyte stream from scratch.
  // Four retries on top of the initial attempt cover the typical
  // single-stall case without dragging out a genuinely dead network.
  constexpr int kMaxRetries = 4;
  int retries = 0;

  // 8 KB chunks ≈ 8x fewer iterations than the historic 1 KB buffer.
  // Each iteration pays for an HTTPClient::available poll, an
  // f_write call (FATFS sector bookkeeping), an mbedtls_sha256_update
  // (HW-accelerated on S3 but per-call overhead still matters), and a
  // progress-callback check. At 4 MB this is the difference between
  // ~33 KB/s and ~250 KB/s on a healthy WiFi link. Heap-allocated so
  // we don't eat the main-loop's 24 KB stack on the AssetLibraryScreen
  // call path; PSRAM is preferred but internal heap is fine too.
  constexpr size_t kChunkBytes = 8192;
  uint8_t* chunk = static_cast<uint8_t*>(std::malloc(kChunkBytes));
  if (!chunk) {
    setError("chunk alloc failed");
    f_close(&fil);
    f_unlink(fs, tmpPath);
    if (wantSha) mbedtls_sha256_free(&shaCtx);
    if (cb) {
      AssetProgress p{0, 0, true, AssetInstallResult::kFsWriteError};
      cb(p, user);
    }
    return false;
  }
  size_t written = 0;
  uint32_t lastReport = 0;
  bool ok = true;
  while (true) {
    int got = s.read(chunk, kChunkBytes);
    const bool isShortEof =
        (got == 0 && total > 0 && written < total);
    if (got < 0 || isShortEof) {
      // Transient: stream dropped or the per-read window expired
      // before the server delivered the rest. Reopen with a Range
      // header at the current write offset and continue. Servers that
      // honour the header reply 206 Partial Content (Stream::open
      // accepts both 200 and 206); servers that ignore it reply 200
      // and we restart the file from scratch as a fallback.
      if (retries++ >= kMaxRetries) {
        char buf[80];
        std::snprintf(buf, sizeof(buf),
                      "stream short %u/%u after %d retries",
                      (unsigned)written, (unsigned)total,
                      retries - 1);
        setError(buf);
        ok = false;
        break;
      }
      Serial.printf(
          "[registry] stream stall at %u/%u, retry %d (resume)\n",
          (unsigned)written, (unsigned)total, retries);
      s.close();
      // Linear backoff: 500 ms, 1 s, 1.5 s, 2 s. Long enough that a
      // briefly overwhelmed AP can recover, short enough that an
      // attentive user doesn't think the install is stuck.
      delay(500U * static_cast<uint32_t>(retries));
      if (!s.open(entry.url, 30000, written)) {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "resume open failed: %s",
                      s.lastError());
        setError(buf);
        ok = false;
        break;
      }
      if (s.httpCode() == 200 && written > 0) {
        // Server ignored Range and is sending the whole file from
        // byte 0. Easiest recovery: rewind the .tmp file, reset the
        // SHA context, and consume the stream as a fresh start. This
        // wastes the bytes we already wrote, but jsDelivr / GitHub
        // CDN / ibiblio all honour Range so the path is rare in
        // practice.
        Serial.printf(
            "[registry] server ignored Range; restarting from 0\n");
        f_lseek(&fil, 0);
        f_truncate(&fil);
        written = 0;
        if (wantSha) {
          mbedtls_sha256_free(&shaCtx);
          mbedtls_sha256_init(&shaCtx);
          mbedtls_sha256_starts(&shaCtx, 0);
        }
      }
      continue;
    }
    if (got == 0) break;  // genuine EOF
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
  std::free(chunk);

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
