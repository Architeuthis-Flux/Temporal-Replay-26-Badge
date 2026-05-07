# spec-012: Two-Badge Hardware Test Harness — Implementation Plan

Build a local, firmware-focused hardware test harness for two physical badges.
The harness controls badges over USB serial, reflashes `echo-dev` through
Ignition's multi-flash workflow, captures raw C++ and MicroPython logs, prepares
backend state through `registrationScanner`, and exercises real badge behavior
with minimal human interaction.

## Current Decision Record

- The harness lives in `Temporal-Badge`; `registrationScanner` is a dependency.
- The backend is assumed solid. Firmware tests may use direct DB/backend helper
  calls for setup and assertions rather than retesting scanner flows.
- The target backend for integration tests is `https://brooklyn.party`.
- The default firmware environment is `echo-dev`; it currently builds.
- Tests may automatically reflash the connected badges.
- Ignition is the default flashing path. Direct PlatformIO upload remains a
  debug fallback for single-port bring-up or when isolating Ignition failures.
- Serial control is the only required hardware control surface. No external
  power relay is part of the baseline.
- The serial stream is shared by C++ `Serial` logs and MicroPython REPL output,
  so the harness must preserve raw logs and use framed/sentinel command output.
- After every flash, the harness must reopen both ports and assert
  `badge.dev("help")` on each badge. A stale image on only one badge can look
  exactly like a feature regression.
- Serial log waits must use a per-badge cursor or snapshot mark. Searching the
  whole accumulated log can accidentally satisfy a new assertion with an old
  success line.
- Logs are wiped at the start of each test pass and saved per badge plus as a
  merged timeline.
- Physical boop coverage must use the real UI path: inject button presses over
  serial to enter the Boop screen and engage the boop state. Do not replace the
  IR path with a mocked pairing event.
- Feature protocol/storage tests may add feature-specific dev hooks behind
  `BADGE_ENABLE_MP_DEV` when UI button driving is too brittle. Keep a separate
  UI smoke path for the real screen.
- Any feature that adds dev-only hooks must continue to build both `echo-dev`
  and production `echo`.
- Ping-backed features need backend-scoped cleanup or a terminal protocol state.
  Do not treat clearing only local `seen` state as cleanup, because persisted
  backend pings can be replayed into a new local store.
- Badge clean-state behavior is production behavior, not just a test shortcut:
  when a previously paired badge is no longer linked to an identity, firmware
  should remove stale local identity/contact state, including `boops.json`.

## Architecture

### Host Harness

Target path:

```text
firmware/hardware_tests/
  conftest.py
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

The harness should own a small Python test environment with `pytest` and
`pyserial`, use `ignition/start.sh` for normal multi-badge flashing, and call
into the backend repo through `uv run` subprocesses when it needs
`registrationScanner` DB helpers. Keeping this split explicit avoids dependency
fights between the harness test environment, PlatformIO's Python environment,
Ignition's virtualenv, and the backend's managed `uv` environment.

### Ignition Flashing Model

Ignition already owns the multi-flash workflow:

```bash
cd ignition
./start.sh -e echo-dev
```

That command starts local Temporal if needed, starts the flash worker, builds
`echo-dev`, detects all connected badges, and flashes firmware plus the
filesystem image to all badges in parallel. The harness should shell out to this
workflow by default and treat successful completion as the flash preflight.

Useful modes:

- `./start.sh -e echo-dev --build-only` verifies the Ignition build path without
  touching hardware.
- `./start.sh -e echo-dev --no-build` flashes the last built image.
- `./start.sh -e echo-dev --watch --no-build --watch-max-badges 2` verifies
  plug-and-flash behavior for exactly two badges.

The harness should close any active serial readers before invoking Ignition and
re-detect ports after flashing, because upload mode can temporarily change
device paths.

Direct PlatformIO commands remain useful for low-level diagnosis:

```bash
cd firmware
/Users/shy/.platformio/penv/bin/pio run -e echo-dev --upload-port <port> -t upload
```

### Serial Model

Each badge gets a long-lived serial reader thread:

- all bytes from the badge are timestamped and written to raw per-badge logs;
- all lines are also appended to a merged timeline;
- harness commands are sent through MicroPython REPL input;
- command results are bracketed by unique sentinel markers so C++ log lines can
  interleave without confusing the parser.

Before issuing a command batch, bring the REPL into a known state:

1. Open the serial port at 115200 baud.
2. Send `Ctrl-C` twice, then `\r\n`.
3. Wait briefly, then send `import badge`.
4. Emit unique begin/end sentinels and capture only the lines between them.

Example host-side command wrapper:

```python
token = "hb_000123"
send("print('__HB_BEGIN:%s__')" % token)
send("print(repr(badge.dev('uid')))")
send("print('__HB_END:%s__')" % token)
```

The raw log remains authoritative even when a command parser fails.
Each wait/assertion should record the current per-badge log position and only
match newer lines. This matters for repeated send/receive tests where identical
HTTP or route log lines can appear many times in a single run.

### Firmware Dev Surface

The initial surface is the existing `BADGE_ENABLE_MP_DEV` API in
`firmware/src/micropython/badge_mp_api/mp_api_dev.cpp`:

- `badge.dev("uid")`
- `badge.dev("help")`
- `badge.dev("btn", "up|down|left|right")`
- `badge.dev("fb")`
- `badge.dev("send", peer_uid_hex, peer_ticket_uuid, peer_name, body)`

The framebuffer dump prints `FB BEGIN`, eight page rows `P0` through `P7`, then
`FB END`. It is useful for screenshots and rough screen assertions, but protocol
tests should still assert logs and on-device state.

Feature-specific dev commands are acceptable when they make a protocol test
deterministic. For example, the 1D Chess work used `clear_chess`,
`chess_challenge`, `chess_move`, and `chess_resign` to test challenge, turn,
terminal-state, and notification behavior without depending on menu focus.
Those commands should stay behind `BADGE_ENABLE_MP_DEV`, and the normal `echo`
build must remain free of them.

The harness may also use normal MicroPython functions that already exist:

- `my_uuid()`
- `boops()`
- `ir_start()`, `ir_stop()`, `ir_send()`, `ir_read()`
- `ir_send_words()`, `ir_read_words()`, `ir_flush()`

Add more `badge.dev(...)` commands only when a test cannot be made reliable
through the existing surface.

### Backend State

Use seeded TESTREPLAY75 tickets from `registrationScanner/seed.sql`. The backend
normalizes 12-character hardware badge IDs into UUID v5 values before writing
`tickets.badge_uuid`; the harness must use the same normalization helper or call
the backend helper that already does it.

Useful backend helpers:

- `workflows._shared.normalize_badge_id`
- `workflows.activities.link_badge_to_ticket`
- `workflows.activities.unlink_badge`
- `db.sync_fetchrow`, `db.sync_execute`
- `db.UPSERT_PAIRING_SQL` only for tests that intentionally create contacts
  without physical IR

The physical boop acceptance test must not pre-create the boop row. It should
link two tickets to two real badge IDs, perform the UI-driven boop, then assert
that both local `/boops.json` views and the backend `badge_boops` row converge.

Ping-backed feature tests should clean `badge_pings` rows scoped to their seed
tickets before starting a fresh run. If cleanup is not available, the feature
protocol should provide a terminal action such as resign/finish/dismiss, and
the test should advance both badges into that terminal state before starting a
new scenario. Clearing only the local JSON file or local `seen` list is not a
valid reset because old backend pings can be fetched again.

## Test Groups

### 1. Preflight

Required before any feature test:

1. Run Ignition build/multi-flash preflight for `echo-dev`.
2. Detect exactly two badge serial devices unless the user supplied explicit
   `--port` values.
3. Flash both badges automatically through Ignition unless `--no-flash` is set.
4. Open serial readers and wipe prior hardware logs.
5. Wait for MicroPython REPL/dev API to respond.
6. Read and record each badge UID.
7. Confirm backend bridge can normalize and link/unlink a seed ticket.

### 2. IR Canary

Before running physical boop tests, prove the badges are in a usable physical
arrangement:

1. Badge A starts IR receive.
2. Badge B sends a simple NEC frame or word frame.
3. Badge A reads the expected frame.
4. Repeat B -> A.
5. If either direction fails, fail or skip physical boop tests with a clear
   placement/IR transport error.

### 3. Identity Pairing Smoke

Purpose: prove firmware can fetch identity from the integration backend.

1. Reset test tickets and badge rows in backend state.
2. Link badge A and badge B to distinct TESTREPLAY75 tickets.
3. Reboot both badges.
4. Wait for paired state evidence in serial logs and/or framebuffer.
5. Assert `badge.dev("uid")` still matches the linked badge IDs.

### 4. Physical Boop Acceptance

Purpose: prove the real user path works on physical hardware.

1. Start from clean test backend state.
2. Link both badges to seed tickets.
3. Reboot and wait until both are in the main menu.
4. Send `badge.dev("btn", "right")` to both badges to select the first menu
   item, `Boop`.
5. Wait for both serial logs to show Boop screen entry and IR enabled.
6. Send `badge.dev("btn", "up")` to both badges within a short window.
7. Wait for both badges to reach `PAIRED_OK` or equivalent success evidence.
8. Assert `boops()` on both badges includes the peer.
9. Assert backend `badge_boops` has an active row for the two linked tickets.
10. Save raw logs and a small JSON summary with ticket UUIDs, badge UUIDs,
    ports, and pairing ID.

### 5. Ping-Backed Feature / Turn-Based Game Acceptance

Purpose: prove ping-backed features behave correctly across two physical badges
without relying on fragile menu timing for every protocol step.

1. Start from clean test backend state for two linked seed tickets.
2. Confirm the real screen is reachable through the menu with button injection.
3. Use dev-only feature hooks, when present, for deterministic challenge,
   response, move, and terminal-state protocol steps.
4. For each send, assert the sender log includes an HTTP status such as
   `http=200 ok=1`.
5. On the receiver, wait from a fresh per-badge log cursor for the matching
   `route ... actionable=...` log line. Capture that receiver cursor before
   issuing the sender command, because the receiver can route the ping before
   the sender-side HTTP wait completes.
6. Read the on-device JSON/state file after each major step and assert active,
   completed, notification, and turn counts.
7. Prefer at least one natural full-game run to terminal mate/draw, then assert
   `next=255` and matching terminal JSON on both badges.
8. Also test end/resign cleanup and assert no active game remains for either
   badge.
9. Confirm the top-level menu notification count only appears when the peer has
   an actionable turn.

### 6. Clean-State / Unpair Acceptance

Purpose: lock the production invariant.

1. Pair/link a badge and ensure it has non-empty boop/contact state.
2. Unlink the badge from backend state.
3. Reboot or wait for the firmware pairing poll.
4. Assert the badge returns to unpaired/QR state.
5. Assert local stale files are removed, especially `/boops.json` and pending
   boop API files.

This is expected to expose a firmware gap if `BadgePairing.cpp` clears NVS and
`/badgeInfo.txt` but leaves boop journals on disk.

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

The harness should delete or replace `logs/hardware/latest` at the start of a
test pass. It should not delete historical `runs/` unless the user asks.

## Non-Goals

- No backend correctness testing. The backend already has its own suite.
- No scanner UI automation for normal firmware tests.
- No destructive hardware assumptions, sacrificial devices, over-voltage tests,
  or power cycling hardware by default.
- No CI requirement. These tests are local hardware tests and should be marked
  separately from normal build/test gates.

## Open Questions

- Which generic filesystem inspection helpers, if any, should be promoted into
  `badge.dev(...)` beyond feature-specific hooks. 1D Chess showed that protocol
  hooks are useful, but file remove/list/read commands should still be added
  only if a concrete harness test needs them.
- Whether the backend bridge should expose a dedicated scoped ping cleanup helper
  instead of relying on ad hoc SQL for `badge_pings` cleanup.
- Whether Ignition's default firmware+filesystem upload should always run before
  hardware tests, or whether feature work usually wants `--no-build`/`--no-flash`
  after the first pass.
- Whether to store known badge USB serial numbers in a checked-in sample manifest
  or keep all device identity in local, gitignored config.
