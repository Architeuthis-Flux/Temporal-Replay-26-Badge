# Temporal Badge — Asset Registry

This folder hosts `registry.json`, the manifest the badge fetches over
WiFi to populate its **Asset Library** screen. The registry lets us
ship downloadable user files (the DOOM WAD, sound packs, fonts, etc.)
without baking them into the firmware image.

## How the badge consumes it

1. The user sets `asset_registry_url` in `firmware/settings.txt`
   (`firmware/settings.txt.example` ships with a default that points at
   `raw.githubusercontent.com/Architeuthis-Flux/Temporal-Replay-26-Badge/main/registry/registry.json`).
2. Once a day, when WiFi is connected, the badge GETs the URL,
   parses each entry, and shows them in `Settings → Asset Library`.
3. The user picks an entry, the badge streams the URL into a
   `<dest_path>.tmp` file on FatFS, optionally SHA-256 verifies, and
   atomically renames it into place.

## Schema (v1)

```json
{
  "schema_version": 1,
  "assets": [
    {
      "id": "<stable identifier, used as NVS key — keep ≤ 13 chars>",
      "name": "<display name shown on the OLED>",
      "version": "<opaque string; change to push an update>",
      "url": "<https:// or http:// download URL>",
      "sha256": "<optional 64-char hex; corruption check, not signature>",
      "size": <bytes, optional but recommended for progress bar>,
      "dest_path": "</absolute/fatfs/path.bin>",
      "min_free_bytes": <bytes, optional — refuse install if FS lower>,
      "description": "<short blurb shown in the detail screen>"
    }
  ]
}
```

### Field notes

- **`id`** is the stable key used by NVS to track which version is
  installed. Treat it like a primary key — never reuse.
- **`version`** is treated as an opaque string. Any change relative
  to the installed value flips the entry's status to "Update
  available". You can use semver, dates, or random strings.
- **`url`** redirects are followed (302), so GitHub Releases
  asset URLs and `raw.githubusercontent.com` work fine. TLS is
  unverified (`setInsecure()`) — the badge has no secrets to
  protect, so don't rely on the registry URL for security.
- **`sha256`** is optional. When present, it's used purely as a
  corruption check (catches truncated downloads). It's *not* a
  signature.
- **`min_free_bytes`** lets you reserve a safety margin above the
  raw asset size — useful for files that grow on disk due to
  cluster rounding.

## Adding a new entry

1. Fork the repo, append your entry to `assets[]`.
2. Open a PR. There's no validation step on merge — keep the JSON
   well-formed and don't reuse an existing `id`.
3. After merge, badges in the field will pick the new entry up the
   next time they refresh (within ~24 hours of WiFi connect).

### Where should the actual asset bytes live?

For files that are too big to bake into the firmware image
(≳ 100 KB — DOOM WAD, sound packs, alternate fonts), the canonical
home is **a GitHub Release attachment** managed by
[`release-assets/manifest.json`](../release-assets/manifest.json).
The `url` field then uses the stable
`https://github.com/<owner>/<repo>/releases/latest/download/<name>`
pattern — it never changes when we cut a new firmware release, so
badges' cached registry stays valid indefinitely. Full walkthrough in
[`firmware/docs/OTA-MAINTAINER.md`](../firmware/docs/OTA-MAINTAINER.md)
§ 2.

For everything else, point `url` at any HTTPS host you control
(see "Hosting your own registry" below).

## Hosting your own registry

Any HTTPS URL serving valid JSON works. A non-exhaustive list of free
hosts that play nicely:

- **GitHub Pages** — drop a `registry.json` in your `gh-pages` branch.
- **GitHub raw** — `https://raw.githubusercontent.com/…/main/registry/registry.json`
  (this is what the firmware ships with).
- **Cloudflare R2 / Pages / Workers** — see
  `firmware/docs/OTA-MAINTAINER.md` for a full walkthrough.
- **Self-hosted** — any plain `nginx` or `python -m http.server` works.

The badge sends `User-Agent: TemporalBadge-OTA/1.0` if you want to
filter access logs.
