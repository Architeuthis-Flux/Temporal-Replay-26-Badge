# Tasks: Two-Badge Hardware Test Harness

**Input**: [spec.md](spec.md), [plan.md](plan.md), and [docs/hardware-test-harness.md](../../docs/hardware-test-harness.md)

**Goal**: Provide a repeatable local hardware test pass for two physical badges
that can flash firmware through Ignition, prepare backend state, drive badges
over USB serial, capture logs, and verify real physical boop behavior.

## Format: `[ID] [P?] Description`

- **[P]** means the task can run in parallel with other tasks that touch
  different files.
- Base repo is `Temporal-Badge`; backend dependency is sibling repo
  `../registrationScanner`.

---

## Phase 1: Baseline and Repo Scaffold

- [ ] T001 Run `cd ignition && ./start.sh -e echo-dev --build-only` and record the result in the implementation notes. `echo-dev` must compile through Ignition before harness work proceeds.
- [ ] T002 [P] Create `firmware/hardware_tests/README.md` with the quick command, prerequisites, and hardware assumptions.
- [ ] T003 [P] Create `firmware/hardware_tests/harness/` Python package with empty modules: `ports.py`, `serial_repl.py`, `ignition.py`, `flashing.py`, `backend.py`, `timeline.py`.
- [ ] T004 Add a dedicated hardware-test Python environment with `pytest` and `pyserial` without relying on PlatformIO's penv. Prefer a local documented `uv run --with pytest --with pyserial ...` command or `firmware/hardware_tests/requirements.txt`.
- [ ] T005 Add pytest discovery for hardware tests without affecting normal firmware builds. Prefer a local `firmware/hardware_tests/pytest.ini` or documented command over changing repo-wide test behavior.

## Phase 2: Serial and Logging Core

- [ ] T006 Implement port detection in `hardware_tests/harness/ports.py`, using the same USB serial heuristics as `firmware/serial_log.py`: `usbmodem`, `usbserial`, `ttyACM`, `ttyUSB`, and stable USB serial metadata when available.
- [ ] T007 Implement explicit port selection via CLI options `--badge-a-port` and `--badge-b-port`; auto-detect only when exactly two badge ports are present.
- [ ] T008 Implement per-badge serial reader threads in `serial_repl.py`, with timestamped raw log writes, reconnect handling, and passive raw capture before any parsing.
- [ ] T009 Implement command framing in `serial_repl.py`: unique begin/end sentinels, command timeout, captured interleaved lines, and per-command/per-badge cursor marks so old log lines cannot satisfy new waits.
- [ ] T010 Implement helpers for a generic `badge.dev(...)` call, `badge.dev("uid")`, `badge.dev("help")`, `badge.dev("btn", ...)`, framebuffer dump parsing (`FB BEGIN`, `P0`-`P7`, `FB END`), `my_uuid()`, and `boops()`.
- [ ] T011 Implement `timeline.py` to write `manifest.json`, `timeline.log`, `commands.jsonl`, and per-badge raw logs under `firmware/logs/hardware/runs/<timestamp>/`; update `latest` at pass start.
- [ ] T012 Ensure every test pass wipes `firmware/logs/hardware/latest` before starting while preserving older `runs/`.

## Phase 3: Ignition Flashing

- [ ] T013 Implement `ignition.py` command wrapper for `cd ignition && ./start.sh -e echo-dev --build-only`; this verifies the Ignition build path without touching hardware.
- [ ] T014 Implement `ignition.py` default multi-flash wrapper for `cd ignition && ./start.sh -e echo-dev`; this should flash firmware and filesystem to all connected badges in parallel.
- [ ] T015 Add a guard that closes the harness serial readers before invoking Ignition, then re-detects ports, reopens readers after the ports return, and re-validates `badge.dev("help")` on both badges.
- [ ] T016 Add optional `--no-flash`, `--no-build`, and `--env` flags so feature work can reuse an already flashed build. `--no-build` should call `./start.sh -e <env> --no-build`; `--no-flash` should skip Ignition entirely.
- [ ] T017 Add an Ignition watch-mode smoke path: `./start.sh -e echo-dev --watch --no-build --watch-max-badges 2`, documented as an explicit maintenance check rather than the default test path.
- [ ] T018 Keep `flashing.py` as a direct PlatformIO fallback for one-port debugging only: `pio run -e <env> --upload-port <port> -t upload`.

## Phase 4: Backend Bridge

- [ ] T019 Implement `backend.py` as a subprocess bridge into `../registrationScanner` using `uv run python ...`; do not duplicate backend DB config in the firmware repo.
- [ ] T020 Add backend helper command to normalize a 12-character hardware UID using `workflows._shared.normalize_badge_id`.
- [ ] T021 Add backend helper command to pick two currently usable TESTREPLAY75 seed tickets from `seed.sql`/DB state.
- [ ] T022 Add backend helper command to link badge A/B hardware IDs to seed tickets via `workflows.activities.link_badge_to_ticket`.
- [ ] T023 Add backend helper command to unlink badges/tickets via `workflows.activities.unlink_badge`.
- [ ] T024 Add backend helper command to delete test-created `badge_boops`, `badge_pings`, `pending_virtual_boops`, and sessions for the selected seed tickets only. For ping-backed features, prefer this backend cleanup over clearing local `seen` state.
- [ ] T025 Add backend assertion helper for active pairing existence between two selected tickets.

## Phase 5: Preflight Tests

- [ ] T026 Create `test_smoke.py::test_ignition_echo_dev_builds` or an equivalent setup gate that runs/records `./start.sh -e echo-dev --build-only`; when firmware dev hooks change, also run and record `/Users/shy/.platformio/penv/bin/pio run -e echo` so the normal `echo` image stays clean.
- [ ] T027 Create `test_smoke.py::test_ignition_multiflash_two_badges` that runs the default Ignition flash path when `--no-flash` is not set and records the Temporal workflow result.
- [ ] T028 Create `test_smoke.py::test_two_badges_detected` requiring exactly two badges unless explicit ports are supplied.
- [ ] T029 Create `test_smoke.py::test_dev_api_responds` that opens both serial streams after flashing, calls `badge.dev("help")`, records the full help output for both badges, and asserts the dev command list includes `btn` and `uid`.
- [ ] T030 Create `test_smoke.py::test_badge_uids_are_distinct` that records both `badge.dev("uid")` values in the run manifest.
- [ ] T031 Create `test_smoke.py::test_backend_bridge_can_link_and_unlink` using one seed ticket and one physical badge UID.

## Phase 6: IR Canary

- [ ] T032 Implement `test_boop_physical.py::test_ir_canary_a_to_b` using MicroPython IR receive/send helpers.
- [ ] T033 Implement `test_boop_physical.py::test_ir_canary_b_to_a`.
- [ ] T034 Make physical boop tests depend on the canary result and fail with an actionable placement/IR transport message when it fails.

## Phase 7: Physical Boop Acceptance

- [ ] T035 Implement setup that links two distinct TESTREPLAY75 tickets to the two real badge UIDs and reboots both badges.
- [ ] T036 Add a wait helper that detects paired main-menu readiness from serial logs and/or framebuffer dumps.
- [ ] T037 Drive the UI path on both badges: send RIGHT to enter the first menu item, then wait for `GUI: BoopScreen entered, IR enabled`.
- [ ] T038 Send UP to both badges in a narrow timing window and wait for both badges to report boop success (`PAIRED_OK`, result screen, or stable local boop JSON).
- [ ] T039 Assert `boops()` on badge A contains badge/ticket B and `boops()` on badge B contains badge/ticket A.
- [ ] T040 Assert backend state contains one active `badge_boops` row for the two linked tickets.
- [ ] T041 Write a compact JSON result summary with ports, hardware UIDs, normalized badge UUIDs, ticket UUIDs, and pairing ID.

## Phase 8: Clean-State Acceptance

- [ ] T042 Implement `test_clean_state.py::test_backend_unlink_returns_badge_to_unpaired_state`.
- [ ] T043 Add a fixture that creates stale local boop state through a real or direct setup path before unlinking.
- [ ] T044 Assert that after backend unlink and reboot, firmware returns to QR/unpaired state.
- [ ] T045 Assert stale `/boops.json` and pending boop API files are gone or `boops()` returns an empty pairings list with clear evidence that the file was reset.
- [ ] T046 If T045 fails, patch firmware cleanup in the identity unpair path so NVS, `/badgeInfo.txt`, `/boops.json`, and pending boop API journals are cleared together.

## Phase 9: Documentation and Agent Workflow

- [ ] T047 Update `docs/hardware-test-harness.md` with the final command line and any changed paths.
- [ ] T048 Update `README.md` if the command or docs path changes.
- [ ] T049 Update `CLAUDE.md` agent instructions if the workflow changes.
- [ ] T050 Add troubleshooting notes for serial-port contention, stale ports after flashing, Ignition/Temporal worker failures, bad IR placement, and backend seed exhaustion.

## Phase 10: Final Verification

- [ ] T051 Run `cd ignition && ./start.sh -e echo-dev --build-only`.
- [ ] T052 Run `cd ignition && ./start.sh -e echo-dev` with two connected badges and confirm both badges complete firmware + filesystem flashing.
- [ ] T053 Run the hardware preflight on two connected badges.
- [ ] T054 Run the physical boop acceptance test on two connected badges.
- [ ] T055 Run the clean-state acceptance test.
- [ ] T056 Confirm `firmware/logs/hardware/latest/` contains per-badge raw logs, merged timeline, command transcript, manifest, and test summary.

## Phase 11: Ping-Backed Feature and USB Serial Lessons

- [ ] T057 Add a reusable wait helper that starts from a fresh per-badge log cursor and waits for a regex in only the new serial output, with the matched line attached to the test artifact.
- [ ] T058 Add a reusable assertion helper for ping-backed sends: capture sender and receiver cursors before issuing the send, then verify the sender HTTP status log, the receiver route/actionable log, and an on-device JSON/state snapshot.
- [ ] T059 Add optional discovery for feature-specific dev commands from `badge.dev("help")`; tests that need commands such as `clear_chess`, `chess_challenge`, `chess_move`, or `chess_resign` should skip with a clear message if the flashed image does not expose them.
- [ ] T060 Add a deterministic turn-based feature example test using the 1D Chess dev hooks: clear or terminally close old active state, send a challenge, accept by making the first move, play alternating moves to a natural mate/draw terminal result, and assert both badges converge on matching terminal JSON.
- [ ] T061 Verify that a receiver can ingest a terminal ping even when it has no active local game, and that old lower-sequence backend pings do not recreate an active game.
- [ ] T062 Verify notification behavior for ping-backed games: the top-level menu shows an outstanding count only when the game is actionable for that badge, and the count clears after move/resign completion.
- [ ] T063 Add a UI smoke for the game screen separately from the protocol test: drive the menu with `badge.dev("btn", ...)`, confirm the screen opens, and capture a framebuffer dump without depending on UI focus for every protocol step.
- [ ] T064 Document the serial testing rule of thumb in the harness README: UI driving is for acceptance smoke, feature-specific dev hooks are for deterministic protocol/storage testing, and both `echo-dev` plus production `echo` must build.

## MVP Stop Point

The first useful landing point is T001-T031 plus T011/T012 logging and T057.
That gives future feature work Ignition-backed multi-flash, serial control,
cursor-safe log waits, command transcripts, and backend state setup even before
the physical boop, ping-backed feature, and clean-state tests are complete.
