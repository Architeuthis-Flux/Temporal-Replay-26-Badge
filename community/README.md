# Temporal Badge — Community Apps

This folder is the contribution surface for community-built MicroPython
apps. Anything that lands here (or in [`external.json`](external.json))
is published to badges in the field on the next push to `main` — no
firmware rebuild required.

There are **two contribution paths**, both produce the same on-badge
experience:

## Path A: PR a folder

Best for small, source-included apps you want vended through this
repo's git history.

```
community/
└── your-app-id/
    ├── main.py            # entry point + dunder metadata
    ├── icon.py            # optional 12×12 home-screen icon
    ├── manifest.toml      # optional: name / description / author / min_firmware
    └── ...
```

### Self-describing app metadata (`main.py` dunders)

After install, the badge's `AppRegistry` scans `/apps/<your-app-id>/main.py`
and surfaces matching dunders on the **home-screen grid** — no
additional config needed. The scan is a fast text walk over the first
~2 KB of `main.py`; the module is *not* imported.

```python
__title__       = "Hello"            # ≤19 chars; shown under the icon
__description__ = "Hello, community!" # ≤63 chars; long-press detail
__icon__        = "icon.py"           # 12x12 XBM bytes; see below
__order__       = 100                 # optional sort hint; lower = earlier
```

Omit any of these and the badge falls back to slug-derived defaults
(title from the folder name, generic apps glyph, no description).

### Home-screen icon (`icon.py`)

A 12x12 packed-XBM bitmap exposed as `DATA = (...)` — 24 bytes total
(2 bytes per row × 12 rows, LSB-first). See
[`example_hello/icon.py`](example_hello/icon.py) for the canonical
layout. Project convention is to author bitmaps as `0b00000000`
binary literals with a visual `.`/`X` comment block so the rendered
shape is readable from the source.

```python
WIDTH = 12
HEIGHT = 12
# Visual block goes here so reviewers can see the shape:
# .....XX.....
# .....XX.....
# .XXXXXXXXXX.
# ...
DATA = (
    0b00000000, 0b00000000,
    # ...24 bytes total...
)
```

### Constraints (enforced by CI)

- Folder name matches `^[a-z0-9][a-z0-9_-]{0,30}[a-z0-9]$`.
- Total bundle size ≤ 256 KB. Larger apps should host externally and
  use Path B.
- Per-file binary blobs ≤ 64 KB. Text files (`.py`, `.md`, `.txt`,
  `.json`, `.toml`) have no per-file cap.
- Every `*.py` file parses with the host Python's `ast` module.
- App id is unique across built-in apps + every other community submission.

PR validation runs [`scripts/regenerate_community_apps.py --check`](../scripts/regenerate_community_apps.py).
On merge to `main` the workflow re-runs the regenerator and commits
`registry/community_apps.json` directly.

## Path B: External URL

Best for apps you'd rather host yourself (your own GitHub repo,
jsDelivr, R2 bucket, blog, etc.). Either:

1. **Open the [community-app issue](../.github/ISSUE_TEMPLATE/community-app.yml)**
   and a maintainer will copy your fields into [`external.json`](external.json).
2. **Or PR `community/external.json` directly**, appending an entry that
   matches [`registry/community_apps.schema.json`](../registry/community_apps.schema.json).

Example external entry:

```json
{
  "id": "my-cool-app",
  "kind": "app",
  "name": "My Cool App",
  "version": "1.0.0",
  "dest_dir": "/apps/my-cool-app",
  "size": 12345,
  "min_free_bytes": 16384,
  "description": "Does cool things on the badge.",
  "author": "@your-handle",
  "files": [
    {
      "path": "/main.py",
      "url": "https://example.com/my_cool_app/main.py",
      "sha256": "<64-hex>",
      "size": 1234
    }
  ]
}
```

URLs must be HTTPS. SHA-256 is a corruption check (the badge does not
do certificate pinning). The on-badge installer streams each file into
`<dest>.tmp` and atomic-renames into place, so a partial download
never corrupts a previously-installed copy.

## How the badge picks up changes

The Community Apps screen fetches `registry/community_apps.json` once
per WiFi-up edge (no 24 h cooldown anymore — see
[`firmware/docs/OTA-MAINTAINER.md`](../firmware/docs/OTA-MAINTAINER.md)).
After the workflow commits a new registry your entry appears the next
time any badge connects to WiFi.

## Local testing

```
python3 scripts/regenerate_community_apps.py --check  # CI parity
python3 scripts/regenerate_community_apps.py          # actually write
```

The script is idempotent. CI installs `jsonschema` for full schema
validation; the local run skips that step gracefully if the package
isn't present.

## Security stance

This is open-source firmware. There is no app-signing infrastructure;
SHA-256 fields are corruption checks, not signatures. The threat model
is "don't brick the badge". An app that explicitly tries to corrupt
NVS, spam the IR transmitter, or otherwise abuse the badge will be
rejected at PR review. If you want a hardened fleet, fork the firmware
and the registry.
