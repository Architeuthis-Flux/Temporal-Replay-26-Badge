#pragma once

// Provision files from the embedded initial_filesystem/ snapshot.
//
// Normal mode (forceSync=false):
//   - Creates any missing files
//   - Overwrites files that match a known older firmware hash (unmodified defaults)
//   - Leaves user-modified files untouched
//
// Force-sync mode (forceSync=true):
//   - Overwrites every file to match initial_filesystem/
//   - Removes files from managed directories that aren't in initial_filesystem/
//
// Call once after the FAT filesystem is mounted (after mpy_start).

void provisionStartupFiles(bool forceSync = false);
