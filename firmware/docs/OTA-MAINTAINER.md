# OTA & Asset Registry — Maintainer Guide

This guide explains what a maintainer needs to do to keep firmware OTA
and the Asset Library working for badges in the field. Two systems
share this doc because they share the underlying transport (HTTP +
SHA-256), but they're independent — neither depends on the other.

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
│   upload firmware.bin -> firmware-echo.bin asset     │
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
`firmware-echo.bin`. If the release has no asset with that exact
name, the badge silently reports `NoMatchingAsset` and the indicator
stays dark — that's the safe failure mode.

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
   few minutes the release will have a `firmware-echo.bin` asset.
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
  cp .pio/build/echo/firmware.bin firmware-echo.bin
  # then upload firmware-echo.bin via the GitHub release UI
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

### Adding a new asset

1. Upload the asset somewhere with a stable HTTPS URL (see
   [Hosting on Cloudflare](#3-hosting-on-cloudflare) below).
2. Compute SHA-256 (optional but recommended):
   ```
   sha256sum doom1.wad
   ```
3. Append an entry to `registry/registry.json`:
   ```json
   {
     "id": "my-asset",
     "name": "My Asset",
     "version": "1.0",
     "url": "https://...",
     "sha256": "<hex>",
     "size": 1234567,
     "dest_path": "/my_asset.bin",
     "min_free_bytes": 1500000,
     "description": "What this is and why I'd want it"
   }
   ```
4. PR + merge.

### Pushing an updated version of an existing asset

Bump the `version` field on the existing entry. Badges that already
have it installed will see the entry flip to "Update" and the user
can re-install over the top.

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
`firmware-echo.bin` asset. To get badges actually upgrading:

1. **Either** cut a 0.1.5+ tag — the new Action will publish the
   `.bin` automatically.
2. **Or** manually attach `firmware-echo.bin` to the existing 0.1.4
   release via the GitHub web UI.

Until one of those happens, badges on 0.1.4 will silently report
`NoMatchingAsset` against 0.1.4 and the update indicator stays dark
— which is the correct, safe behavior.

---

## 5. Partition layout (16 MB flash)

The 2026 production layout uses every byte of the 16 MB chip:

| Region    | Offset    | Size       | Purpose                              |
|-----------|-----------|------------|--------------------------------------|
| boot+ptab | 0x000000  | 36 KB      | bootloader + partition table         |
| nvs       | 0x009000  | 20 KB      | preferences (incl. `badge_ota`)      |
| otadata   | 0x00E000  | 8 KB       | bootloader's slot pointer            |
| **app0**  | 0x010000  | **4.5 MB** | OTA slot A (currently ~64 % full)    |
| **app1**  | 0x490000  | **4.5 MB** | OTA slot B                           |
| **ffat**  | 0x910000  | **~6.9 MB** | user filesystem                     |
| coredump  | 0xFF0000  | 64 KB      | panic dumps                          |

This was bumped from a layout that left ~2 MB unused (and grew the OTA
slots to a clean 4.5 MB each, leaving comfortable room for future
firmware bloat). **The OTA path itself doesn't care about ffat**, so
OTA-ing this firmware to existing badges is safe — but their FAT
volume header is still sized for the old 6 MB partition, so they only
see 6 MB of the ~6.9 MB available.

**Initial-filesystem footnote**: the contents of `firmware/initial_filesystem/`
get baked into the firmware binary as `StartupFilesData.h` (so they
take app-slot space) AND copied to ffat by `provisionStartupFiles()` on
first boot (so they take ffat space too). It's accepted overhead —
trades flash space for a guaranteed-good first-boot filesystem even if
ffat is wiped.

The Firmware Update screen surfaces an "Expand storage" affordance
(press X) whenever it detects this gap. It double-confirms, then
reformats `ffat` (wipes contacts, nametags, downloaded assets,
settings.txt) and reboots. New badges flashed via USB never see the
prompt — they get the full 7.9 MB on first boot.

If you change partition sizes again later, the same flow handles it
automatically — the helper compares `esp_partition_find_first(...)`
size against the live FATFS volume size and only shows the option
when there's a meaningful gap.

## 6. Things that go wrong (and what to do)

| Symptom (on badge serial)         | Likely cause                                    | Fix                                          |
|-----------------------------------|-------------------------------------------------|----------------------------------------------|
| `wifi unavailable`                | No WiFi credentials, or auth failed             | Settings → WiFi Setup → re-enter             |
| `http status 404`                 | Asset URL changed                               | Update `registry.json`                       |
| `release X has no 'firmware-...'` | Action didn't run, or asset name mismatch       | Re-run Action / fix `OTA_ASSET_NAME`         |
| `sha256 mismatch`                 | File served by host changed without version bump | Re-upload + bump `version`                  |
| `Update.begin failed: ...`        | Image larger than the OTA partition (4 MB)      | Trim build / reduce features                 |
| `battery too low — plug in`       | Battery <30% and no charger                     | Plug in USB                                  |
| `stream short N/M`                | Bad WiFi mid-flash                              | Try again                                    |

The Update screen surfaces the underlying error message verbatim, so
users can include it in bug reports.
