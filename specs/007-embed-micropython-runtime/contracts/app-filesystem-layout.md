# Contract: App Filesystem Layout

**Feature**: 007-embed-micropython-runtime (v1)

---

## Root Directory

Python apps live in `/apps/` on the badge VFS (FAT filesystem) as plain `.py` source files.
The same `.py` files live in the repository under `apps/` and are copied to the VFS at flash time.

```
/apps/          ← badge VFS (plain .py source files)
  hello.py
  snake.py
  <app_name>.py

apps/           ← repository source (same .py files)
  hello.py
  snake.py
```

---

## App Format (v1)

A v1 app is a **single `.py` source file** on the badge VFS. The firmware reads the source
into a buffer and calls `mp_embed_exec_str()` — no bytecode compilation step required.
App developers write `.py` files and submit them; the badge owner flashes the VFS image.

| Rule | Detail |
|------|--------|
| Badge VFS location | `/apps/<name>.py` |
| Source location | `apps/<name>.py` in repo |
| Filename | Lowercase, alphanumeric + underscore + hyphen; no spaces |
| Max file size | `MP_SCRIPT_MAX_BYTES` (default 16 384 bytes; configurable in `BadgeConfig.h`) |
| Execution | `mp_embed_exec_str(src)` — plain source, `MICROPY_ENABLE_COMPILER=1` required |

The menu item name displayed on the badge is derived by stripping the `.py` extension.
Example: `hello.py` → menu entry `hello`.

**Note**: The `manifest.json` + subdirectory format described in the constitution's
Badge App Platform section is deferred to a future spec. v1 uses single files only.

---

## App Developer Workflow

App developers write `.py` files and submit them for inclusion in the next firmware
release. No tools, no upload step, no toolchain required.

```bash
# Write your app
vim apps/my_app.py

# Submit it (PR, email, Slack — whatever your team uses)
# The badge owner handles build + flash
```

Apps appear on the badge after the next flash cycle (~15 seconds total).

---

## Build + Flash Workflow (Badge Owner)

`build.sh` produces the firmware binary and a VFS filesystem image containing `apps/*.py`.
`flash.py` flashes both images. The badge reboots fully configured with apps available.

```bash
cd firmware/Firmware-0308-modular/
./build.sh      # compiles firmware binary + builds VFS image from apps/*.py
./flash.py      # flashes firmware binary + VFS image
```

App developers do not need to run either script.

---

## Constraints

- The firmware scans `/apps/` at menu-open time and builds the app list dynamically.
  Files added while the badge is running appear in the list the next time the menu is opened.
- Only `.py` files are listed by the v1 scanner. Subdirectories and other files are ignored.
- Maximum number of simultaneously listed apps: 16 (menu constraint).
- `/apps/` must exist on the VFS. If the directory is absent, the launcher shows "No apps"
  rather than crashing.
