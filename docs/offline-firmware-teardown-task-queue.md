# Offline Firmware Teardown Task Queue

This tracks the API/OpenAPI removal work so the current thread can resume cleanly after context collapse.

## Decisions

- Runtime firmware is offline-first: no automatic WiFi, no server pairing gate, no background API polling.
- Keep explicit hacker networking available from MicroPython helpers only.
- Boops are local IR contact exchange only. Local contacts stay in `/boops.json`.
- Badge identity is a MicroPython-accessible JSON file at `/badgeInfo.json`; remove any old `/badgeInfo.txt` on first boot.
- Default identity:
  - `name`: `Ziggy`
  - `title`: `Chief Tartigrade`
  - `company`: `Temporal`
- Badge identity is read-only in badge UI. Users edit the file over USB; received contacts are written separately.
- Keep the Replay boot animation, cap it at 5 seconds, and allow any button press to skip it.
- Keep Map and Schedule as offline/static features.
- Remove native ping-backed apps and menu entries: Messages, Zigmoji, 1D Chess, Voting, Notes.
- Keep visual assets for later reuse.
- Remove firmware-owned OpenAPI artifacts and Replay API transport.
- Gate BLE proximity/ad code behind an off-by-default compile flag rather than deleting it.
- Build and flash with Ignition as needed for validation.

## Queue

- [x] Capture agreed scope and local dirty worktree notes.
- [x] Update boot flow: no pairing gate, replay animation timeout/skip, no auto WiFi.
- [x] Convert badge identity from `/badgeInfo.txt` to `/badgeInfo.json` with first-boot defaults and old-file cleanup.
- [x] Strip BadgeAPI/OpenAPI/generated types from firmware runtime.
- [x] Simplify boops to local IR exchange plus `/boops.json`.
- [x] Delete native ping/social/Notes app code and menu items.
- [x] Keep Map/Schedule offline; compile-gate BLE proximity.
- [x] Keep MicroPython explicit HTTP helpers and point examples at public open APIs.
- [x] Remove required build-time server/WiFi prompts; keep optional WiFi credentials for explicit HTTP.
- [x] Build firmware with PlatformIO/build.sh and fix fallout.
- [x] Flash through Ignition if connected hardware is available and the build is healthy.

## Pause / Rebase Checkpoint

- 2026-05-06: Paused local teardown mid-boop-protocol cleanup at the user's request before rebasing against `main`.
- Completed locally before pause: offline boot path, 5-second skippable Replay animation, `/badgeInfo.json` identity defaults and legacy `/badgeInfo.txt` removal, read-only badge info UI, optional-only build WiFi config, offline `WiFiService`, offline Schedule behavior, and menu removal for ping/social app entries.
- In progress at pause: `BoopsProtocol.cpp` still has stale API-worker references after the API worker block was removed. Finish or reconcile this after rebase before building.
- 2026-05-06: Rebased `ewijrfiojfgsjfgjhjhrapgu` onto `origin/main` and kept branch-side resolutions for conflicts by request.
- 2026-05-06: `firmware/build.sh echo -n` passed on the rebased branch. Ignition flashed the batch, but the badges landed in the old WiFi lifecycle and did not reach the desired offline experience. Do not push that state.
- 2026-05-06: Reapplied the paused offline teardown stash on top of the rebase. Continue from the stale `BoopsProtocol.cpp` API references.
- 2026-05-06: Removed the remaining WiFi-state UI/status labels from the offline boot path and renamed Ignition verification from WiFi verification to boot-marker verification.
- 2026-05-06: `pio run -e echo` passed after adding the MicroPython HTTP QSTR entries and fixing the ESP32 `HTTPClient::POST` payload cast.
- 2026-05-06: Ignition flashed 32 connected `echo` badges with `./start.sh -e echo --no-build -y`; all 32 passed boot-marker verification. After carrying forward the offline-compatible remote battery/Doom tweaks, direct `pio run -e echo` passed again and a second 32-badge Ignition flash also passed 32/32.
- Next action: commit and push the rebased branch.

## Watch Points

- Preserve unrelated existing changes in `firmware/platformio.ini`, `firmware/src/main.cpp`, and `ignition/`.
- Do not move/rename broad directories like `firmware/src/api/`; other work may be happening nearby.
- Keep `TextInputScreen` if still used by non-removed features such as drawing/file naming.
- Keep assets even if their old app entrypoints disappear.
