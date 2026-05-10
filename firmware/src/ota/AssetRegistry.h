// AssetRegistry.h — Generic file fetcher driven by a remote registry.json.
//
// Lets the badge install user-facing asset files (DOOM WAD, sound packs,
// fonts, anything else) on demand from a configurable URL set in
// `settings.txt` (`community_apps_url`, formerly `asset_registry_url`).
//
// Schema v2 (registry/community_apps.json) supports two entry kinds:
// single files (`kind: "file"`) and multi-file app bundles
// (`kind: "app"`). The on-badge install path currently implements
// kind:"file"; kind:"app" handling is a follow-up.
//
//   {
//     "schema_version": 2,
//     "assets": [
//       {
//         "id": "doom1-shareware",
//         "kind": "file",
//         "name": "DOOM 1 Shareware WAD",
//         "version": "1.9",
//         "url": "https://.../doom1.wad",
//         "sha256": "<hex>",            // optional, corruption check only
//         "size": 4196020,
//         "dest_path": "/doom1.wad",
//         "min_free_bytes": 4500000,    // optional
//         "description": "..."
//       },
//       {
//         "id": "tardigotchi",
//         "kind": "app",
//         "name": "Tardigotchi",
//         "version": "<sha256-stem>",
//         "dest_dir": "/apps/tardigotchi",
//         "size": 33744,
//         "description": "...",
//         "files": [                     // inlined; no separate manifest.json
//           {"path": "/main.py",   "size": 187,
//            "sha256": "...", "url": "https://..."},
//           {"path": "/engine.py", "size": 24006,
//            "sha256": "...", "url": "https://..."}
//         ]
//       }
//     ]
//   }
//
// Per-asset state (installed version) is persisted in NVS under
// namespace `badge_assets` keyed by asset id. Single-file installs
// stream into `<dest_path>.tmp` and atomic-rename; on failure the
// live file is left untouched.

#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

namespace ota {

constexpr uint8_t kMaxRegistryAssets = 48;
constexpr size_t kAssetIdMax = 32;
constexpr size_t kAssetNameMax = 48;
constexpr size_t kAssetVersionMax = 16;
constexpr size_t kAssetUrlMax = 256;
constexpr size_t kAssetPathMax = 64;
constexpr size_t kAssetSha256Max = 72;  // hex chars, NUL
constexpr size_t kAssetDescMax = 96;

struct AssetEntry {
  char id[kAssetIdMax];
  char name[kAssetNameMax];
  char version[kAssetVersionMax];
  char url[kAssetUrlMax];
  char sha256[kAssetSha256Max];   // empty if not provided
  char dest_path[kAssetPathMax];
  char description[kAssetDescMax];
  uint32_t size;                  // 0 if registry omitted it
  uint32_t min_free_bytes;        // 0 if not specified
};

enum class AssetStatus : uint8_t {
  kNotInstalled,
  kInstalled,
  kUpdateAvailable,
  kFailed,           // last install attempt errored
};

enum class RegistryRefresh : uint8_t {
  kOk,
  kCooldownActive,
  kNetworkError,
  kParseError,
  kNotConfigured,    // asset_registry_url is empty
};

enum class AssetInstallResult : uint8_t {
  kOk,
  kDestPathInvalid,
  kInsufficientSpace,
  kHttpError,
  kFsWriteError,
  kSha256Mismatch,
  kAborted,
};

struct AssetProgress {
  size_t bytesWritten;
  size_t totalBytes;
  bool done;
  AssetInstallResult result;  // valid when done=true
};

using AssetProgressCb = void (*)(const AssetProgress&, void* user);

namespace registry {

void begin();

// Daily-cadence trigger; cheap when nothing to do.
void tick();

// Manual refresh from the Asset Library screen. Synchronous.
RegistryRefresh refresh(bool ignoreCooldown);

size_t count();
const AssetEntry* at(size_t index);
const AssetEntry* findById(const char* id);

AssetStatus statusOf(const AssetEntry& entry);
const char* installedVersionOf(const AssetEntry& entry);

bool install(const AssetEntry& entry, AssetProgressCb cb, void* user);
bool remove(const AssetEntry& entry);

const char* lastErrorMessage();
time_t lastRefreshEpoch();

}  // namespace registry

}  // namespace ota
