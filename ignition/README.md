# ignition

Build and flash Temporal Badge firmware to one connected batch of badges.

Uses a local [Temporal](https://temporal.io) server to orchestrate the build
and flash pipeline — no cloud required. Runs are browsable in the Temporal UI
at `http://localhost:8233` while the local dev server is running.

## What it does

1. Builds firmware via PlatformIO (`firmware/` by default)
2. Detects all connected badges over USB
3. Prepares a fresh default filesystem image when building, then starts one child workflow per detected badge
4. Flashes firmware + apps to all badges in parallel with one direct esptool upload per badge and auto-retry
5. Shows per-badge live status (Resolving USB → Writing firmware + apps → Verify WiFi → Verify BLE → Done), with esptool progress in Temporal activity heartbeat details
6. Includes bounded WiFi and BLE verification logs in the verification activity results
7. Requires operator confirmation before writing to connected badges (`-y` skips the prompt for a ready batch)
8. Concurrency cap: up to 32 badges flashing at once

## First-time setup

Run once to install PlatformIO, Temporal CLI, and Python dependencies:

```bash
./setup.sh
```

Then create or edit `firmware/wifi.local.env` with your WiFi credentials.

## Usage

```bash
./start.sh                           # build echo + flash all connected badges
./start.sh --build-and-flash         # explicit build + fresh default filesystem + flash
./start.sh -e charlie                # charlie environment
./start.sh -y                        # skip the pre-flash Enter prompt
./start.sh --no-build                # skip build, flash last compiled binary
./start.sh --build-only              # build only, do not flash
./start.sh --firmware-dir /path      # target a different PlatformIO project
```

## Badge upload mode

Badges are reset into bootloader automatically via USB serial (RTS/DTR).
Connect the hub batch and power it on before pressing Enter. After the batch
finishes, unplug the whole hub, connect the next hub, and run ignition again.

The Enter prompt is the batch boundary. Ignition discovers whatever badges are
currently connected, starts child workflows for that set, and never reuses old
child workflows for a newly swapped hub.

## How it works

```
start.sh
  ├── checks/starts temporal server  (localhost:7233)
  ├── checks/starts flash worker     (flash_worker/worker.py)
  └── runs flash.py
        ├── BuildFirmwareWorkflow    → build_firmware activity
        └── FlashBadgesWorkflow      → prepare_flash_artifacts
                                     → detect_badge_devices
                                     → BadgeFlashWorkflow child per badge
                                           → resolve_badge_port
                                           → flash_badge_images
                                           → verify_badge_boot_marker
                                           → verify_badge_ble
```

The Temporal worker (`flash_worker/`) handles all the actual work. `flash.py`
just starts workflows and polls for live status.

`FlashBadgesWorkflow.status` reports the batch phase, detected count, child
workflow IDs, and pass/fail totals. Each `BadgeFlashWorkflow.status` reports
that badge's stable USB identity, initial/current port, phase, attempts, and
boot log tail. Detailed esptool progress lives in activity heartbeat details.

Set `IGNITION_UPLOAD_BAUD` to override the direct esptool upload baud rate.
The default is `921600`, matching the board definitions.

## Logs

Build output, short flash tails, and bounded WiFi/BLE verification logs are stored
in Temporal workflow history. Esptool progress is also visible while activities
run via heartbeat details. To browse:

```
http://localhost:8233/namespaces/default/workflows
```

Temporal keeps the useful operator history automatically without storing giant
serial/esptool logs in one event payload.

## Files

| File | Purpose |
|---|---|
| `start.sh` | One-command launcher — setup + build + flash |
| `setup.sh` | First-time install of all dependencies |
| `flash.py` | CLI — starts Temporal workflows, shows live status |
| `flash_worker/workflows.py` | Temporal workflow definitions |
| `flash_worker/activities.py` | Build and flash implementations |
| `flash_worker/worker.py` | Worker process entry point |
| `flash_worker/requirements.txt` | Python deps (`temporalio`, `rich`) |
