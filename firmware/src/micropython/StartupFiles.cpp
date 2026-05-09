#include "StartupFiles.h"

#include <Arduino.h>
#include <cstring>

extern "C" {
#include "lib/oofatfs/ff.h"
FATFS* replay_get_fatfs(void);
}

#include "../infra/Filesystem.h"
#include "StartupFilesData.h"

// ── FNV-1a hash (matches the Python generator) ─────────────────────────────

static uint32_t fnv1a32(const uint8_t* data, size_t len) {
    uint32_t h = 0x811C9DC5;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x01000193;
    }
    return h;
}

static bool fnv1a32_file(FATFS* fs, const char* path, uint32_t& outHash) {
    FIL fil;
    if (f_open(fs, &fil, path, FA_READ) != FR_OK) return false;

    uint32_t h = 0x811C9DC5;
    uint8_t buf[256];
    UINT bytesRead;

    for (;;) {
        if (f_read(&fil, buf, sizeof(buf), &bytesRead) != FR_OK) {
            f_close(&fil);
            return false;
        }
        if (bytesRead == 0) break;
        for (UINT i = 0; i < bytesRead; i++) {
            h ^= buf[i];
            h *= 0x01000193;
        }
    }

    f_close(&fil);
    outHash = h;
    return true;
}

// ── File helpers ────────────────────────────────────────────────────────────

static bool writeFile(FATFS* fs, const char* path, const char* data, uint32_t len) {
    f_unlink(fs, path);

    FIL fil;
    if (f_open(fs, &fil, path, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        Serial.printf("[startup] FAILED to create %s\n", path);
        return false;
    }

    UINT written = 0;
    FRESULT res = f_write(&fil, data, len, &written);
    f_sync(&fil);
    f_close(&fil);

    // FATFS returns FR_OK with written < len when the volume is full.
    // Treat any short write as a failure AND clean up the dirent so we
    // don't leave a 0-byte ghost behind that later boots will see as
    // "user-modified" via the historical-hash check. Without this, a
    // single out-of-space write at first boot poisons every subsequent
    // boot's view of the file ("file exists, hash unknown → preserve
    // user edits → never overwrite") and Python apps fail forever with
    // `can't import name run_app`.
    if (res != FR_OK || written != len) {
        Serial.printf("[startup] FAILED to write %s (%u/%u, fr=%d)%s\n",
                      path, written, len, (int)res,
                      (res == FR_OK && written != len) ? " (NO SPACE)" : "");
        f_unlink(fs, path);
        return false;
    }
    return true;
}

static bool fileExists(FATFS* fs, const char* path) {
    FILINFO fno;
    return f_stat(fs, path, &fno) == FR_OK;
}

// Returns true and fills outSize when the file exists. False on stat
// failure (treated as "unknown"). Used by the empty-file recovery
// path in provisionStartupFiles.
static bool fileSize(FATFS* fs, const char* path, uint32_t& outSize) {
    FILINFO fno;
    if (f_stat(fs, path, &fno) != FR_OK) return false;
    outSize = static_cast<uint32_t>(fno.fsize);
    return true;
}

static void ensureDir(FATFS* fs, const char* path) {
    if (!fileExists(fs, path)) {
        f_mkdir(fs, path);
    }
}

// ── Upstream-changed marker for user-modified text files ───────────────────
//
// When a file in the startup set has been edited on-badge AND its upstream
// (firmware-bundled) version has new contents, we don't overwrite the user's
// edits.  Instead we prepend a one-line comment so the user knows newer
// upstream content exists.  Idempotent: if the marker substring is already
// at the top, we leave the file alone.

static const char kUpstreamMarkerNeedle[] =
    "wasn't reloaded to preserve your edits";

static const char kUpstreamMarkerPy[] =
    "#This file has upstream changes but wasn't reloaded to preserve your "
    "edits. If you want the newest version, delete this file and reboot "
    "and it will be loaded at startup\n";

static const char kUpstreamMarkerMd[] =
    "<!-- This file has upstream changes but wasn't reloaded to preserve "
    "your edits. If you want the newest version, delete this file and "
    "reboot and it will be loaded at startup -->\n";

static bool pathHasExt(const char* path, const char* ext) {
    size_t pl = strlen(path);
    size_t el = strlen(ext);
    if (el > pl) return false;
    return strcasecmp(path + pl - el, ext) == 0;
}

// True for text formats we feel safe prepending a comment to.  We
// deliberately exclude .json (no comment syntax), all binary blobs, and
// files marked PROTECTED (caller already short-circuits those, but the
// guard here is a belt-and-braces check).
static bool isUpstreamMarkerEligible(const StartupFileInfo& f) {
    if (f.flags & STARTUP_FILE_PROTECTED) return false;
    static const char* kBinaryExts[] = {
        ".json", ".bin", ".bmp", ".png", ".jpg", ".jpeg", ".gif",
        ".ico", ".raw", ".pbm", ".xbm", ".fb", ".wad",
    };
    for (const char* ext : kBinaryExts) {
        if (pathHasExt(f.path, ext)) return false;
    }
    return true;
}

static const char* upstreamMarkerForPath(const char* path) {
    if (pathHasExt(path, ".md")) return kUpstreamMarkerMd;
    return kUpstreamMarkerPy;
}

// True iff the first ~512 bytes of <path> already contain the marker
// substring.  Avoids re-stamping a file every boot.
static bool hasUpstreamMarker(FATFS* fs, const char* path) {
    FIL fil;
    if (f_open(fs, &fil, path, FA_READ) != FR_OK) return false;
    char head[512];
    UINT n = 0;
    FRESULT res = f_read(&fil, head, sizeof(head) - 1, &n);
    f_close(&fil);
    if (res != FR_OK || n == 0) return false;
    head[n] = '\0';
    return strstr(head, kUpstreamMarkerNeedle) != nullptr;
}

// Read existing file, prepend the marker comment, rewrite atomically
// (write to .tmp, then unlink + rename).  Returns true on success.
static bool stampUpstreamMarker(FATFS* fs, const char* path) {
    if (hasUpstreamMarker(fs, path)) return false;

    FIL fil;
    if (f_open(fs, &fil, path, FA_READ) != FR_OK) return false;
    UINT existingLen = f_size(&fil);

    const char* marker = upstreamMarkerForPath(path);
    size_t markerLen = strlen(marker);
    size_t totalLen = markerLen + existingLen;

    char* buf = (char*)malloc(totalLen);
    if (!buf) {
        f_close(&fil);
        Serial.printf("[startup] mark %s: alloc %u failed\n",
                      path, (unsigned)totalLen);
        return false;
    }

    memcpy(buf, marker, markerLen);
    UINT bytesRead = 0;
    FRESULT rres = (existingLen > 0)
        ? f_read(&fil, buf + markerLen, existingLen, &bytesRead)
        : FR_OK;
    f_close(&fil);

    if (rres != FR_OK || bytesRead != existingLen) {
        free(buf);
        Serial.printf("[startup] mark %s: read fr=%d got=%u/%u\n",
                      path, (int)rres, (unsigned)bytesRead,
                      (unsigned)existingLen);
        return false;
    }

    char tmpPath[160];
    int n = snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof(tmpPath)) {
        free(buf);
        return false;
    }

    f_unlink(fs, tmpPath);
    FIL out;
    if (f_open(fs, &out, tmpPath, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        free(buf);
        Serial.printf("[startup] mark %s: open tmp failed\n", path);
        return false;
    }
    UINT written = 0;
    FRESULT wres = f_write(&out, buf, totalLen, &written);
    f_sync(&out);
    f_close(&out);
    free(buf);

    if (wres != FR_OK || written != totalLen) {
        f_unlink(fs, tmpPath);
        Serial.printf("[startup] mark %s: write fr=%d %u/%u\n",
                      path, (int)wres, (unsigned)written,
                      (unsigned)totalLen);
        return false;
    }

    f_unlink(fs, path);
    if (f_rename(fs, tmpPath, path) != FR_OK) {
        f_unlink(fs, tmpPath);
        Serial.printf("[startup] mark %s: rename failed\n", path);
        return false;
    }
    return true;
}

// ── Force-sync: remove files not in the startup set ─────────────────────────

static bool isInStartupSet(const char* fullPath) {
    for (int i = 0; i < kStartupFileCount; i++) {
        if (strcmp(fullPath, kStartupFiles[i].path) == 0) return true;
    }
    return false;
}

static void cleanManagedDir(FATFS* fs, const char* dirPath) {
    FF_DIR dir;
    FILINFO fno;

    if (f_opendir(fs, &dir, dirPath) != FR_OK) return;

    char fullPath[128];
    char toDelete[20][128];
    int deleteCount = 0;

    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0') {
        if (fno.fattrib & AM_DIR) continue;

        snprintf(fullPath, sizeof(fullPath), "%s/%s", dirPath, fno.fname);

        if (!isInStartupSet(fullPath) && deleteCount < 20) {
            strncpy(toDelete[deleteCount], fullPath, sizeof(toDelete[0]) - 1);
            toDelete[deleteCount][sizeof(toDelete[0]) - 1] = '\0';
            deleteCount++;
        }
    }
    f_closedir(&dir);

    for (int i = 0; i < deleteCount; i++) {
        Serial.printf("[startup] Removing extra file: %s\n", toDelete[i]);
        f_unlink(fs, toDelete[i]);
    }
}

// ── Main provisioning logic ─────────────────────────────────────────────────

void provisionStartupFiles(bool forceSync) {
    if (kStartupFileCount == 0) return;

    // Pre-flight: detect a wedged FAT (free space tiny relative to
    // the partition). Common cause: partitions.csv resized but the
    // on-disk FAT still references the old smaller layout, so writes
    // silently truncate to 0 bytes. Reformat once and re-enter via
    // formatAndReprovisionFFat → provisionStartupFiles(true). Skip in
    // forceSync so the explicit-action path stays predictable. Take
    // the lock in a sub-scope so it's released before we recurse into
    // formatAndReprovisionFFat() (which takes its own lock).
    bool needsReformat = false;
    if (!forceSync) {
        Filesystem::IOLock probeLock;
        FATFS* probeFs = replay_get_fatfs();
        if (probeFs != nullptr) {
            DWORD freeClusters = 0;
            if (f_getfree(probeFs, &freeClusters) == FR_OK) {
                const uint32_t bytesPerCluster =
                    static_cast<uint32_t>(probeFs->csize) * FF_MAX_SS;
                const uint64_t totalBytes =
                    static_cast<uint64_t>(probeFs->n_fatent - 2) *
                    bytesPerCluster;
                const uint64_t freeBytes =
                    static_cast<uint64_t>(freeClusters) * bytesPerCluster;
                if (totalBytes >= (1u << 20) && freeBytes < (16u << 10)) {
                    Serial.printf(
                        "[startup] FAT wedged (free=%u B / total=%u B); "
                        "auto-reformatting\n",
                        static_cast<unsigned>(freeBytes),
                        static_cast<unsigned>(totalBytes));
                    needsReformat = true;
                }
            }
        }
    }
    if (needsReformat) {
        formatAndReprovisionFFat();
        return;
    }

    // Wrap the entire provisioning pass in one IOLock — many f_open /
    // f_write / f_unlink calls in sequence, and we don't want any other
    // journal task interleaving inside FATFS while we're churning the
    // root directory.  The lock is recursive so the static helpers
    // (writeFile/fileExists/etc.) which never re-lock are fine.
    Filesystem::IOLock fsLock;

    FATFS* fs = replay_get_fatfs();
    if (fs == nullptr) {
        Serial.println("[startup] FAT not mounted, skipping file provisioning");
        return;
    }

    // Ensure managed directories exist.
    for (int i = 0; i < kStartupDirCount; i++) {
        ensureDir(fs, kStartupDirs[i]);
    }

    int created = 0, updated = 0, skipped = 0;

    for (int i = 0; i < kStartupFileCount; i++) {
        const StartupFileInfo& f = kStartupFiles[i];
        bool isProtected = (f.flags & STARTUP_FILE_PROTECTED);
        bool exists = fileExists(fs, f.path);

        if (isProtected && exists) {
            skipped++;
            continue;
        }

        if (!exists) {
            if (writeFile(fs, f.path, f.content, f.contentLen)) {
                Serial.printf("[startup] Created %s\n", f.path);
                created++;
            }
            continue;
        }

        // File exists. Decide whether to overwrite.
        if (forceSync) {
            if (writeFile(fs, f.path, f.content, f.contentLen)) {
                Serial.printf("[startup] Force-synced %s\n", f.path);
                updated++;
            }
            continue;
        }

        // Empty-file recovery: nobody intentionally edits a startup
        // file down to zero bytes when the upstream has real content.
        // Without this branch, a corrupted (0-byte) on-FAT file's
        // FNV-1a hash equals the seed (0x811C9DC5), which isn't in
        // any known-historical-default list, so the "user-modified"
        // path stamps the upstream marker and skips the overwrite —
        // leaving the badge stuck with empty libs (`from badge_app
        // import run_app` then fails on every Python app launch).
        // Detect that case here and treat it as missing instead.
        uint32_t diskSize = 0;
        if (f.contentLen > 0 && fileSize(fs, f.path, diskSize) &&
            diskSize == 0) {
            if (writeFile(fs, f.path, f.content, f.contentLen)) {
                Serial.printf("[startup] Recovered empty %s\n", f.path);
                updated++;
            }
            continue;
        }

        // Normal mode: 3-way hash check (same logic as JumperlOS).
        uint32_t currentHash = (f.hashCount > 0) ? f.knownHashes[0] : 0;
        uint32_t diskHash = 0;
        bool hashed = fnv1a32_file(fs, f.path, diskHash);

        if (hashed && diskHash == currentHash) {
            skipped++;
            continue;
        }

        bool isOldFirmwareDefault = false;
        if (hashed) {
            for (int h = 1; h < f.hashCount; h++) {
                if (diskHash == f.knownHashes[h]) {
                    isOldFirmwareDefault = true;
                    break;
                }
            }
        }

        if (isOldFirmwareDefault) {
            if (writeFile(fs, f.path, f.content, f.contentLen)) {
                Serial.printf("[startup] Updated %s (old firmware default)\n", f.path);
                updated++;
            }
        } else {
            // User-modified file with upstream changes.  Preserve the
            // user's edits but stamp a one-line comment at the top so
            // they know a newer upstream version exists.  Skipped for
            // protected, JSON, and binary files (no safe comment
            // syntax / no point editing blobs).
            if (isUpstreamMarkerEligible(f)) {
                if (stampUpstreamMarker(fs, f.path)) {
                    Serial.printf("[startup] Marked %s (upstream changed; user edits preserved)\n",
                                  f.path);
                }
            }
            skipped++;
        }
    }

    if (forceSync) {
        for (int i = 0; i < kStartupDirCount; i++) {
            cleanManagedDir(fs, kStartupDirs[i]);
        }
    }

    // doom1.wad is shipped via `pio run -t uploadfs` (it's too large to
    // embed in the firmware image alongside the factory app), so it
    // never appears in kStartupFiles and we can't recreate it here.
    // Warn loudly if it's missing so the operator knows to re-uploadfs.
    if (!fileExists(fs, "/doom1.wad")) {
        Serial.println("[startup] WARNING: /doom1.wad missing — Doom will not launch. "
                       "Re-run `pio run -e <env> -t uploadfs` to restore it.");
    }

    if (created > 0 || updated > 0) {
        Serial.printf("[startup] Provisioned: %d created, %d updated, %d unchanged\n",
                      created, updated, skipped);
    }
}

// ── Emergency FAT reformat ─────────────────────────────────────────────────
//
// In-place `f_mkfs` against the same FATFS object that
// replay_vfs_mount_fat() registered with MicroPython. We don't unmount
// from MicroPython's VFS — we just ask the FATFS driver to lay down a
// fresh super-floppy filesystem on top of the wear-levelled block
// device, then re-call f_mount on the same FATFS struct. After the
// reformat we synchronously reprovision the embedded startup files so
// the badge has a usable /lib + /apps tree before returning.
//
// Only the ffat partition is touched; NVS-backed state (badge UID,
// HMAC secret, contacts, menu order) is preserved.
bool formatAndReprovisionFFat() {
    // Hold the I/O lock for the entire operation — the boops/journal
    // tasks must not interleave a write between our unmount and the
    // mkfs.
    Filesystem::IOLock fsLock;

    FATFS* fs = replay_get_fatfs();
    if (fs == nullptr) {
        Serial.println("[startup] formatAndReprovisionFFat: FAT not mounted");
        return false;
    }

    Serial.println("[startup] reformatting ffat partition…");

    // Working buffer for f_mkfs. 4 KiB matches the original mount path
    // in replay_bdev.c; smaller buffers fail on FAT32-eligible volumes.
    constexpr size_t kWorkBufSize = 4096;
    uint8_t* working_buf = static_cast<uint8_t*>(malloc(kWorkBufSize));
    if (!working_buf) {
        Serial.println("[startup] formatAndReprovisionFFat: malloc 4 KiB failed");
        return false;
    }

    // Unmount the FATFS struct so f_mkfs can take it. f_mount(NULL,
    // path, …) would be the upstream incantation; here we just clear
    // the volume's mount flag by remounting onto the same struct after
    // mkfs. f_mkfs operates on the FATFS struct directly via its
    // ->drv pointer (which our bdev layer set up).
    FRESULT res = f_mkfs(fs, FM_ANY | FM_SFD, 0, working_buf, kWorkBufSize);
    free(working_buf);
    if (res != FR_OK) {
        Serial.printf("[startup] f_mkfs failed: %d\n", (int)res);
        return false;
    }

    // Re-attach the freshly-formatted FATFS to its driver so subsequent
    // f_open/f_write calls hit the new on-disk structure.
    res = f_mount(fs);
    if (res != FR_OK) {
        Serial.printf("[startup] post-mkfs f_mount failed: %d\n", (int)res);
        return false;
    }

    Serial.println("[startup] ffat reformatted; reprovisioning embedded files…");

    // Force-sync because every embedded file is now "missing" — but
    // forceSync also rewalks the dir tree to remove stale entries
    // (none after reformat) and is the one path that doesn't depend on
    // the historical-hash logic we're trying to escape from.
    provisionStartupFiles(/*forceSync=*/true);
    Serial.println("[startup] formatAndReprovisionFFat: done");
    return true;
}
