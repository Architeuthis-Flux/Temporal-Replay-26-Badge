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

The `release-firmware.yml` Action runs on **every push to `main`** and
creates a release automatically. You don't have to touch the GitHub
release UI for routine patch bumps.

How the auto-resolve works (`scripts/resolve_push_tag.sh`):

1. Reads `firmware/VERSION` (e.g. `0.2.9`).
2. If `v0.2.9` doesn't exist on origin yet, that's the new tag.
3. If it already exists, bumps the patch component to `0.2.10`,
   commits with `[skip ci]`, and pushes — so the next release tag is
   `v0.2.10`. GitHub's native `[skip ci]` rule prevents the bot's
   commit from triggering another workflow run.
4. The build then runs on the resulting commit, the tag is
   force-pointed at it, and `softprops/action-gh-release` creates the
   release (with auto-generated notes from commits since the previous
   tag) if it doesn't already exist.

So in practice:

- **Routine patch release**: just push to `main`. The action picks the
  next patch number.
- **Minor / major bump**: edit `firmware/VERSION` (or run
  `firmware/scripts/bump_version.sh minor`), commit, and push. Because
  the new tag won't exist yet, the action uses your version verbatim.
- **Rebuild an existing tag**: trigger the workflow manually from the
  Actions tab and supply the tag (e.g. `v0.2.9`). The tag will be
  force-moved to the latest `main` HEAD before the rebuild.

Badges in the field pick the new release up on their next WiFi-up
edge — the firmware fires one OTA check + one community-apps refresh
exactly once per reconnect (the previous 24 h cooldown was removed in
firmware v0.2.3). A user can also force a recheck from the **FW
UPDATE** screen by pressing D-pad Up, or trigger an install with
D-pad Right when an update is cached.

If something goes wrong with the Action:

- Check the **Actions** tab on GitHub for failure details.
- Re-run the workflow via `workflow_dispatch`. Leave the tag input
  empty to re-resolve via `firmware/VERSION`, or supply an existing
  tag to rebuild it.
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

## 2. Asset Registry (DOOM WAD, etc.) and Community Apps

The asset registry is a single JSON file (`registry/community_apps.json`,
schema v2) that lists every app + downloadable user file the badge can
fetch on demand. Schema docs live in
[`registry/README.md`](../../registry/README.md) and the JSON Schema is
[`registry/community_apps.schema.json`](../../registry/community_apps.schema.json).

### Contribution flow (no firmware rebuild)

The registry is **regenerated by CI** from three sources, in this
order:

1. `firmware/initial_filesystem/apps/<name>/` — built-in apps that ship
   in the factory FATFS image.
2. `community/<id>/` — PR'd community contributions. Each folder is
   bundled, hashed, and emitted as a `kind:"app"` entry pointing at
   `https://raw.githubusercontent.com/.../community/<id>/...`.
3. `community/external.json` — external-URL-only entries for apps that
   don't live in this repo.
4. `registry/registry.json` (legacy schema v1) — pulled forward for
   backwards compatibility with badges still running pre-v0.2 firmware.

Anyone can add an app by either PR'ing a folder under `community/<id>/`
or appending to `community/external.json` (or filing a structured
GitHub issue — see [community/README.md](../../community/README.md)).
The
[`community-apps.yml`](../../.github/workflows/community-apps.yml)
workflow validates submissions on PR (schema, id uniqueness, size
budget, Python AST parse) and re-runs
[`scripts/regenerate_community_apps.py`](../../scripts/regenerate_community_apps.py)
on every push to `main`, committing the regenerated
`registry/community_apps.json` back to the branch.

Badges in the field pick up the new entry on the next WiFi-up edge.

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
- The badge ignores HTTP cache headers and has no per-request
  cooldown anymore (removed in firmware v0.2.3) — it fires one
  refresh per WiFi-up edge and one per Community Apps screen entry.
  Edge cache TTL only matters between the moment CI commits a new
  registry and the moment Cloudflare clears its edge cache.

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

**OTA works on both layouts from the same `firmware.bin`.** The
release Action publishes one asset built for the smaller `_doom`
app slot (3.84 MB). The binary has no hardcoded flash offsets — the
bootloader maps whichever app slot is active to the standard exec
virtual address via the MMU, and the firmware finds every data
partition (ffat, otadata, nvs, coredump) at runtime via
`esp_partition_find_first`. So a badge on `_ver2` (4.5 MB slots,
6.875 MB ffat) installs the same release `.bin` as a badge on
`_doom` (3.84 MB slots, 6.0 MB ffat) and gets the larger ffat for
free.

**Hard ceiling:** `firmware.bin` must fit the smaller `_doom` slot.
The release Action checks `stat(firmware.bin) <= 0x3E0000` and fails
the upload on overflow — dropping `_doom` support would also drop
OTA for every deployed badge.

**Layout-change UX:** `BadgeOTA` records the running partition map
in NVS (`badge_ota/last_layout`). When it changes from one boot to
the next (e.g. user just ran `erase_and_flash_expanded.sh`), the
next visit to the FW UPDATE screen shows a one-shot welcome panel
with the new ffat size before falling through to the normal OTA flow.

### "Expand storage" affordance

The FW UPDATE screen has two adjacent X-button affordances:

1. **Expand the FAT volume within the current partition** — only
   fires when the formatted FAT capacity is meaningfully smaller
   than the partition (>256 KB slack). Reformats `ffat` in place.
2. **Bigger storage (layout migration)** — only shown on the
   default `_doom` layout. Walks the user through an **in-place
   partition-table swap** (see § 5c) and falls back to the legacy
   USB-script path if the running firmware doesn't have the
   partition blob embedded.

The `BADGE_PARTITIONS_EXPANDED` define is now only a debugging
breadcrumb — runtime code detects the layout from the mounted ffat
address and doesn't consume the macro.

## 5c. In-place partition-table migration (`_doom` → `_ver2`)

The badge can rewrite its own partition table at runtime to switch
from `_doom` (6.0 MB ffat) to `_ver2` (6.875 MB ffat) without USB
reflashing. The mechanism, the guarantees, and what to do when it
breaks all live here.

### Why it's safe (most of the time)

Two facts of the layout pair make this tractable:

- **`app0` is at the same physical offset (0x10000) in both tables.**
  A `_doom`-built firmware at `0x10000` boots correctly under a
  `_ver2` partition table — the slot just becomes bigger (3.84 MB →
  4.5 MB) and the same image still occupies its first ~2.8 MB.
- **`firmware.bin` is layout-agnostic.** Every data partition is
  located at runtime via `esp_partition_find_first`; there are no
  hardcoded flash offsets. So no separate firmware needs to be
  flashed alongside the table swap.

`app1` *does* differ between the layouts (`0x3F0000` vs `0x490000`).
That's the failure mode — see "It refused to run" below.

### What the badge actually does

`ota::migrateToExpandedLayout()`:

1. Confirms the running app is at `app0` (i.e. `esp_ota_get_running_partition()->address == 0x10000`).
2. Confirms battery ≥ 50 % OR USB power is present.
3. Snapshots the existing 4 KB partition-table sector at `0x8000`
   into RAM (`heap_caps_malloc`).
4. `esp_flash_erase_region(NULL, 0x8000, 0x1000)`.
5. `esp_flash_write(NULL, embedded_ver2_blob, 0x8000, blob_len)`.
6. Read back + `memcmp` against the embedded blob.
7. **On verify success:** `ESP.restart()`. The next boot brings up
   the larger ffat at `0x910000` (unformatted → caught by the
   FATFS mount path → reformatted on first mount failure).
8. **On any failure between (4) and (6):** erase the sector again,
   write the snapshotted old bytes back, return `MigrationResult::k*Failed`
   to the UI. The badge keeps running.

The dangerous window is between erase (step 4) and a successful
verified write (step 6) — a few hundred ms. Power loss in that
window leaves the partition table sector blank; the bootloader will
panic and the badge becomes unbootable until USB reflashed.

### The recovery story (what the on-device QR points at)

If the migration window is interrupted (power cut, JTAG reset,
flash defect mid-write), the badge cannot recover on its own. The
fix is the same as any first-time flash:

```bash
# Default `_doom` layout (matches every production badge):
cd firmware && ./scripts/erase_and_flash.sh

# `_ver2` layout (if the user wanted the migration to land):
cd firmware && ./scripts/erase_and_flash_expanded.sh
```

Both scripts do a full chip erase + reflash bootloader + partition
table + factory app + filesystem image over USB DFU. NVS (contacts,
WiFi creds, badge UID) is wiped — recovery is destructive, same as
the original factory provisioning.

The FW UPDATE screen shows a QR code pointing at this document
*before* asking the user to confirm the destructive step, so the
recovery URL ends up on their phone before they take the risk.

### UI flow (`UpdateFirmwareScreen`)

| Phase                          | What the screen shows |
|--------------------------------|-----------------------|
| `kLayoutMigratePrecheck`       | Headline + 3 condition rows (battery, layout, recovery blob present). Confirm advances; cancel returns to idle. |
| `kLayoutMigrateRecovery`       | QR linking to this doc, plus a 4-line "USB + erase_and_flash.sh" recap. Confirm advances; cancel returns. |
| `kLayoutMigrateConfirm`        | Final modal panel ("Rewrites partition tbl / Wipes ffat / Auto-reboots") with a danger frame. Confirm fires the migration; cancel returns. |
| `kLayoutMigrating`             | "Swapping partition table / DO NOT POWER OFF". Painted once before the synchronous flash ops; on success the badge reboots; on rollback it falls through to `kLayoutMigrateError`. |
| `kLayoutMigrateError`          | One-line cause from `MigrationResult` plus the raw `ota::lastErrorMessage()` fallback. Any button returns to idle. |
| `kLayoutWelcome` (next boot)   | One-shot panel reading "Layout changed / FS partition: 6.9 MB" on the next FW UPDATE entry after a successful migration. Cleared by `ota::acknowledgeLayoutChange()`. |

### It refused to run — common reasons

| `MigrationResult`          | Meaning                                                                                       | Fix                                                                                          |
|----------------------------|-----------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------|
| `kAlreadyExpanded`         | Layout is already `_ver2`. Migration is a no-op.                                              | Nothing to do.                                                                               |
| `kBatteryTooLow`           | Battery <50 % and not on USB.                                                                 | Plug in USB or charge above 50 %, then retry.                                                |
| `kNotRunningFromApp0`      | Active slot is `app1` (at `0x3F0000`, which doesn't exist in `_ver2`).                        | Install the latest OTA from the FW UPDATE screen. OTA always writes to the *inactive* slot, so the install flips the active slot back to `app0` on next boot. |
| `kEmbedMissing`            | This firmware build doesn't have `partitions_ver2.bin` embedded. Should not happen on releases. | Confirm `firmware/build_assets/partitions_ver2.bin` exists locally and rebuild.              |
| `kFlashReadFailed` / `kFlashEraseFailed` | Flash driver returned an error before any destructive write completed. Safe.    | Retry; if persistent, the hardware may be failing.                                           |
| `kFlashWriteFailed`        | Write returned non-`ESP_OK`. Rollback was attempted automatically — badge keeps running.       | Retry.                                                                                       |
| `kVerifyFailed`            | Readback didn't match. Rollback was applied; badge keeps running.                              | Retry; if it keeps failing, USB recovery via `erase_and_flash_expanded.sh`.                  |

### Building / shipping the partition blob

The embedded blob is produced at build time by
[`firmware/scripts/gen_expanded_partitions.py`](../scripts/gen_expanded_partitions.py),
which runs the framework's `gen_esp32part.py` against
`partitions_replay_16MB_ver2.csv` and writes the result to
`firmware/build_assets/partitions_ver2.bin`. Then
`board_build.embed_files` in `[env:echo]` links it into the firmware
and `scripts/normalize_bundle_symbols.py` normalizes the symbol
names to `_binary_partitions_ver2_bin_{start,end}`.

The blob is small (~150 bytes — one MD5'd partition table). It adds
no meaningful size to `firmware.bin` and is regenerated on every
build only when the CSV or the generator script is newer than the
existing output (idempotent).

### Reverting from `_ver2` back to `_doom`

Not supported in-place — the inverse would force-format the larger
ffat region and there's no upside vs. the USB script. If a user
needs to go back, they USB-flash from the recovery image (see § 5d
below).

<a id="recovery"></a>

## 5d. Recovery — full-flash 16 MB image

The FW UPDATE screen's "BIGGER STORAGE" warning panel shows a QR code
linking to this section. Every GitHub release attaches a single-file
16 MB recovery image so any bricked-badge situation has the same
one-line answer.

### Where the file lives

Attached to every release as
`temporal-badge-full-flash-16mb.bin`. Stable URL:

```
https://github.com/Architeuthis-Flux/Temporal-Replay-26-Badge/releases/latest/download/temporal-badge-full-flash-16mb.bin
```

What it contains, at production flash offsets:

| Offset    | Content              | Source                              |
|-----------|----------------------|-------------------------------------|
| 0x000000  | bootloader.bin       | PlatformIO build output             |
| 0x008000  | partitions.bin       | `partitions_replay_16MB_doom.csv`   |
| 0x00E000  | boot_app0.bin        | arduino-esp32 framework             |
| 0x010000  | firmware.bin         | the release's app slot 0            |
| 0x7D0000  | fatfs.bin            | `initial_filesystem/` + `doom1.wad` + every in-repo `community/<id>/` preloaded into `/apps/<id>/` |
| (gaps)    | 0xFF                 | `--fill-flash-size 16MB` from esptool merge_bin |

Total size: exactly 16 MiB (16,777,216 bytes). The file is what the
SPI flash chip looks like the moment a brand-new badge boots for the
first time — wipe and reflash with this and you have a factory-fresh
device.

### How a user recovers

1. **Plug the badge into USB.** No special button hold — the
   ESP32-S3's native USB-CDC handles reset automatically.
2. **Install `esptool`** (one-time, on the recovery host):

   ```bash
   python3 -m pip install --upgrade esptool
   ```

3. **Download the recovery image** from the latest release:

   ```bash
   curl -LO https://github.com/Architeuthis-Flux/Temporal-Replay-26-Badge/releases/latest/download/temporal-badge-full-flash-16mb.bin
   ```

4. **Find the badge's serial port**: macOS / Linux `/dev/cu.usbmodem*`
   or `/dev/ttyACM*`; Windows `COMx` from Device Manager.

5. **Flash it**:

   ```bash
   esptool.py --chip esp32s3 --port /dev/cu.usbmodemXXXX \
       write_flash 0x0 temporal-badge-full-flash-16mb.bin
   ```

   Takes ~90 seconds at the default 460800 baud. `--baud 921600`
   roughly halves that on hardware that can keep up.

6. **Power-cycle.** Badge boots into the factory state — no contacts,
   no nametag, no saved WiFi (those live in NVS, which was wiped along
   with everything else). Walk through Settings → WiFi Setup and
   you're back online.

`write_flash 0x0 <image>` reproduces the chip byte-for-byte, so any
prior partition-table layout (`_doom` or `_ver2`), corrupted
`otadata`, or trashed FATFS is replaced — the recovery image always
lands on the production `_doom` layout regardless of what was there
before.

### Building the recovery image locally

```bash
cd firmware
# Build firmware + initial fatfs.bin first (the script reuses these):
~/.platformio/penv/bin/pio run -e echo

# Then assemble the 16 MB merged image:
python3 scripts/build_recovery_image.py \
    --env echo \
    --out artifacts/recovery/temporal-badge-full-flash-16mb.bin
```

The script (`firmware/scripts/build_recovery_image.py`):

1. Stages `community/<id>/` → `firmware/data/apps/<id>/` (drops
   `manifest.toml` + dotfiles).
2. Runs `pio run -e echo -t buildfs` so the fresh fatfs.bin carries
   the community apps + DOOM WAD.
3. Calls `esptool.py merge_bin --fill-flash-size 16MB …` against
   bootloader + partitions + boot_app0 + firmware + fatfs.

CI runs the same script from `.github/workflows/release-firmware.yml`,
so what the user downloads from the release is reproducible from a
clean checkout.

### Recovery image vs. `erase_and_flash_expanded.sh`

| Path                                  | When to use                                                | Layout after recovery |
|---------------------------------------|------------------------------------------------------------|-----------------------|
| Full-flash recovery image (this §)    | Bricked badge, no PlatformIO, no source checkout           | `_doom` (production)  |
| `firmware/scripts/erase_and_flash_expanded.sh` | Working badge, opt-in to bigger ffat from source  | `_ver2` (expanded)    |
| In-place partition migration (§ 5c)   | Working badge on `_doom`, wants `_ver2` without USB        | `_ver2` (expanded)    |

The recovery image is the simplest of the three because it doesn't
care about prior state — it just overwrites everything. The trade is
that NVS goes with it (contacts, paired ticket UUID, saved WiFi).

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
| `partition blob missing from firmware` | Build did not include `partitions_ver2.bin`  | Run `scripts/gen_expanded_partitions.py` and rebuild |
| `flash erase rc=…` / `flash write rc=…` mid-migration | Underlying SPI flash op failed       | Retry; persistent failure → USB recovery via `erase_and_flash.sh` |
| `run from app0 — reinstall OTA first` | Migration refused: active slot is `app1`     | Install the latest OTA, then retry migration |
| Bricked after partition swap      | Power cut during the partition-table write window | USB recovery — `./scripts/erase_and_flash.sh` from the `firmware/` dir |

The Update screen surfaces the underlying error message verbatim, so
users can include it in bug reports.
