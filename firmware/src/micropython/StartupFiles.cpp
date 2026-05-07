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

    if (res != FR_OK || written != len) {
        Serial.printf("[startup] FAILED to write %s (%u/%u)\n", path, written, len);
        return false;
    }
    return true;
}

static bool fileExists(FATFS* fs, const char* path) {
    FILINFO fno;
    return f_stat(fs, path, &fno) == FR_OK;
}

static void ensureDir(FATFS* fs, const char* path) {
    if (!fileExists(fs, path)) {
        f_mkdir(fs, path);
    }
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
            skipped++;
        }
    }

    if (forceSync) {
        for (int i = 0; i < kStartupDirCount; i++) {
            cleanManagedDir(fs, kStartupDirs[i]);
        }
    }

    if (created > 0 || updated > 0) {
        Serial.printf("[startup] Provisioned: %d created, %d updated, %d unchanged\n",
                      created, updated, skipped);
    }
}
