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

// Reformat the ffat partition in place (`f_mkfs(FM_ANY | FM_SFD)`)
// and immediately reprovision the embedded startup files. Used when
// the on-device FAT is wedged (e.g. reports 0 free bytes despite the
// partition being mostly empty — usually a stale FAT structure left
// behind by an older partitions.csv layout). Loses all on-FAT files
// (settings.txt, /apps/<slug>/save.json, /tardigrade_save.json, etc.);
// NVS state is untouched. Returns true on success.
bool formatAndReprovisionFFat();
