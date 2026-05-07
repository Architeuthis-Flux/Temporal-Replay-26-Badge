# Two-Badge Hardware Test Harness

Status: planned, not implemented yet. Use this document plus
`specs/012-hardware-test-harness/spec.md` and
`specs/012-hardware-test-harness/tasks.md` when implementing or resuming after
context compaction.

## Purpose

The hardware harness is for firmware feature development. It should automate as
much of a two-badge physical test pass as possible:

- build and multi-flash the badges;
- control each badge through USB serial and the MicroPython REPL;
- capture raw C++ `Serial` logs and MicroPython output;
- prepare backend state through the sibling `registrationScanner` repo;
- run real physical IR boop tests through the UI path;
- preserve logs and summaries for later review.

The backend test suite already covers backend correctness. These tests should
verify that badge firmware works against the backend and on physical hardware.

## Repos and Targets

- Firmware repo: `Temporal-Badge`
- Backend dependency: sibling repo `../registrationScanner`
- Backend target for integration tests: `https://brooklyn.party`
- Default firmware env: `echo-dev`
- Default flash path: Ignition multi-flash
- Direct PlatformIO upload: fallback only

## Required Hardware Setup

- Two Temporal badges connected over USB serial.
- Badges should be close enough for IR in both directions.
- No external power relay is required for the baseline harness.
- No sacrificial/destructive hardware testing is in scope.

## Flashing Policy

Use Ignition for normal multi-badge flashing:

```bash
cd /Users/shy/Documents/Temporal/replayBadgeOmni/Temporal-Badge/ignition
./start.sh -e echo-dev
```

Useful smoke commands:

```bash
# Verify the Ignition build path without flashing.
cd /Users/shy/Documents/Temporal/replayBadgeOmni/Temporal-Badge/ignition
./start.sh -e echo-dev --build-only

# Flash the already-built image to all connected badges.
cd /Users/shy/Documents/Temporal/replayBadgeOmni/Temporal-Badge/ignition
./start.sh -e echo-dev --no-build

# Verify watch/plug-and-flash behavior for exactly two badges.
cd /Users/shy/Documents/Temporal/replayBadgeOmni/Temporal-Badge/ignition
./start.sh -e echo-dev --watch --no-build --watch-max-badges 2
```

Ignition starts local Temporal and a flash worker, detects all connected badges,
then flashes firmware and the filesystem image in parallel. Review workflow
history at `http://localhost:8233`.

Before flashing, close any serial monitors or harness serial readers. Serial
logging and flashing compete for the same USB ports.

Direct PlatformIO upload remains useful for isolating a single bad port:

```bash
cd /Users/shy/Documents/Temporal/replayBadgeOmni/Temporal-Badge/firmware
/Users/shy/.platformio/penv/bin/pio run -e echo-dev --upload-port /dev/cu.usbmodemXXXX -t upload
```

## Harness Layout

Planned implementation path:

```text
firmware/hardware_tests/
  README.md
  conftest.py
  pytest.ini
  harness/
    backend.py
    ignition.py
    flashing.py
    ports.py
    serial_repl.py
    timeline.py
  test_smoke.py
  test_boop_physical.py
  test_clean_state.py
```

Run shape once implemented:

```bash
cd /Users/shy/Documents/Temporal/replayBadgeOmni/Temporal-Badge/firmware
uv run --with pytest --with pyserial python -m pytest hardware_tests -m hardware
```

Expected options:

- `--env echo-dev`
- `--no-flash`
- `--no-build`
- `--badge-a-port /dev/cu...`
- `--badge-b-port /dev/cu...`

Do not assume PlatformIO's Python can run the pytest harness. It has pyserial
available on this machine, but not pytest. The harness should own or document a
small Python test environment that includes both `pytest` and `pyserial`.

## Serial Control Model

Each badge gets a long-lived serial reader. The serial reader must write every
raw line before trying to parse command responses. C++ logs and MicroPython REPL
responses share the same stream.

Use framed command output:

```python
token = "hb_000123"
send("print('__HB_BEGIN:%s__')" % token)
send("print(repr(badge.dev('uid')))")
send("print('__HB_END:%s__')" % token)
```

Bring the REPL into a known state before each command batch:

1. Open the serial port at 115200 baud.
2. Send `Ctrl-C` twice, then `\r\n`.
3. Wait briefly, then send `import badge`.
4. Use unique sentinels for each command and capture only lines between those
   sentinels.

Do not parse by searching the whole accumulated log from the beginning. Keep a
per-badge cursor or snapshot mark for each wait, otherwise old successful lines
can satisfy a new assertion. The raw log remains the source of truth when the
command capture gets noisy.

Initial firmware control surface in `echo-dev`:

- `badge.dev("uid")`
- `badge.dev("help")`
- `badge.dev("btn", "up|down|left|right")`
- `badge.dev("fb")`
- `badge.dev("send", peer_uid_hex, peer_ticket_uuid, peer_name, body)`

Feature work may add additional `badge.dev(...)` commands behind
`BADGE_ENABLE_MP_DEV` when the test needs to verify protocol/storage behavior
without depending on menu focus. For example, the 1D Chess work used
dev-only `chess_challenge`, `chess_move`, and `chess_resign` helpers to prove
ping/state behavior over USB, while keeping the production `echo` image free of
those hooks.

Useful normal MicroPython functions:

- `my_uuid()`
- `boops()`
- `ir_start()`, `ir_stop()`, `ir_send()`, `ir_read()`
- `ir_send_words()`, `ir_read_words()`, `ir_flush()`

Add new C++ dev commands only when tests cannot be reliable through the current
surface. Always verify both builds:

```bash
cd /Users/shy/Documents/Temporal/replayBadgeOmni/Temporal-Badge/firmware
/Users/shy/.platformio/penv/bin/pio run -e echo-dev
/Users/shy/.platformio/penv/bin/pio run -e echo
```

### USB Serial Lessons From 1D Chess

- `echo-dev` exposes `badge.dev(...)`; normal `echo` does not. If a serial test
  needs dev hooks, flash and verify `echo-dev` on both badges.
- After every flash, reopen the ports and assert `badge.dev("help")` on both
  badges. A stale image on one badge can masquerade as a firmware bug.
- Use explicit ports when known, for example `/dev/cu.usbmodem101` and
  `/dev/cu.usbmodem1101`. Auto-detect is fine only when exactly two candidate
  badge ports are present.
- `badge.dev("fb")` dumps `FB BEGIN`, eight `P0`-`P7` hex rows, then `FB END`.
  This is useful for screenshots/assertions but should not replace state/log
  assertions for protocol tests.
- UI driving with synthetic button presses is appropriate for acceptance tests,
  but it is brittle for deep protocol debugging because menu focus, tutorial
  state, completed rows, and notification screens can change the path. Prefer
  feature-specific dev hooks for deterministic protocol tests, then keep a
  separate UI smoke that opens the real screen and exercises the visible flow.
- Polling pings can take 30 seconds unless a foreground screen has pushed the
  faster Wi-Fi poll interval. Serial tests should wait on log evidence such as
  `route one_d_chess ... actionable=1` rather than fixed sleeps.
- Backend pings persist. Clearing a local game store, especially the `seen`
  list, can make old backend pings reappear. Clean backend rows for the selected
  seed tickets when possible, or send/ingest a terminal ping so stale lower-seq
  events cannot revive an active game.
- For turn-based games, the harness should assert both the HTTP send log
  (`[OneDChess] send ... http=200 ok=1`) and the receiver route log
  (`[WiFi] route one_d_chess ... actionable=...`), then read the on-device JSON
  file to confirm final state.
- Take the receiver's log cursor before issuing the sender command. In the
  full-game 1D Chess QA pass, the peer sometimes routed the ping before the
  sender-side HTTP wait finished, so a cursor taken afterward missed the real
  route line.
- A full-game protocol QA should play until a terminal result, not only through
  resign. The 1D Chess serial pass completed at `seq=6`, `next=255`,
  `result=2` (`BlackMate`), with matching terminal JSON on both badges.
- Local JSON stores use ArduinoJson arenas and may need explicit
  `garbageCollect()` after removes/reorders. Treat `json gc ... reclaimed ...`
  logs as useful evidence that the tested path exercised compaction.
- Opening a MicroPython REPL over USB interrupts app code enough to change
  timing. Keep readers passive whenever possible, and only enter REPL when
  issuing framed commands.

## Log Artifacts

Default log root:

```text
firmware/logs/hardware/
  latest -> runs/<timestamp>/
  runs/<timestamp>/
    manifest.json
    BADGE-A_<usb-serial>.raw.log
    BADGE-B_<usb-serial>.raw.log
    timeline.log
    commands.jsonl
    junit.xml
```

At the start of every test pass, replace `latest` and wipe the current pass's
logs. Preserve older `runs/` unless the user explicitly asks to delete them.

`manifest.json` should include:

- firmware env and git SHA if available;
- Ignition command/result;
- badge ports and USB serial identifiers;
- hardware UID from `badge.dev("uid")`;
- normalized backend badge UUID;
- seed ticket UUIDs used by the test pass;
- backend URL target.

## Backend State Setup

Use the sibling backend repo as the source of truth:

```text
/Users/shy/Documents/Temporal/replayBadgeOmni/registrationScanner
```

Call backend helpers through `uv run` subprocesses from that repo. Do not copy
backend DB config or secrets into the firmware repo.

Known helpers:

- `workflows._shared.normalize_badge_id`
- `workflows.activities.link_badge_to_ticket`
- `workflows.activities.unlink_badge`
- `db.sync_fetchrow`
- `db.sync_execute`

Use TESTREPLAY75 seed tickets from `registrationScanner/seed.sql`. Tests may
reset those seed rows as needed. For physical boop acceptance, do not pre-create
the `badge_boops` row; let the badges create it through the real IR/API path.

For ping-backed feature tests, backend cleanup should remove pings scoped to the
selected seed tickets before a fresh run. When a backend delete helper is not
available, the firmware protocol should have a terminal/dismissed state and the
test should use that terminal event instead of repeatedly clearing local state.

## Required Test Groups

### Preflight

1. Run Ignition `echo-dev` build/multi-flash unless disabled.
2. Detect exactly two badges or use explicit ports.
3. Open serial readers and confirm `badge.dev("help")`.
4. Record distinct badge UIDs.
5. Confirm backend bridge can link and unlink a seed ticket.

### IR Canary

Before physical boop tests, prove badge placement and IR hardware:

1. Badge A receives while badge B sends a simple IR frame.
2. Badge B receives while badge A sends a simple IR frame.
3. Fail or skip physical boop tests with a clear placement/IR message if either
   direction fails.

### Physical Boop Acceptance

1. Link badge A and badge B to two distinct TESTREPLAY75 tickets.
2. Reboot/wait until both badges are paired and on the main menu.
3. Send RIGHT to both badges to enter the first menu item, `Boop`.
4. Wait for `GUI: BoopScreen entered, IR enabled` on both serial logs.
5. Send UP to both badges in a short window.
6. Wait for both badges to reach boop success.
7. Assert `boops()` on both badges includes the peer.
8. Assert backend `badge_boops` has an active row for the two linked tickets.

### Clean-State Acceptance

1. Start with a badge linked and holding non-empty contact/boop state.
2. Unlink the badge in backend state.
3. Reboot or wait for firmware pairing poll.
4. Assert the badge returns to unpaired/QR state.
5. Assert stale `/boops.json` and pending boop API files are removed or reset.

Known firmware gap to check: the current unpair path clears NVS and
`/badgeInfo.txt`, but may leave `/boops.json` and pending boop API journals.

### Ping-Backed Feature Acceptance

For features that use `/api/v1/pings`, such as turn-based games:

1. Start with two linked seed tickets and a known pair/boop relationship.
2. Clean or terminally close prior ping-backed state for those tickets.
3. Drive the user-visible entry path far enough to confirm the screen is
   reachable.
4. Use dev-only protocol hooks, when available, for the deterministic send,
   receive, and end-of-game path.
5. Assert sender HTTP status, receiver route log, local JSON state on both
   badges, notification counts, and absence of unexpected active games.
6. Leave completed state, not active state, so future runs are not blocked by
   the one-active-game-per-peer rule.

## Future-Agent Bootstrap

When resuming this work:

1. Read this document.
2. Read `specs/012-hardware-test-harness/spec.md`.
3. Read `specs/012-hardware-test-harness/plan.md`.
4. Continue from `specs/012-hardware-test-harness/tasks.md`.
5. Check `git status --short` and do not revert unrelated user work.
6. Run Ignition build-only before editing harness code:

```bash
cd /Users/shy/Documents/Temporal/replayBadgeOmni/Temporal-Badge/ignition
./start.sh -e echo-dev --build-only
```

7. If two badges are available and the user is ready to flash, run:

```bash
cd /Users/shy/Documents/Temporal/replayBadgeOmni/Temporal-Badge/ignition
./start.sh -e echo-dev
```

8. Capture all harness output under `firmware/logs/hardware/`.

## Troubleshooting

- Serial open fails: stop `firmware/serial_log.py`, PlatformIO monitor, or any
  prior harness process using the same ports.
- Flashing fails after serial logging: close serial readers before Ignition and
  re-detect ports after flashing.
- Ignition worker fails: inspect `/tmp/temporal-badge-worker.log` and Temporal
  workflow history at `http://localhost:8233`. Start with the parent
  `FlashBadgesWorkflow`, then open the failed badge's `BadgeFlashWorkflow`
  child for the per-badge flash/WiFi result and bounded WiFi log.
- Temporal server fails: inspect `/tmp/temporal-badge-server.log`.
- IR canary fails: move badges closer, align IR sides, and rerun only canary
  before running physical boop acceptance.
- Backend seed exhaustion: reset TESTREPLAY75 seed rows in `registrationScanner`
  or choose explicit seed tickets from `seed.sql`.
- Dev command missing from one badge: confirm that badge was flashed with the
  latest `echo-dev`, then re-run `badge.dev("help")` on both ports.
- A receiver logs an actionable ping but no game appears in its local JSON:
  inspect the incoming payload parsing path for required fields getting
  overwritten during state initialization, then confirm the store save did not
  prune the object as invalid.
- A send succeeds but the peer never reacts: confirm the peer's ticket UUID
  matches the backend-linked ticket, confirm Wi-Fi polling is enabled, and wait
  on the peer's `getPings(type)` log rather than the sender log.
