# spec-012: Two-Badge Hardware Test Harness

Build a local hardware test harness that can flash, control, observe, and verify
two physical badges over USB serial while using the real integration backend.

## User Stories

### Story 1: Flash and Identify Two Badges

As a firmware developer, I want one command to flash both connected badges and
confirm both are running the expected dev image, so I do not lose time debugging
a stale firmware image on one side.

Acceptance:

1. The harness flashes both badges through Ignition using `echo-dev` by default.
2. It closes any serial readers before flashing, then re-detects and reopens the
   ports afterward.
3. It calls `badge.dev("help")` on both badges after every flash and records the
   full help output.
4. It records each badge's hardware UID from `badge.dev("uid")`.
5. It auto-detects ports only when exactly two badge ports are present; otherwise
   the user must provide explicit ports.

### Story 2: Capture Reliable USB Serial Evidence

As a firmware developer, I want tests to preserve raw serial logs and parse REPL
commands safely, so C++ logs and MicroPython output cannot hide each other.

Acceptance:

1. Each badge has a passive reader that writes timestamped raw output before any
   parsing.
2. REPL commands are framed with unique begin/end sentinels.
3. Before command batches, the harness sends `Ctrl-C` twice, sends `\r\n`, waits
   briefly, and imports `badge`.
4. Every wait starts from a fresh per-badge log cursor. Tests must not search the
   whole accumulated log for success markers.
5. Raw logs, command transcripts, a merged timeline, and a manifest are saved for
   each run.

### Story 3: Verify Real UI Paths Separately From Protocol Paths

As a firmware developer, I want UI smoke tests and deterministic protocol tests
to be separate, so menu focus and tutorial state do not make protocol debugging
unreliable.

Acceptance:

1. UI acceptance tests may use `badge.dev("btn", ...)` to navigate real screens.
2. Protocol/storage tests may use feature-specific `badge.dev(...)` hooks behind
   `BADGE_ENABLE_MP_DEV` when button driving is too brittle.
3. Dev-only hooks must not be required by the production `echo` image.
4. Firmware changes that add dev hooks must still build both `echo-dev` and
   `echo`.
5. `badge.dev("fb")` may be used for visual smoke; it returns `FB BEGIN`, eight
   rows `P0` through `P7`, then `FB END`.

### Story 4: Prepare and Clean Backend State

As a firmware developer, I want the harness to create only the backend state a
test needs, so test badges can be reused without stale ticket, boop, or ping
state corrupting results.

Acceptance:

1. The harness uses TESTREPLAY75 seed tickets from the backend.
2. It normalizes hardware badge IDs the same way the backend does before linking
   tickets.
3. It links and unlinks badges through backend helpers rather than duplicating DB
   configuration in the firmware repo.
4. It deletes test-created backend rows only for the selected seed tickets.
5. For ping-backed features, cleanup includes scoped `badge_pings` rows when
   available.

### Story 5: Test Ping-Backed Turn-Based Features

As a firmware developer, I want a repeatable two-badge test for ping-backed
features, so games and notifications can be verified without manual timing.

Acceptance:

1. The harness starts from two linked tickets with a known peer relationship.
2. It drives the visible game entry path far enough to prove the screen is
   reachable.
3. It may then use dev-only hooks such as `clear_chess`, `chess_challenge`,
   `chess_move`, or `chess_resign` for deterministic protocol steps.
4. Before each send, it records both the sender and receiver log cursors.
5. For each send, it asserts the sender log includes a successful HTTP status.
6. On the receiver, it waits from the pre-send cursor for the matching route log
   and expected `actionable` value.
7. It reads on-device state after important steps and verifies active,
   completed, turn, and notification state.
8. It runs at least one game to a natural terminal result, such as mate or draw,
   and verifies both badges converge on the same terminal sequence, board, and
   result.
9. It also ends a game with a terminal action, such as resign, and verifies no
   active game remains for either badge.
10. It does not clear only local `seen` state as a reset, because old backend
   pings can be fetched again.

### Story 6: Verify Clean-State Behavior

As an event operator, I want a badge that loses its paired identity to clear
stale local state, so replacing or reassigning badges does not preserve someone
else's contacts or boops.

Acceptance:

1. After backend unlink, firmware returns to QR/unpaired state.
2. Local identity, contacts, boops, pending boop API files, and related journals
   are removed or reset.
3. Re-pairing the same attendee to a new badge preserves backend relationships
   through the ticket identity, not through stale local badge files.

## Requirements

- The harness MUST run from `Temporal-Badge` and may call the sibling
  `../registrationScanner` repo for backend helpers.
- The harness MUST target `https://brooklyn.party` for integration tests unless
  explicitly overridden.
- The harness MUST preserve per-badge raw logs even when command parsing fails.
- The harness MUST distinguish old serial output from new output by using
  per-badge cursors or snapshot marks.
- The harness MUST record the firmware env, git SHA when available, USB ports,
  hardware UIDs, normalized backend badge UUIDs, and ticket UUIDs in the run
  manifest.
- The harness SHOULD wait on serial evidence such as route logs instead of fixed
  sleeps, especially for Wi-Fi ping polling that can take up to 30 seconds.
- The harness SHOULD use explicit ports when known and only auto-detect when the
  result is unambiguous.
- The harness SHOULD keep UI smoke tests small and use deterministic dev hooks
  for deep protocol/storage behavior.

## Non-Goals

- Backend correctness testing.
- Scanner UI automation.
- CI gating.
- External power relay control.
- Destructive hardware tests.
