# Release-bundled assets

Files listed in [`manifest.json`](manifest.json) get attached to every
GitHub release of this repo as named assets, alongside the firmware
binary. They're served from a stable URL pattern that the badge's
Asset Library can fetch directly:

```
https://github.com/<owner>/<repo>/releases/latest/download/<name>
```

This is the right home for files that are **too large to bake into
the firmware image** (i.e. things `firmware/initial_filesystem/`
can't carry without inflating the OTA app slot — anything ≳ 100 KB,
realistically). The DOOM WAD lives here.

## Why not just initial_filesystem/?

`initial_filesystem/` files are embedded into the `.bin` via
`StartupFilesData.h` and ALSO copied to ffat on first boot. That's
fine for ≤ 100 KB things (a config file, a few PNGs). For a 4 MB
WAD it'd push the firmware off the OTA partition cliff. So we keep
the WAD out of the build, attach it to the release, and let the
badge pull it on demand into ffat.

## Adding a new bundled asset

1. Add an entry to `manifest.json`:

   ```json
   {
     "name": "soundpack-1.tar",
     "local_path": "firmware/data/soundpack-1.tar",
     "fallback_url": "",
     "registry_id": "soundpack-1",
     "description": "Optional sound pack for DOOM"
   }
   ```

   - `name`: the filename shown on the GitHub release (and the URL
     suffix the badge will GET).
   - `local_path`: where the file lives in the repo. Optional if
     `fallback_url` is set — CI will curl it instead.
   - `fallback_url`: used when `local_path` is missing (e.g. the
     file is gitignored). Optional.
   - `registry_id`: optional. If set, the staging script verifies
     that `registry/registry.json`'s entry with that id has a
     matching `sha256` and `size`. Mismatch fails the release build
     so you can't accidentally ship a stale manifest.

2. Add (or update) the matching entry in
   [`registry/registry.json`](../registry/registry.json) with the
   stable `/releases/latest/download/<name>` URL, the file's
   SHA-256, and its size in bytes.

3. PR + merge + cut a release. The
   [release-firmware.yml](../.github/workflows/release-firmware.yml)
   Action stages the file (local copy or download), verifies it
   against the registry, and uploads it to the release.

## Locally checking your changes

```
python3 scripts/stage_release_assets.py --check
```

`--check` runs the same verification CI does (sha256 / size match
between manifest, files, and registry) without writing any artifacts
or hitting the network if everything is local. Drop the `--check`
flag to actually stage files into `artifacts/release/`.
