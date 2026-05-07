# Planning index — future work

One plan doc per focused workstream. Each sub-doc is scoped to fit a
single context window with enough file citations and data model
context that the next agent can start work without re-discovering
architecture. Read only the sub-doc relevant to the next chat; this
index is bookkeeping.

## Workstreams

1. [`01-backend-integration.md`](./01-backend-integration.md) —
   Bring up the `replayBadgeBackend` locally, verify the firmware's
   `/api/v1/pings` wire contract against the server, HMAC enrollment
   plumbing, canned test flows. **Useful early** because it unblocks
   end-to-end testing of the other three streams.

2. [`02-ir-multiframe-parity.md`](./02-ir-multiframe-parity.md) —
   Reliable notification delivery + adjacent IR work. Six parts:
   (A) persistent `/outbox.json` with 2-min retry, IR + API fired
   in parallel from t=0, API moved to a Core 1 background task;
   (B) MANIFEST/DATA/NEED chunked multi-frame IR for long bodies
   mirroring the boop v2 protocol; (C) multicast fan-out to up to
   4 recipients per send (thread store already supports it via
   `kMaxParticipants=4`); (D) wire the "Messages" main-menu entry
   to the existing thread-list surface; (E) apply outbox-style
   retry to `recordBoopOnce`'s server-side reconciliation so
   offline boops actually sync once WiFi returns; (F) new
   `IR_ANYCAST` wire type for untrusted installation broadcasts
   (exhibits, kiosks, queue terminals) with a strict allowlist of
   action codes, per-installation rate limiting, and a separate
   `/anycast_log.json` journal that never mixes with real peer
   data. Cross-transport dedup stays on the shared `msg:<token>`
   key.

   Status: Part A shipped on both charlie and delta. Part B
   kickoff + harness-usage instructions live in the sibling doc
   [`02b-part-b-kickoff.md`](./02b-part-b-kickoff.md) — that's
   the doc to open for a Part B chat that needs to drive the
   two-badge test setup without Kevin present.

3. [`03-chat-bubble-ui.md`](./03-chat-bubble-ui.md) — Refactor
   `ThreadDetailScreen` rendering into right-justified (outgoing) vs
   left-justified (incoming) chat bubbles with rounded corners, à la
   iMessage / phone SMS. Research u8g2 helper libraries that match
   the display driver we already use.

4. [`04-keyboard-polish.md`](./04-keyboard-polish.md) — TextInput
   improvements: Cancel button, Settings-switchable Grid vs QWERTY,
   mouse mode as a proper toggle, emoji layer via u8g2 unifont
   glyphs, canned-message presets ("where you at?"). General visual
   polish on the keyboard surface.

## Shared conventions (every chat should follow)

- **Test loop.** See [`../AGENTS.md`](../AGENTS.md). Flash both
  badges in parallel with explicit `--upload-port`, then `cat` each
  port into `/tmp/serial_<name>.log` for `tail`/`grep`.
- **Clangd noise.** `ArduinoJson` template errors are the baseline
  clangd false-positive pattern across the repo; real build errors
  show up when `pio run -e charlie` fails. Don't chase the template
  complaints — they don't block the actual toolchain.
- **Docs.** When a subsystem changes architecturally, update its
  `codeDocs/*.md` file as the last step — this is how the next
  agent bootstraps.
  - [`../codeDocs/BoopSystem.md`](../codeDocs/BoopSystem.md)
  - [`../codeDocs/NotificationSystem.md`](../codeDocs/NotificationSystem.md)
- **Threading.** IR-adjacent state (`irTask`, `SendBoopBackground`,
  `BadgeNotifications::addIncoming`) runs on Core 0; GUI runs on
  Core 1. FAT writes use a FreeRTOS mutex (NOT `portMUX`) because
  the flash I/O is long enough to trip the interrupt watchdog.
- **Cross-transport dedup contract.** IR frames carry a 32-bit
  message token in TLV tag `0x05`; API pings carry the same value
  in `data.msg_id`. Receiver's store dedup key is `"msg:<hex>"`.
  Tag and key definitions in
  [`../src/BadgeNotifications.cpp`](../src/BadgeNotifications.cpp)
  `irNotifyFrameDispatch` and
  [`../src/BadgeIR.cpp`](../src/BadgeIR.cpp) `irBeginNotify`.

## Suggested order

`01` → `02` → `03` → `04`. Each is independent, but having the
backend live makes the parity-send test in `02` observable, and
getting `02` on stable wire before polishing bubbles in `03` means
the UI gets real long messages to lay out instead of 30-char toys.
