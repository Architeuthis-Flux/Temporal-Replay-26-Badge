# OTA, Community Apps & Filesystem Sync — Maintainer Guide

This guide explains what a maintainer needs to do to keep firmware
OTA, the Community Apps registry, and the diff-sync engine working
for badges in the field. The three systems share this doc because
they share the same HTTP + SHA-256 transport plumbing, but they're
independent — none of them depends on the others.

> **Heads up**: the registry was renamed from "Asset Library" to
> "Community Apps" in firmware v0.2. The old `asset_registry_url`
> setting + `registry/registry.json` v1 schema still work; new
> badges fetch `registry/community_apps.json` (schema v2) by
> default. See § 4 below for the migration story.

> **Storage model preflight**: read
> [`STORAGE-MODEL.md`](STORAGE-MODEL.md) before changing anything in
> this doc — it explains which tier (NVS / FATFS / app0) each kind
> of flash touches, and which user state survives where.

> **Audience**: you are publishing firmware releases or asset files
> for a fleet of Temporal Replay 2026 badges. Badge users do not need
> any of this; they just need WiFi.

> **Security stance**: this is open-source firmware with no secrets to
> protect. There is no image signing, no certificate pinning, no
> auth. SHA-256 hashes are corruption checks only. The threat model is
> "don't brick the badge", which is mitigated by a battery guard and
> bootloader rollback — not by cryptography. If you want a hardened
> fleet, you'll need to fork the OTA module and bring your own keys.

---

## 1. How firmware OTA works end-to-end

```
┌──────────────────────────────────────────────────────┐
│ Maintainer cuts a GitHub release tag, e.g. v0.2.0.   │
└──────────────────────────────────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────────┐
│ release-firmware.yml runs:                           │
│   pip install platformio                             │
│   pio run -e echo                                    │
│   upload firmware.bin -> firmware.bin release asset  │
└──────────────────────────────────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────────┐
│ Badge (any time after, on WiFi):                     │
│   GET api.github.com/repos/<owner>/<repo>/           │
│       releases/latest                                │
│   Compare tag_name vs FIRMWARE_VERSION (semver).     │
│   If newer + matching asset present → cache it.      │
│   Status bar shows down-arrow glyph.                 │
│   Home tile flips to "UPDATE".                       │
└──────────────────────────────────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────────┐
│ User opens "FW UPDATE" tile → Confirm.               │
│ Badge streams the .bin into the inactive OTA slot,   │
│ ESP.restart()s, and the bootloader picks the new     │
│ image. If the new image fails to boot the bootloader │
│ rolls back automatically.                            │
└──────────────────────────────────────────────────────┘
```

### The naming contract

The badge looks for an asset whose `name` field exactly matches the
`OTA_ASSET_NAME` define in `firmware/platformio.ini`. The default is
`firmware.bin` — same as the local PlatformIO build product, so
JumperIDE and other external tooling can point at the release URL
directly without renaming. If the release has no asset with that
exact name, the badge silently reports `NoMatchingAsset` and the
indicator stays dark — that's the safe failure mode.

### What the version comparison does

`BadgeOTA::compareSemver()` parses both tags as `<major>.<minor>.<patch>`
(stripping a leading `v` if present). Pre-release suffixes like
`-rc1` are ignored — `1.2.3` and `1.2.3-rc1` compare equal. If you
need stricter pre-release ordering, edit `parseSemver` in
`firmware/src/ota/BadgeOTA.cpp`.

### Releasing a new firmware version

1. Bump `firmware/VERSION`:
   ```
   cd firmware && ./scripts/bump_version.sh 0.2.0
   ```
   (or just edit the file by hand).
2. Commit and push.
3. Create a GitHub release tagged `0.2.0` (the leading `v` is
   optional — both work).
4. The `release-firmware.yml` Action runs automatically; within a
   few minutes the release will have a `firmware.bin` asset.
5. Badges in the field will pick it up the next time they hit their
   24h check, or immediately if a user opens **FW UPDATE → Check now**.

If something goes wrong with the Action:

- Check the **Actions** tab on GitHub for failure details.
- You can re-run the Action manually from `workflow_dispatch` —
  pass the existing tag name.
- As a last-ditch fallback, build locally and drop the `.bin` onto
  the release manually:
  ```
  cd firmware && pio run -e echo
  # then upload .pio/build/echo/firmware.bin via the GitHub release
  # UI (must be uploaded with the literal name "firmware.bin").
  ```

### Forking the firmware (different repo / different asset name)

In your fork, edit `firmware/platformio.ini` `[env:echo] build_flags`:

```ini
'-DOTA_GITHUB_REPO="YourOrg/YourBadgeFork"'
'-DOTA_ASSET_NAME="firmware-yourfork.bin"'
```

Update the Action to upload an asset with the matching name. Done.

---

## 2. Asset Registry (DOOM WAD, etc.)

The asset registry is a single JSON file (`registry/registry.json`)
that lists files the badge can fetch on demand. Schema docs are in
[`registry/README.md`](../../registry/README.md).

### Default hosting (no infrastructure)

The firmware's `settings.txt.example` ships with:

```
asset_registry_url = https://raw.githubusercontent.com/Architeuthis-Flux/Temporal-Replay-26-Badge/main/registry/registry.json
```

Anyone with PR access can add an entry. Badges pick up changes within
24 hours of their next refresh.

### Where the actual asset bytes live

There are two normal hosting paths, in order of preference:

1. **GitHub Release attachments via the release-assets pipeline**
   (recommended for files ≳ 100 KB — DOOM WAD lives here). The repo's
   [`release-assets/manifest.json`](../../release-assets/manifest.json)
   declares which files the release Action attaches to every GitHub
   release; the registry's `url` field then points at the stable
   `https://github.com/<owner>/<repo>/releases/latest/download/<name>`
   pattern, which automatically resolves to whatever was attached to
   the most recent release. See
   [`release-assets/README.md`](../../release-assets/README.md) for
   the manifest schema and add-a-new-asset workflow.
2. **External HTTPS host** (Cloudflare R2, your own server, etc.) for
   things you don't want in the GitHub repo at all. See
   [Hosting on Cloudflare](#3-hosting-on-cloudflare) below.

### Adding a new asset (release-bundled path)

1. Drop the file at a sensible local path (or note a `fallback_url`
   that CI can curl). For files committed to the repo, anywhere works
   — for things like the DOOM WAD that we don't want in git history,
   just add a `.gitignore` line pointing at the local copy.
2. Compute the SHA-256:
   ```
   sha256sum path/to/file.bin
   ```
3. Append an entry to
   [`release-assets/manifest.json`](../../release-assets/manifest.json):
   ```json
   {
     "name": "file.bin",
     "local_path": "firmware/data/file.bin",
     "fallback_url": "",
     "registry_id": "my-asset"
   }
   ```
4. Append the matching entry to `registry/registry.json` with
   `url` set to the stable release URL pattern:
   ```json
   {
     "id": "my-asset",
     "name": "My Asset",
     "version": "1.0",
     "url": "https://github.com/Architeuthis-Flux/Temporal-Replay-26-Badge/releases/latest/download/file.bin",
     "sha256": "<hex from step 2>",
     "size": 1234567,
     "dest_path": "/my_asset.bin",
     "min_free_bytes": 1500000,
     "description": "What this is and why I'd want it"
   }
   ```
5. Verify locally before pushing:
   ```
   python3 scripts/stage_release_assets.py --check
   ```
   The script confirms the manifest's files match the registry's
   `sha256` / `size` / `url` fields. Drift = build failure with a
   clear "you forgot to update X" message.
6. PR + merge + cut a release. The release Action attaches the file
   to the release alongside `firmware.bin`. Badges in the field see
   the new entry within 24 h of the next refresh.

### Adding a new asset (external-host path)

Same as the release-bundled path, except step 3 is omitted, the
`url` in step 4 points at your external host, and the release Action
doesn't have to do anything. Use this for files you don't want in the
GitHub repo or that change on a different cadence than firmware
releases.

### Pushing an updated version of an existing asset

Bump the `version` field on the existing registry entry. If the
file's bytes changed, also bump the `sha256` and `size`. The release
Action's `--check` step will catch drift between the file and the
registry on the next release. Badges that already have the asset
installed see the entry flip to "Update" and the user can re-install
over the top.

---

## 3. Hosting on Cloudflare

Cloudflare's free tier covers everything we need: a static
`registry.json` and large binary asset downloads, both with global
edge caching. You have two reasonable options.

### Option A: R2 (object storage) — best for big files

R2 is S3-compatible object storage with **no egress fees**. The
DOOM WAD is 4 MB and a fleet of badges hitting it weekly is well
under a free-tier quota.

1. **Create an R2 bucket.** In the Cloudflare dashboard:
   `R2 → Create bucket → name it e.g. `temporal-badge-assets``.
2. **Make it publicly readable.** R2 buckets are private by default.
   Either:
   - Connect a custom domain (`assets.example.com`) under the bucket
     settings → "Public access". Cloudflare will issue a TLS cert
     automatically and serve the bucket at
     `https://assets.example.com/<key>`.
   - Or enable the auto-generated `r2.dev` subdomain (good for
     testing, has rate limits).
3. **Upload the asset.** Drag a file into the bucket UI, or use
   `wrangler` / `rclone` / any S3-compatible client. The file's
   public URL is `https://<your-domain>/<filename>`.
4. **Update `registry.json`.** Point the entry's `url` at the public
   URL.
5. **(Optional) Configure cache headers.** In the R2 bucket's
   "Settings → Public access → Edit CORS" you can also set
   `Cache-Control` policies. The default 4-hour edge cache is fine
   for assets that change rarely.

### Option B: Cloudflare Pages — best for the registry JSON itself

Pages hosts a static site directly from a Git repo, so you can
serve `registry.json` from a repo that's separate from the firmware
if you want assets to update without touching firmware history.

1. **Create a new repo** with just `registry.json` (and any other
   static files you want to expose).
2. **Cloudflare dashboard** → `Pages → Create project → connect to
   Git → pick your repo`. Build command: leave blank. Output
   directory: `.` (or `/`).
3. **Pages will deploy on every push** to the configured branch.
   The default URL is `<project>.pages.dev`; you can attach a
   custom domain under `Pages → your project → Custom domains`.
4. **Set the badge `asset_registry_url`** (in `settings.txt`) to
   `https://<your-pages-host>/registry.json`.

### Option C: Workers — for advanced routing

If you want per-badge personalization, A/B rollouts, or analytics,
write a Cloudflare Worker that proxies the registry and inspects
the badge's `User-Agent` (`TemporalBadge-OTA/1.0`). Outside the
scope of this doc — see Cloudflare's Workers tutorials.

### A note on caching

Both R2 (with public access) and Pages serve files through
Cloudflare's edge cache by default. If you push a new
`registry.json` and the badge is still seeing the old one:

- **Pages:** purge the project's cache (`Pages → project →
  Caching → Purge cache`) or wait for the default TTL (~30 min).
- **R2 with custom domain:** add a `Cache-Control: max-age=300`
  header at upload time, or use Cloudflare's
  "Cache Rules → Bypass" for that specific URL while iterating.
- The badge ignores HTTP cache headers — it has its own 24h
  cooldown — so the cache only matters when a user hits
  "Refresh" from the Asset Library screen.

### Verifying it works

From any browser:

```
curl -i https://your-host/registry.json
```

Should return `200 OK`, `Content-Type: application/json`, and the
JSON body. If the badge is still ignoring it, check on-device serial
logs for `[registry]` lines — they include the URL fetched and the
parse result.

---

## 4. Bootstrapping the very first release

The current 0.1.4 release predates this OTA work and won't have a
`firmware.bin` asset. To get badges actually upgrading:

1. **Either** cut a 0.1.5+ tag — the new Action will publish the
   `.bin` automatically.
2. **Or** manually attach `firmware.bin` to the existing 0.1.4
   release via the GitHub web UI.

Until one of those happens, badges on 0.1.4 will silently report
`NoMatchingAsset` against 0.1.4 and the update indicator stays dark
— which is the correct, safe behavior.

---

## 5. Partition layout (16 MB flash)

There are **two partition layouts** in the tree. Which one a badge
uses is determined entirely at flash time — neither layout is OTA-
upgradable to the other (the partition table itself lives at flash
offset 0x8000 and is never rewritten by an OTA app-slot install).

### Default — `partitions_replay_16MB_doom.csv` (production, OTA path)

| Region    | Offset    | Size       | Purpose                              |
|-----------|-----------|------------|--------------------------------------|
| boot+ptab | 0x000000  | 36 KB      | bootloader + partition table         |
| nvs       | 0x009000  | 20 KB      | preferences (incl. `badge_ota`)      |
| otadata   | 0x00E000  | 8 KB       | bootloader's slot pointer            |
| **app0**  | 0x010000  | **3.84 MB** | OTA slot A (currently ~74 % full)   |
| **app1**  | 0x3F0000  | **3.84 MB** | OTA slot B                          |
| **ffat**  | 0x7D0000  | **6 MB**   | user filesystem                      |
| (gap)     | 0xDD0000  | 2 MB       | unused — reserved for future bumps   |
| coredump  | 0xFD0000  | 64 KB      | panic dumps                          |

**This is what every shipped badge runs and what `firmware.bin` on
the GitHub release is built against.** Keep it that way unless you
also want to manage a parallel asset stream — `[env:echo]` in
[firmware/platformio.ini](../platformio.ini) points at this file and
the release Action builds for this layout exclusively.

### Opt-in — `partitions_replay_16MB_ver2.csv` (expanded, USB only)

| Region    | Offset    | Size        | Purpose                            |
|-----------|-----------|-------------|------------------------------------|
| boot+ptab | 0x000000  | 36 KB       | bootloader + partition table       |
| nvs       | 0x009000  | 20 KB       |                                    |
| otadata   | 0x00E000  | 8 KB        |                                    |
| **app0**  | 0x010000  | **4.5 MB**  | OTA slot A (currently ~64 % full)  |
| **app1**  | 0x490000  | **4.5 MB**  | OTA slot B                         |
| **ffat**  | 0x910000  | **6.875 MB** | user filesystem                   |
| coredump  | 0xFF0000  | 64 KB       |                                    |

Bigger OTA slots (more headroom for future firmware bloat) and a
larger ffat (more room for assets / contacts). Reclaims the 2 MB gap.

### How a user opts in

The partition table is at flash offset 0x8000 and `pio run -t upload`
only writes the OTA app slot — it doesn't rewrite the partition
table. So the only safe way to switch a badge from `_doom` to `_ver2`
is a full chip erase + reflash over USB:

```
cd firmware && ./scripts/erase_and_flash_expanded.sh
```

The script triple-confirms because it wipes everything (contacts,
nametag, settings, WAD, WiFi credentials). After the reflash, the
user re-enters WiFi via Settings → WiFi Setup, and downloaded assets
re-fetch from the Asset Library tile.

### What about OTA after opting in?

**For now, opting in means leaving the OTA path.** The release Action
publishes one asset (`firmware.bin`) built for the `_doom` layout. An
expanded badge that pulls that asset would mis-align ffat and possibly
overflow into the wrong region — definitely a bricking risk.

**Detection:** the badge's `BADGE_PARTITIONS_EXPANDED` build symbol
identifies expanded firmware at compile time, and at runtime
`ffatPartitionBytes()` returns the actual partition size. The OTA
module currently doesn't consume this — `installCached()` just
streams whatever URL is cached. Future work, in priority order:

1. **Fail-safe gate (cheapest)**: in `BadgeOTA::installCached()`,
   compare the running build's partition size against a constant
   baked into the asset (or a value embedded in a sidecar
   `manifest.json`) and refuse to install when they disagree. Today
   an expanded badge would silently flash a `_doom`-built asset.
2. **Dual-asset OTA (full fix)**: the release Action builds both
   `pio run -e echo` and `pio run -e echo-expanded`, uploads
   `firmware.bin` and `firmware-expanded.bin`. The badge picks based
   on `#ifdef BADGE_PARTITIONS_EXPANDED` (which would override
   `OTA_ASSET_NAME`). Cost: ~2× the Action time and a manifest
   update.

Until either of those ships, a badge that runs the expanded build
should stay on the build it was flashed with — communicate this to
opt-in users.

### "Expand storage" affordance

The FW UPDATE screen has a latent "Expand storage" prompt (press X)
that triggers when the FATFS volume is meaningfully smaller than the
partition. With the current setup it doesn't fire on either layout
(both ffat volumes match their partitions on a fresh flash). It's
kept for future scenarios where a partition bump *is* OTA-deliverable
(see "Dual-asset OTA" above) or where a different partition table
ships with a wider ffat than the existing FAT volume header.

### Initial-filesystem footnote

The contents of `firmware/initial_filesystem/` get baked into the
firmware binary as `StartupFilesData.h` (so they take app-slot space)
AND copied to ffat by `provisionStartupFiles()` on first boot (so
they take ffat space too). It's accepted overhead — trades flash
space for a guaranteed-good first-boot filesystem even if ffat is
wiped.

## 5a. Factory FATFS provisioning via uploadfs

Every badge needs a populated FATFS partition before users can run
apps, see docs, browse images, or play DOOM. There are two ways to
get there at flash time:

```bash
# Single-badge dev flow:
cd firmware
~/.platformio/penv/bin/pio run -e <env> -t uploadfs

# Batch / factory flow:
cd firmware
python3 flash_loop_gui2.py <env>
```

`pio run -t uploadfs` writes a `fatfs.bin` built from `firmware/data/`
(itself a generated mirror of `firmware/initial_filesystem/`) and
flashes it to the `ffat` partition. Everything in the source tree
ships, including:

- `/lib/*.py`  (bake set + `badge_kv.py`)
- `/matrixApps/*.py`
- `/apps/<app>/**`
- `/docs/*.md`, `/images/*.png`, `/messages.json`
- `/API_REFERENCE.md`
- `/doom1.wad`  ← 4 MB; this is the canonical install path. Excluded
  from `manifest.json` because pushing 4 MB over the raw REPL takes
  forever — but `uploadfs` writes it directly. Users without
  PlatformIO can also pull it from `Community Apps → DOOM 1
  Shareware WAD` over WiFi.

> **Critical: use the bundled `pio` binary.** The macOS / micromamba /
> pyenv shim path frequently resolves `pio` to a Python without
> `platformio` installed. Always invoke
> `~/.platformio/penv/bin/pio` directly (or `cd ignition && ./start.sh`
> which handles the path itself). Symptom: `ModuleNotFoundError: No
> module named 'platformio'`.

After a factory uploadfs the FATFS tree is byte-identical to
`firmware/data/` — `badge_sync diff` should show zero missing/stale
entries.

## 5b. Filesystem diff-sync (`badge_sync.py`)

`firmware/scripts/badge_sync.py` is the canonical engine for getting
non-baked files (apps, docs, images) onto a badge **without** doing
a `fatfs.bin` reflash. It's used by:

| Caller | Invocation |
|--------|-----------|
| Operator CLI | `python3 -m badge_sync sync /dev/cu.usbmodemXXXX` |
| PlatformIO | `pio run -t fssync` *(planned target)* |
| flash_loop_gui2 | add `--post-sync` flag to the existing GUI |
| Ignition | `sync_badge_filesystem` Temporal activity (registered in `flash_worker/worker.py`) |
| JumperIDE | "Sync Filesystem" button (TS port of the same protocol) |

All five paths consume the **same** `firmware/data/manifest.json`
generated by `scripts/generate_startup_files.py`. The wire protocol
is the MicroPython raw REPL (Ctrl-A / Ctrl-D), the same protocol
mpremote and JumperIDE already speak.

### How it works

1. `list_badge(port)` → enters raw REPL, sends a 25-line walker that
   prints `f|<path>|<size>|0xFNV` for every file on `/`.
2. `diff(badge, manifest)` → buckets paths into
   `missing` / `stale` / `unchanged` / `extras`.
3. `push_files(...)` → for each path, base64-chunks the bytes and
   feeds them to a small `_put(path)` script via `input()`. Atomic
   write via `<path>.tmp` → `os.rename`.
4. Optional `clear_paths(...)` → `os.remove(path)` over the same REPL.

### When operators should run it

- **Refresh flashes** that only update `app0` (e.g. `./start.sh`):
  the FATFS partition is untouched, so user uploads survive — but
  any new files added to `firmware/data/` since the badge was last
  factory-flashed are missing. Run sync to top them up.
- **Recovery flashes** after a corrupted FATFS or accidental
  `os.remove("/lib/...")`: the bake set + sync gets you back to a
  full filesystem.
- **Field demos** where re-uploading `fatfs.bin` (which wipes user
  files) is too aggressive.

### When operators should *not* run it

- **Factory flashes via `flash_loop_gui2.py`** already write
  `fatfs.bin` in the same `esptool` batch as the firmware. Running
  sync afterwards is redundant unless you've changed `firmware/data/`
  since `pio run -t buildfs` last ran.

### NVS state is invariant

Crucially, `badge_sync` only touches FATFS — never NVS. Operator
state (badge UID, contacts, badgeInfo, `badge.kv` saves) is preserved
across every sync, even `--clear-extras`.

---

## 6. Things that go wrong (and what to do)

| Symptom (on badge serial)         | Likely cause                                    | Fix                                          |
|-----------------------------------|-------------------------------------------------|----------------------------------------------|
| `wifi unavailable`                | No WiFi credentials, or auth failed             | Settings → WiFi Setup → re-enter             |
| `http status 404`                 | Asset URL changed                               | Update `registry/community_apps.json`        |
| `community_apps_url not configured` | Settings cleared the URL                      | Restore via Settings or re-flash defaults    |
| badge_sync `could not enter raw REPL` | App still running, or REPL is wedged       | Press Ctrl+C in JumperIDE, retry             |
| badge_sync `manifest path not on disk: ...` | `firmware/data/manifest.json` is stale | Re-run `scripts/generate_startup_files.py`   |
| `release X has no 'firmware-...'` | Action didn't run, or asset name mismatch       | Re-run Action / fix `OTA_ASSET_NAME`         |
| `sha256 mismatch`                 | File served by host changed without version bump | Re-upload + bump `version`                  |
| `Update.begin failed: ...`        | Image larger than the OTA partition (4 MB)      | Trim build / reduce features                 |
| `battery too low — plug in`       | Battery <30% and no charger                     | Plug in USB                                  |
| `stream short N/M`                | Bad WiFi mid-flash                              | Try again                                    |

The Update screen surfaces the underlying error message verbatim, so
users can include it in bug reports.
