# 02 — Reliable notification delivery + anycast + menu entry

Goal: make outgoing notifications behave like a real message
queue instead of one-shot best-effort, lift the long-message
IR ceiling, wire a menu entry to the existing thread list,
fold reliability improvements back into the boop pairing API
reconciliation, and add a new broadcast ("anycast") frame type
for exhibits/kiosks that doesn't require a UID handshake.

Six parts, each implementable independently; suggested order
A → D → B → C → E → F.

1. **Part A — persistent outbox + always-parallel send.** Every
   outgoing boop lands in `/outbox.json` the moment the user
   submits, and fires IR + API in parallel from t=0. IR retries
   run on a background schedule (not blocking the GUI) for up
   to 2 min, and continue until every intended recipient has
   confirmed (IR ACK or API 200). Entries survive reboots.
   Duplicates on the receiver are already handled via the
   shared `msg:<token>` dedup key.

2. **Part B — IR multi-frame via a MANIFEST protocol.** Same
   shape as the boop v2 streaming protocol (`IR_BOOP_MANIFEST`
   / `IR_BOOP_DATA` / `IR_BOOP_NEED` in
   [`../src/BadgeIR.h`](../src/BadgeIR.h) L66–L71). Today's
   120-byte payload cap in
   [`../src/BadgeIR.cpp`](../src/BadgeIR.cpp) `irBeginNotify`
   truncates anything longer; a manifest header + chunked DATA
   frames + receiver-driven NEED repair lifts that cap cleanly.

3. **Part C — multicast up to 4 recipients.** The thread store
   already supports `kMaxParticipants = 4` with a hashed
   `thr:g<fnv32>` id (see
   [`../src/BadgeNotifications.cpp`](../src/BadgeNotifications.cpp)
   L225–L254). Outbox entry carries an array of targets; IR
   fans out one frame per target; API loops `sendPing` once
   per target.

4. **Part D — Messages menu entry.** Today
   [`../src/GUI.cpp`](../src/GUI.cpp) L211 has a placeholder
   `onMessagesSelected` that just logs "not yet ported". Wire
   it to navigate into the same thread list Home currently
   renders, so the menu item becomes the canonical "inbox"
   entry regardless of what Home eventually displays.

5. **Part E — boop pairing reliability crossover.** Apply the
   outbox-style retry pattern from Part A to `recordBoopOnce`
   in [`../src/BadgeBoops.cpp`](../src/BadgeBoops.cpp) L1186 so
   offline-recorded boops actually reconcile with the server
   once WiFi returns, instead of living forever as
   `boopStatus.offlineBoop = true` local-only records. IR-side
   handshake reliability is already pretty good — no wire
   changes there.

6. **Part F — anycast frames.** New wire type `IR_ANYCAST` for
   installations (exhibits, kiosks, queue terminals, schedule
   cues) that broadcasts an action + payload without a UID
   handshake. Explicitly untrusted: badges only execute an
   allowlisted set of side-effect-visible actions, rate-limit
   per installation id, never ACK, and tag the resulting
   journal entries so they can't masquerade as real
   peer-to-peer boops.

Already done — do not regress: **target-UID filtering before
ACK** is enforced transport-side in
[`../src/BadgeIR.cpp`](../src/BadgeIR.cpp) `isSelfEcho`
L89–101. A NOTIFY frame whose `words[1..2]` doesn't match our
`uid[]` never reaches `irNotifyFrameDispatch`, so we cannot
ACK someone else's notify. Add a regression test in Part A's
bring-up and move on. Anycast in Part F is explicitly
exempted from this filter because it has no target UID —
handled separately.

## Decisions (settled)

- **Ship all six parts in this track.** Order A → D → B → C →
  E → F. A and D together make the current single-peer short-
  message path genuinely reliable and give the menu item a
  real destination; everything after that is protocol growth.
- **2-min retention as a hardcoded constant** —
  `kOutboxMaxAgeMs = 120 * 1000`. Not surfaced to
  `badgeConfig`. Good enough signal-to-noise and one fewer
  setting to explain.
- **No back-compat hedging on protocol version bumps.** The
  notification wire hasn't shipped; Part B can introduce new
  frame types without a fallback policy for old receivers.
  `kNotifyProtocolVer` still bumps to `0x02` for the new
  frame shapes, but no "old receiver graceful degradation"
  code is needed.
- **One haptic pulse per send, total.** Fires on the FIRST
  confirmation of the FIRST target to come back. Multicast
  doesn't stack pulses; IR+API parallel doesn't double-pulse.

## Context — what exists

### Wire layout today

Single-frame IR_NOTIFY — see
[`../src/BadgeIR.h`](../src/BadgeIR.h) L73–L93 +
[`../src/BadgeIR.cpp`](../src/BadgeIR.cpp) `irBeginNotify`:

```
word0       : 0xD0 | kind<<8 | ver<<16 | flags<<24
words[1..2] : TARGET UID (6 bytes LE) — receiver filter
words[3..]  : TLV payload packed 4 bytes per word:
   0x01 = title   (UTF-8)
   0x02 = body    (UTF-8)
   0x03 = source UID (6 B, reply target)
   0x04 = 16-bit retry nonce (LE)
   0x05 = 32-bit msg token   (LE, cross-transport dedup key)
```

Hard hardware ceiling: `NEC_MAX_WORDS = 64`, 3-word envelope →
up to 61 words = 244 B raw per frame. The existing sender caps
`payload[]` at 120 B inside `irBeginNotify`, same conservative
budget the boop v2 protocol uses for its DATA frames (see
[`../codeDocs/BoopSystem.md`](../codeDocs/BoopSystem.md)
§RMT hardware tuning for why 120 is safe and 240 tends to drop).

After title+source+nonce+token TLV overhead (~1+1+8+2+2+2+2+4
= ~22 B), the realistic single-frame body limit is ~90-100
characters of ASCII.

### ACK today

[`../src/BadgeIR.cpp`](../src/BadgeIR.cpp) `irSendNotifyAck`:

```
word0       : 0xD1 | kind<<8 | ver<<16
words[1..2] : original sender UID (so their recvFrame filter passes)
word3       : low 16 bits = nonce echoed from the NOTIFY
```

`processNotifyAck` matches on nonce against a single
`s_pendingNotify` slot. Only one outgoing notify in flight at a
time — strictly sequential UI flow.

Retry: `kNotifyTxRetryMs = 500 ms`, `kNotifyTxDeadlineMs = 5000
ms`. After 5 s without an ACK → `IR_NOTIFY_TX_FAILED`, the UI
falls through to a blocking `BadgeAPI::sendPing` in
[`../src/GUI.cpp`](../src/GUI.cpp) `SendBoopBackground::tick`.

### Reference: boop v2 manifest protocol

Copy its SHAPE for notifications; do NOT literally reuse the
state machine.  Read
[`../src/BadgeBoops.cpp`](../src/BadgeBoops.cpp) `serveStreamReq`
L2140–L2207 for the DATA packing, `encodeNeed` / `decodeNeed`
L1012–L1030 for NEED repair, and `XchgPhase` L786–L794 for the
phase transitions.  Frame types already reserved in
[`../src/BadgeIR.h`](../src/BadgeIR.h) L66–L71:

```
IR_BOOP_MANIFEST   0xC0  // "here's what I'll send"
IR_BOOP_STREAM_REQ 0xC1  // "send me everything now"
IR_BOOP_DATA       0xC2  // batched TLVs, hasMore bit
IR_BOOP_NEED       0xC3  // "resend (tag,chunk)"
IR_BOOP_FIN        0xC4  // close
IR_BOOP_FINACK     0xC5  // close-ack
```

Retransmit tuning lives in
[`../src/BadgeBoops.cpp`](../src/BadgeBoops.cpp) L783:
`kRetxMsSlow = 2800 ms` when the expected response is a DATA
burst (must exceed one frame's wire time or TXes collide half-
duplex); `kRetxMsFast = 1000 ms` for short MANIFEST/FIN replies.
Bake these numbers in — real-world derived.

### Receiver dedup key

[`../src/BadgeNotifications.cpp`](../src/BadgeNotifications.cpp)
`irNotifyFrameDispatch` L710:

- TLV 0x05 present → id = `"msg:<8-hex-token>"` (preferred,
  shared with API path)
- Fallback → `"ir:<nonce><kind>"`
- Final fallback → 16-bit FNV hash of the whole frame

[`../src/WiFiService.cpp`](../src/WiFiService.cpp)
`WiFiService::pollNotifications` L251: if `data.msg_id` in the
ping payload → `"msg:<msg_id>"`; else `"ping:<server-id>"`.

`addIncoming(peerUid, peerName, id, title, body, createdAt)`
dedups on `id` inside the thread
([`../src/BadgeNotifications.cpp`](../src/BadgeNotifications.cpp)
`appendMessage` L289–L315).  Duplicate → returns false, no
haptic, no save.  **Current behaviour: first arrival wins** — a
later-arriving IR frame cannot overwrite an earlier API-delivered
record (or vice versa).

### Thread store already supports multicast

[`../src/BadgeNotifications.h`](../src/BadgeNotifications.h)
L36: `kMaxParticipants = 4`.
[`../src/BadgeNotifications.cpp`](../src/BadgeNotifications.cpp)
`threadIdForPeers` L378 takes a `const char* const* peerUids` +
count.  `computeThreadId` L225 produces `thr:g<fnv32>` for N>1
participants, `thr:<12-hex-uid>` for N=1.  `findOrCreateThread`
L258 seeds every participant + name into the JSON doc.  Every
message inside a thread is attributed via `senderUid`, so a
multicast reply from peer C in a (me, A, B, C) thread still
lands in the same thread.

UI consequence: HomeScreen and ThreadDetailScreen only exercise
the single-peer path today.  Multicast threads render, but the
title/preview code at
[`../src/GUI.cpp`](../src/GUI.cpp) `threadDisplayName` (search
it) shows only the first participant.  Display polish is out of
scope for this plan — call it out in the multicast testing
section and leave it to `03-chat-bubble-ui.md`.

### Send flow today

[`../src/GUI.cpp`](../src/GUI.cpp) `SendBoopBackground` L2407–
L2707 — single-slot, strictly linear:

```
Idle
 └─ beginAfterBody  (generates msgToken, addOutgoing, irBeginNotify)
     └─ SendingIr
         ├─ irNotifyTxStatus==ACKED  → short haptic → Idle
         └─ irNotifyTxStatus==FAILED → SendingApi → sendViaApi (blocks 2-5 s)
```

Pain points the new design has to fix:

- API only fires AFTER IR timeout — user waits ~5 s in IR retry
  before the WiFi rail gets a chance.
- `sendViaApi` is blocking from within `tick()`, which runs in
  the GUI render loop.  2-5 s freeze.
- No persistence — reboot mid-send loses the message.
- Single target — no multicast.
- Body > ~90 chars is silently truncated at `irBeginNotify`'s
  120 B payload cap.

## Goals — restated crisply

1. **Outbox persistence.**  Every submitted boop is written to
   `/outbox.json` before either rail fires.  Entries stay until
   every recipient has confirmed or the 2-min retention ceiling
   fires.  Survives reboot.
2. **Parallel rails from t=0.**  IR retry loop + API POST start
   on the same GUI tick.  API path runs on a Core 1 background
   task so `tick()` never blocks.
3. **Long IR messages via chunked manifest.**  New frame types
   under `kNotifyProtocolVer = 0x02`.  Short messages keep the
   single-frame `IR_NOTIFY` fast path (~250 ms); long messages
   use MANIFEST + DATA + NEED repair.  No legacy fallback code
   — notifications haven't shipped.
4. **Multicast up to 4 targets.**  One outbox entry → N IR
   recipients + N API POSTs.  Entry closes when every target
   has confirmed on at least one rail.
5. **Messages menu entry reaches the thread list.**  Wire the
   placeholder `onMessagesSelected` to navigate into the
   existing thread list so the menu item becomes a real inbox
   entry point.
6. **Offline boops reconcile when WiFi returns.**  Apply the
   outbox retry pattern to `recordBoopOnce`'s server call so
   `offlineBoop = true` records aren't stranded forever.
7. **Anycast = untrusted broadcasts.**  New `IR_ANYCAST` frame
   type for exhibits/kiosks/queue terminals.  Allowlist of
   side-effect-visible actions only, per-installation rate
   limit, separate journal, never mixes with real peer data.
8. **Preserve target-UID-filtered ACK.**  No regression; add
   an explicit test that a NOTIFY frame with wrong target UID
   is silently dropped at the transport layer without an ACK
   on the wire.  Anycast in Part F is exempt — no target UID
   by design.

## Research to do first

- [`../codeDocs/BoopSystem.md`](../codeDocs/BoopSystem.md) §RMT
  hardware tuning + §Exchange + Open Work section.  The
  multi-frame back-to-back TX reliability bug mentioned in Open
  Work is live here too — any design that TXes two long frames
  ~10 ms apart will hit it.
- [`../src/BadgeBoops.cpp`](../src/BadgeBoops.cpp) — grep
  `serveStreamReq`, `processDataFrame`, `sendNeedFor`,
  `kRetxMs`.  Reference only, don't copy wholesale.
- [`../src/hw/ir/nec_mw_encoder.h`](../src/hw/ir/nec_mw_encoder.h)
  L16: `NEC_MAX_WORDS = 64`.  Frame ceiling.
- [`../src/BadgeBoops.cpp`](../src/BadgeBoops.cpp) L45–L82 —
  the `/boops.json` journal.  Same mutex + atomic-rename pattern
  the outbox should use, FreeRTOS mutex NOT portMUX.
- [`../src/BadgeAPI.cpp`](../src/BadgeAPI.cpp) `sendPing` L395–
  L444 — single-target POST.  Multicast = N POSTs; no server
  change.

## Design

Six parts, each implementable independently.  Suggested order:
**A → D → B → C → E → F**.  A is the load-bearing correctness
win and can ship standalone; D is a trivial follow-on that
gives the menu entry a real destination; B/C grow the protocol;
E and F are adjacent reliability/feature work that reuses the
outbox journal plumbing from A.

---

### Part A — Persistent outbox + always-parallel rails

Smallest wire change (none), largest behavioural change.

#### A.1 — On-disk schema `/outbox.json`

```json
{
  "entries": [
    { "token":       "a1b2c3d4",
      "title":       "Boop from Kevin",
      "body":        "...",
      "createdAtMs": 1234567890,
      "firstAttempt": 1234567890,
      "targets": [
        { "uid":"aabbccddeeff", "ticket":"uuid-...",
          "name":"Kate",
          "ir":  { "state":"pending", "attempts":0, "lastTxMs":0 },
          "api": { "state":"pending", "attempts":0, "lastTxMs":0 }
        }
      ]
    }
  ]
}
```

`state` values: `pending | in_flight | confirmed | failed_final`.
An entry can be retired once every target has at least ONE rail
in `confirmed`.  Terminal entries are removed from the journal
(we don't need a send history — the outgoing message is already
recorded in `/notifications.json` via `addOutgoing`).

Retention policy defaults:

```
kOutboxMaxEntries       = 16
kOutboxMaxAgeMs         = 120 * 1000   // 2 min after firstAttempt
kOutboxIrRetryInterval  = 5000         // 5 s between IR retry bursts
kOutboxApiRetryInterval = 30000        // 30 s between API retries
kOutboxIrMaxAttempts    = 24           // 24 × 5 s = 2 min
```

`kOutboxMaxAgeMs` is the "2 min if no wifi" cap from the user
brief.  Once hit, any still-pending rail for that entry flips to
`failed_final` and the entry is retired.  The `addOutgoing`
record in `/notifications.json` stays — the user can see their
message with an (implicit) "delivery unknown" state.  Rendering
that state is a later UI task (`03-chat-bubble-ui.md`).

Journal layout mirrors
[`../src/BadgeBoops.cpp`](../src/BadgeBoops.cpp) `/boops.json`
(L45–L82): `DynamicJsonDocument(8 * 1024)`, FreeRTOS mutex,
write-tmp-rename atomic save.  FAT writes can take tens of ms
and would trip the interrupt watchdog under a `portMUX` spinlock
— use a real mutex.

#### A.2 — New module `BadgeOutbox`

`src/BadgeOutbox.{h,cpp}` — thin layer on top of the JSON doc.
Public surface:

```cpp
namespace BadgeOutbox {
  void begin();                               // load /outbox.json
  uint32_t enqueue(const char* const* peerUids,
                   const char* const* peerTickets,
                   const char* const* peerNames,
                   uint8_t count,
                   const char* title,
                   const char* body);         // returns msgToken
  void markIrConfirmed (uint32_t token, const uint8_t senderUid[6]);
  void markApiConfirmed(uint32_t token, const char* peerTicket);
  void markIrFailed    (uint32_t token, const uint8_t senderUid[6]);
  void markApiFailed   (uint32_t token, const char* peerTicket);
  void tick(uint32_t nowMs);                  // drive retries; called from Core 1
  bool isPending(uint32_t token);
  int  pendingCount();                        // for UI badge
}
```

`enqueue` generates the msgToken via
`BadgeNotifications::newMessageToken()`, writes the outbox
entry, then returns.  Does NOT fire IR/API itself — the caller
(`SendBoopBackground::beginAfterBody`) kicks off both rails on
the same tick.

`tick` is the retry engine.  Runs on a timer from Core 1 (see
A.4).  For each pending entry:

- If `nowMs - firstAttempt > kOutboxMaxAgeMs`, retire entry:
  flip all still-pending rails to `failed_final`, save, remove.
- Otherwise for each target:
  - If `ir.state == pending` and `nowMs - ir.lastTxMs >
    kOutboxIrRetryInterval` and `ir.attempts <
    kOutboxIrMaxAttempts` and `irNotifyTxStatus == IDLE`: fire
    `irBeginNotify(target, ...)` for this (entry, target) pair,
    set `ir.state = in_flight`, bump `ir.attempts`, stamp
    `lastTxMs`.  The receipt of IR_NOTIFY_ACK drives the state
    back to `confirmed`; a TX_FAILED kicks it back to `pending`
    for the next retry cycle.  Serialized — one `irBeginNotify`
    at a time (single-slot sender).
  - If `api.state == pending` and WiFi is up and `nowMs -
    api.lastTxMs > kOutboxApiRetryInterval`: kick an API POST
    via a one-shot FreeRTOS task (A.4).

#### A.3 — Parallel send from `SendBoopBackground`

Rework [`../src/GUI.cpp`](../src/GUI.cpp) L2560–L2707
`SendBoopBackground::beginAfterBody`:

1. Generate `msgToken` via
   `BadgeNotifications::newMessageToken()` (moved into the
   outbox or caller-provided).
2. Call `BadgeOutbox::enqueue(...)` which writes
   `/outbox.json` and returns the same token.
3. Call `addOutgoing(threadId, msgToken, title, body)`
   (unchanged).
4. Fire IR on this tick: `irBeginNotify(target0, title, body,
   msgToken)` — one target only, even on multicast sends, to
   respect the single-slot sender.  The outbox tick picks up
   remaining targets once this one resolves.
5. Fire API in parallel: `xTaskCreatePinnedToCore(...)` spins
   up `sendViaApiTask` on Core 1 (A.4) with a copy of the
   request.  Non-blocking from the GUI side.
6. `tick()` becomes trivial: observe `irNotifyTxStatus`, call
   the outbox mark* helpers, never block.

The blocking `sendViaApi` path and the `SendingIr/SendingApi`
phases go away.  Haptics fires exactly once per entry, on the
transition "all targets confirmed on at least one rail".  If
both rails confirm for the same target, haptic stays single-
pulse.

#### A.4 — Background API worker

New file-scoped helper `sendViaApiTask` spawned per-entry (or
per-entry-per-target for multicast) on Core 1, priority 1, 8 KB
stack — same pattern as
[`../src/BadgeBoops.cpp`](../src/BadgeBoops.cpp)
`attendeePrefetchTask` L845.  Task:

1. Build the same JSON body the current `sendViaApi` does
   (`msg_id` hex = msgToken).
2. Call `BadgeAPI::sendPing(uid_hex, peerTicket, "notification",
   data)`.
3. Post result to the outbox via
   `BadgeOutbox::markApiConfirmed(token, peerTicket)` on 200,
   `markApiFailed` otherwise.
4. `vTaskDelete(nullptr)` self-destructs.

One task in flight per (entry, target) — guard with a bitmap on
the entry so a retry tick doesn't re-spawn while a POST is
still pending.

#### A.5 — Reboot recovery

`BadgeOutbox::begin()` loads `/outbox.json` and resets every
`in_flight` state back to `pending` (an in-flight write that
didn't get ACKed or POST-completed before reboot must be
retried, we have no idea of its fate).  The same `msg:<token>`
dedup key means if the peer already received it, addIncoming
will return false and the ACK will still fire — we'll see
ACKED and retire the entry harmlessly.

#### A.6 — Checklist

- [ ] Create `src/BadgeOutbox.{h,cpp}` with the journal + mutex
      + retry tick.  Mirror
      [`../src/BadgeBoops.cpp`](../src/BadgeBoops.cpp) L45–L82
      for the journal boilerplate.
- [ ] Call `BadgeOutbox::begin()` from `main.cpp` after
      `BadgeNotifications::begin()`
      ([`../src/main.cpp`](../src/main.cpp) L127).
- [ ] Spawn a Core 1 retry-tick task (500 ms cadence; same
      pattern as `WiFiService`'s supervisor).
- [ ] Refactor [`../src/GUI.cpp`](../src/GUI.cpp)
      `SendBoopBackground`: delete `SendingIr`/`SendingApi`
      phases, replace with one-shot `enqueue + fire-both-rails`
      logic.  Haptic = single pulse on first-rail-confirm per
      entry.
- [ ] Move `sendViaApi` into a Core 1 one-shot task
      (`sendViaApiTask`), self-deleting, posting result via
      `BadgeOutbox::markApi*`.
- [ ] Hook `processNotifyAck` to also call
      `BadgeOutbox::markIrConfirmed(token, senderUid)` — needs
      the msgToken threaded through the ACK, which today uses
      nonce.  Either (a) add token echo to the ACK word4 (wire
      change, requires ver bump) or (b) maintain a nonce↔token
      map in BadgeOutbox for the single-frame case.  (b) is
      zero wire change — prefer it.
- [ ] `BadgeOutbox` boot recovery: `in_flight → pending` sweep.
- [ ] Update
      [`../codeDocs/NotificationSystem.md`](../codeDocs/NotificationSystem.md)
      §Sending.

---

### Part B — IR multi-frame via MANIFEST protocol

Bumps `kNotifyProtocolVer` to `0x02`. Nothing has shipped on
the notification wire yet, so there is no graceful-degradation
path required — we can delete dead single-frame code that only
existed as a "short message fallback". In practice: keep
single-frame `IR_NOTIFY` (0xD0) for short messages because
it's cheaper on the wire (~250 ms vs. MANIFEST + DATA +
~500 ms), but treat both branches as v0.2 of one protocol, not
"legacy + new".

Frame type allocations:

```
IR_NOTIFY            0xD0  // short (≤ ~90-byte body) single-frame fast path
IR_NOTIFY_ACK        0xD1  // word3 = msgToken low16 (single-frame) OR
                             word3 = full 32-bit msgToken (multi-frame)
IR_NOTIFY_MANIFEST   0xD2  // "I'm sending msgToken in N chunks totaling L bytes"
IR_NOTIFY_DATA       0xD3  // one chunk body-bytes; seq = chunkIdx
IR_NOTIFY_NEED       0xD4  // receiver requests chunkIdx (bitmap for up to 16)
```

#### B.1 — Sender strategy

In `BadgeOutbox::tick` (or `irBeginNotify` directly — TBD during
implementation which is cleanest), decide per-target:

- If `strlen(body) + strlen(title) + overhead ≤ 90 B`: legacy
  single-frame `IR_NOTIFY`.  No protocol change on the wire for
  this target — receivers on old firmware still work.
- Else: multi-frame path.

Multi-frame flow (mirrors boop v2 but with no symmetric pull):

```
TX  IR_NOTIFY_MANIFEST(token, totalChunks, totalBytes, title_tlv_inline)
    // title fits in the manifest frame because it's small; saves
    // a round trip and lets the receiver populate the thread
    // summary before any DATA arrives

repeat up to 3 times at kNotifyTxRetryMs:
  TX  IR_NOTIFY_DATA(token, chunkIdx=0..N-1, bytes)  // back-to-back
  RX  IR_NOTIFY_ACK(token)        → done
   or IR_NOTIFY_NEED(token, bitmap) → retransmit missing chunks, loop
```

Chunk size: `kNotifyChunkBytes = 100`.  Below the 120 B reliable-
single-frame threshold, so each DATA frame is single-frame
reliable.  16 chunks × 100 B = 1.6 KB max; covers any realistic
message without exceeding `kBodyMax = 160` in the store anyway.

Actually — `kBodyMax = 160` in
[`../src/BadgeNotifications.h`](../src/BadgeNotifications.h) L43
already caps our RECEIVER-SIDE storage at 160 characters.  If
we're not willing to enlarge that (and we shouldn't in this
plan — cascading changes across ThreadDetailScreen and the JSON
doc capacity), then the real budget is `title(40) + body(160)
+ overhead ~30` = ~230 B total payload.  That fits in 3 chunks
of 100 B.  Lock `totalChunks ≤ 4` for headroom, and let
`kBodyMax` be the enforced ceiling in the UI composer.

#### B.2 — MANIFEST frame format

```
word0       : 0xD2 | kind<<8 | ver<<16 | flags<<24
words[1..2] : TARGET UID
word3       : low 16 = msgToken (low16), high 16 = msgToken (high16)
              (full 32-bit msgToken fits in one word)
word4       : totalChunks<<0 | totalBytes<<8 | titleLen<<20
words[5..]  : title bytes (packed 4/word), padded to totalChunks*chunkSize?
              TBD: keep title out of MANIFEST vs. inline.
```

Inline title is a nice-to-have for UI preview before DATA lands;
if the encoding gets awkward, drop it and TX a separate
`IR_NOTIFY` with just the title first (costs one frame, ~250 ms).

#### B.3 — DATA frame format

```
word0       : 0xD3 | kind<<8 | ver<<16 | flags<<24
words[1..2] : TARGET UID
word3       : low16=msgToken_lo, high16=msgToken_hi
word4       : chunkIdx<<0 | chunkLen<<8 | hasMore<<16
words[5..]  : chunk bytes packed 4/word
```

#### B.4 — NEED frame format

```
word0       : 0xD4 | kind<<8 | ver<<16
words[1..2] : SENDER UID (original NOTIFY target, i.e. the receiver
              of MANIFEST is the TXer of NEED, so the sender's UID
              goes here so their recvFrame can match via isSelfEcho)
word3       : full 32-bit msgToken
word4       : low 16 bits = missingChunkBitmap (1 bit per chunkIdx 0..15)
```

Receiver TXes NEED after a short grace window (~500 ms) once it
has seen MANIFEST but is still missing DATA chunks.  Sender's
retry tick sees NEED and re-TXes only the requested chunks.

#### B.5 — Reassembly buffer

On the receiver, a static `notifyReassembler_t` slot keyed on
`(senderUidBytes, msgToken)`.  One slot is enough — concurrent
multi-frame sends from two different senders are rare and
dropping one while another completes is acceptable.  Timeout:
15 s since last activity → drop partial (matches API poll
cadence, so the API copy will fill in any gap).

Once every chunk is received:

1. Concatenate chunk bytes in order.
2. Run the existing TLV walker from
   [`../src/BadgeNotifications.cpp`](../src/BadgeNotifications.cpp)
   `irNotifyFrameDispatch` L722–L798 on the flattened payload.
3. `addIncoming(peerUid, peerName, "msg:<token>", title, body,
   "")` — same call, same dedup.
4. TX `IR_NOTIFY_ACK` with `word3 = msgToken low16` (sender
   matches on msgToken, not nonce, in multi-frame mode).

#### B.6 — No back-compat required

The notification wire hasn't shipped; every badge will be on
`kNotifyProtocolVer = 0x02` from the first build that includes
this work.  Don't write fallback code for 0x01.  The version
field stays on the wire so we have room to bump again later.

#### B.7 — Checklist

- [x] Allocate frame types in
      [`../src/BadgeIR.h`](../src/BadgeIR.h): `IR_NOTIFY_MANIFEST
      = 0xD2`, `IR_NOTIFY_DATA = 0xD3`, `IR_NOTIFY_NEED = 0xD4`.
      Bump `kNotifyProtocolVer = 0x02`.
- [x] Extend `isSelfEcho`
      ([`../src/BadgeIR.cpp`](../src/BadgeIR.cpp) L58) to
      include MANIFEST/DATA/NEED (SENDER UID in words[1..2],
      matches the boop v2 self-echo pattern).
- [x] Sender chunker: new
      [`BadgeIRNotifyStream::irBeginNotifyStream`](../src/BadgeIRNotifyStream.h)
      that emits MANIFEST + N DATA frames.  Short messages still
      go through existing `irBeginNotify`; outbox's
      `kickIrAttempt` branches by body length vs
      `irSingleFrameBodyBudget`.
- [x] Receiver reassembler slot + NEED loop
      (`BadgeIRNotifyStream::onManifestFrame / onDataFrame /
      receiverTick`).
- [x] ACK carries msgToken (full 32-bit in `word3`) for
      multi-frame; single-frame unchanged.
      `BadgeIR::processNotifyAck` checks streaming slot first,
      falls through to nonce.
- [x] Extract TLV walker → `BadgeNotifications::parseNotifyTlvs`
      + `commitIncomingFromDecoded` so single- and multi-frame
      paths share decode logic.
- [x] Delete body-truncation stopgap in
      [`../src/BadgeIR.cpp`](../src/BadgeIR.cpp) `irBeginNotify`
      — oversized bodies now refuse (rc=-1) so the outbox
      branch is the single source of truth for the stream route.
- [x] Update [`../codeDocs/NotificationSystem.md`](../codeDocs/NotificationSystem.md)
      §IR wire format and §Sending.
- [x] Integration test: 173-byte body over 7 × 32 B chunks
      (bumped from initial 80 B design — the larger chunks hit
      the documented multi-frame back-to-back TX reliability bug
      in codeDocs/BoopSystem.md §Open Work, truncating long
      DATA frames at the RMT DMA refill boundary).  Full wire
      payload (200 B including TLV overhead) reassembles cleanly
      on the peer, commits to the store, and ACKs via the 32-bit
      msgToken echo.  Haptic discipline preserved: one
      `BadgeNotifs new incoming` + one `BadgeOutbox IR confirmed`
      per entry.  See B.1 / B.2 serial logs in 02b-part-b-kickoff.

---

### Part C — Multicast up to 4 recipients

Cheapest change once A is done — mostly just "iterate the
targets array in the outbox tick".

#### C.1 — Composer surface

Add a "pick peers" step to
[`../src/GUI.cpp`](../src/GUI.cpp) `SendBoopBackground::setTarget`:

- Today it takes a single `peerUidHex` + ticket + name.
- New `setTargets(const char* const* uids, const char* const*
  tickets, const char* const* names, uint8_t count)` overload,
  count 1..4.
- UI to actually pick multiple peers = later task.  For
  bring-up, wire the existing `SendBoopScreen` contact picker
  (search `kScreenSendBoop` in
  [`../src/GUI.cpp`](../src/GUI.cpp)) to a multi-select mode
  behind a config flag.  For v1 just expose a programmatic
  entry point plus a "reply-all" affordance in
  `ThreadDetailScreen` when the thread has multiple
  participants.

#### C.2 — IR fan-out

The IR sender is single-slot (`s_pendingNotify.active`).
Multicast TX = N sequential single-target sends.  The outbox
tick naturally drives this: when target[0]'s IR transitions to
confirmed (ACK) or pending-again (timeout), target[1]'s IR
takes the slot next.

Expect 4 targets × (single-frame: ~500 ms each, multi-frame:
~3-5 s each) = 2-20 s of wire time to saturate the list.
Not great but correct.  A future optimization is a new
`IR_NOTIFY_MULTI` frame type with multiple target UIDs in the
envelope, ACKed by any recipient — out of scope here; the
outbox retry machinery makes saturating slower-but-correct
perfectly acceptable for now.

#### C.3 — API fan-out

`BadgeAPI::sendPing` is single-target
([`../src/BadgeAPI.cpp`](../src/BadgeAPI.cpp) L397).  Multicast
API = N sequential POSTs, one per target ticket.  Per-target
state in the outbox already models this cleanly.  Consider
spawning ONE Core 1 task per send that loops over targets
sequentially (not N parallel tasks — the HTTP/TLS stack is
fat).

#### C.4 — Thread identity

Single call: `BadgeNotifications::threadIdForPeers(uids, count,
...)` already produces the right id.  `addOutgoing` today only
takes a single peer — extend it (or add `addOutgoingToPeers`)
to accept the same array shape.  Implementation is mostly
lifting the single-peer argument into the existing `uids[][12]`
+ `namesArr[4]` path inside `addMessageInternal` L395.

#### C.5 — Checklist

- [ ] Extend `BadgeNotifications::addOutgoing` to accept
      multiple target UIDs, or add `addOutgoingToPeers`.
- [ ] `SendBoopBackground::setTargets` array overload.
- [ ] `BadgeOutbox::enqueue` already arrays — wire the composer.
- [ ] Multi-select UI in the contact picker (or defer to a
      later plan; the data layer accepts programmatic
      multi-target today).

---

### Part D — Messages menu entry

Plumb the already-placeholder menu item in
[`../src/GUI.cpp`](../src/GUI.cpp) L211:

```cpp
static void onMessagesSelected(GUIManager& gui) {
  messagesRequested = true;
  Serial.println("GUI: Messages requested (not yet ported to GUI)");
}
```

Today HomeScreen *is* the thread list
([`../src/GUI.cpp`](../src/GUI.cpp) L1648 — it calls
`BadgeNotifications::threadCount()` + `fetchThread()` and
renders per-row with the `'*' name (N)` format). So for v1 the
Messages menu entry is just:

```cpp
static void onMessagesSelected(GUIManager& gui) {
  Haptics::shortPulse();
  gui.pushScreen(kScreenHome);
}
```

That's a 5-line change + deletion of the stale `messagesRequested`
global. When/if HomeScreen grows non-chat content (dashboard
tiles, weather, schedule, etc.), the Messages item should
become a dedicated `kScreenMessages` that renders the
thread-list subset.  To keep the future split cheap:

- Extract the thread-list render+input code out of
  `HomeScreen` into a `ThreadListSurface` helper (no new
  screen yet, just a function that takes the `oled&` and
  cursor/scroll state).
- `HomeScreen::render` continues to call
  `ThreadListSurface::render` today.  A future
  `MessagesScreen` can call the same helper, which
  future-proofs the split without costing anything now.

Checklist:

- [x] Replace the broken Messages menu entry with a direct
      `kScreenHome` route (the HomeScreen is the thread list).
      The placeholder `onMessagesSelected` callback is gone; the
      menu entry now reads "Messages" and navigates via
      `NavMenuItem{kScreenHome}`.  See
      [`../src/GUI.cpp`](../src/GUI.cpp) `kBadgeMenuItems`.
- [x] Deleted `messagesRequested` global
      ([`../src/BadgeGlobals.cpp`](../src/BadgeGlobals.cpp)) and
      its sole `extern` declaration in GUI.cpp.
- [ ] (Deferred — optional.)  Thread-list rendering could be
      extracted into a `ThreadListSurface` helper so a dedicated
      `MessagesScreen` can share the code later, but the current
      menu-wiring approach leaves the Home screen as the single
      renderer.  Revisit only if `HomeScreen` grows non-thread
      content.

GUI unification done in the same pass (not in the original D
checklist — surfaced by the Part D wire-up):

- [x] Removed the redundant "Home" menu entry; "Messages" now
      routes to `kScreenHome` so the thread list has exactly one
      menu entry point.
- [x] Renamed "Boop" menu entry → "Pair" (IR handshake that adds
      a contact) to distinguish from the "New Msg" composer.
- [x] Renamed "Send Boop" menu entry → "New Msg" and renamed the
      matching in-screen label on `ContactDetailScreen`
      (`>> New Msg <<`).
- [x] Consolidated the three "start a message to this peer"
      call sites (Contacts pick mode, ContactDetail SendBoop
      row, ThreadDetailScreen::replyHere) onto a new
      `SendBoopBackground::launchFor(gui, uid, ticket, name,
      skipConfirm)` helper that wraps `setTarget +
      Haptics::shortPulse + launchBodyEditor` once.  Removes
      3 × 3 lines of duplication and gives follow-up Part C
      multicast work a single place to add a `launchForPeers`
      variant.
- [x] Updated
      [`../codeDocs/NotificationSystem.md`](../codeDocs/NotificationSystem.md)
      §Sending with the new vocabulary and the `launchFor`
      funnel.

Testing:

- From main menu → Messages: lands on HomeScreen's thread
  list.  Down/Up/Right/Left behave identically to Home.
- Back from thread list pops back to menu (not to splash or
  anywhere else).

---

### Part E — Boop pairing reliability crossover

The IR side of boop pairing is already pretty reliable
(MANIFEST + NEED repair is in place in
[`../src/BadgeBoops.cpp`](../src/BadgeBoops.cpp) L2140).  The
reliability GAP is on the API reconciliation:
`recordBoopOnce()` at
[`../src/BadgeBoops.cpp`](../src/BadgeBoops.cpp) L1186–L1223
does a single synchronous `BadgeAPI::createBoop` call; if
WiFi is down or the server is reachable but misbehaving, it
sets `boopStatus.offlineBoop = true` and the boop exists
only in the local `/boops.json` journal forever.

Part E lifts the outbox retry pattern from Part A onto that
API call so an offline boop reconciles with the server the
next time WiFi is up.

#### E.1 — Pending-boop-API queue

New lightweight journal `/pending_boop_api.json`, separate
from `/outbox.json` (different semantics: no IR, no target
UID filtering, no multicast).

Schema:

```json
{ "pending": [
  { "peerUid":        "aabbccddeeff",
    "boopType":       0,
    "myFields":       { ... BadgeInfo::Fields snapshot ... },
    "peerFields":     { ... BoopStatus peer snapshot ... },
    "firstAttemptMs": 1234567890,
    "attempts":       0,
    "lastAttemptMs":  0 } ] }
```

Capture the peer's `BoopStatus` snapshot (name, title, company,
ticket UUID, bio, etc.) at enqueue time so the eventual API
reconciliation can POST the full context — not just the UID
pair.  Today's `createBoop` only uploads the UID pair; this
lets a future `createBoopRich` variant send everything once
it exists on the server, or we just retry the lean call
unchanged.

#### E.2 — Retry driver

Reuse the same Core 1 retry task pattern as `BadgeOutbox::tick`:

- Every 30 s, iterate pending entries.
- If WiFi is up and `nowMs - lastAttemptMs > 30 s`, fire a
  one-shot `xTaskCreatePinnedToCore` that calls
  `BadgeAPI::createBoop` with the stashed UID pair.
- On 200 → remove the entry + flip `boopStatus.offlineBoop`
  to false on the in-memory record (if still the current
  peer) + update `/boops.json` to record the `pairing_id`.
- On any other code → bump `attempts`, keep entry.
- No retention ceiling — these retry forever (or until the
  user manually clears the journal).  Boop records are
  precious; unlike notifications they aren't ephemeral.

#### E.3 — Where it hooks in

`recordBoopOnce()` at L1186 currently does:

```cpp
BoopResult result = BadgeAPI::createBoop(uid_hex, peerUID);
```

Wrap this:

```cpp
BoopResult result = BadgeAPI::createBoop(uid_hex, peerUID);
if (!result.ok && result.httpCode <= 0) {
  // Transport failure — enqueue for retry
  BadgePendingBoopApi::enqueue(peerUID, boopStatus.boopType,
                               /*peerSnapshot=*/boopStatus,
                               /*myFields=*/localCache);
  boopStatus.offlineBoop = true;
} else if (result.ok) {
  // Unchanged happy path
} else {
  // 4xx/5xx from server — DO NOT enqueue (would retry a
  // permanent rejection forever).  Log + drop.
  Serial.printf("[%s] boop rejected http=%d (not queued)\n",
                TAG, result.httpCode);
}
```

HTTP-code gating is important: only retry transport-layer
failures (`httpCode <= 0` means connect/DNS/timeout), NOT 400
or 401 where the server has decided our request is malformed.

#### E.4 — Field-exchange failure recovery (IR-side)

Separate concern, but worth noting: if the IR field exchange
in `peer_tickPostConfirm` times out or corrupts mid-stream
(the documented multi-frame back-to-back TX race in
[`../codeDocs/BoopSystem.md`](../codeDocs/BoopSystem.md) Open
Work section), the boop still completes with
`fieldRxMask == 0` and the partner info stays blank.

Plan A's `BadgeOutbox` infrastructure doesn't apply here
(it's synchronous IR, not a queued retry), but we CAN reduce
the race window by:

1. Dropping `kMaxTlvBytes` from 120 B to 100 B in
   [`../src/BadgeBoops.cpp`](../src/BadgeBoops.cpp)
   `serveStreamReq` L2141.  This brings DATA wire time from
   ~1.85 s to ~1.55 s, which gives NEED repair loops more
   slack before `kRetxMsSlow = 2800` fires.
2. When `fieldRxMask == 0` at `finishPaired()` time AND
   `boopStatus.peerTicketUuid[0]` is populated (we at least
   got the ticket before IR died), kick an
   `attendeePrefetch` task RIGHT THEN so the partner info
   gets filled from the server instead of staying blank.
   `attendeePrefetch` already exists at L845 and is exactly
   right for this — just move the trigger earlier.

Both are small optimizations.  Treat them as E.4a (frame
size) and E.4b (fallback prefetch); either or neither can
ship with the E.1-E.3 API queue.

#### E.5 — Checklist

- [x] Create `src/BadgePendingBoopApi.{h,cpp}` — mirrors the
      `BadgeOutbox` journal pattern (FreeRTOS mutex, atomic
      save, reboot recovery), simplified for single-rail
      API-only retry. Journal at `/pending_boop_api.json`.
      Schema trimmed to `{peerUid, boopType, firstAttemptMs,
      attempts, lastAttemptMs}` — the myFields/peerFields
      snapshots the design doc sketched aren't needed by
      today's `createBoop(myUid, peerUid)` call and can be
      added without breaking the interface when a richer
      endpoint lands.
- [x] Wired into `recordBoopOnce` at both offline branches
      (WiFi-down at entry + `httpCode <= 0` after
      `createBoop`).  4xx/5xx branch intentionally does NOT
      enqueue (permanent reject).
- [x] Added a dedicated Core 1 retry task (`PBApiRty`) at
      5 s tick, 30 s per-entry cooldown. Plan hinted at
      reusing `BadgeOutbox::retryTask` for a single Core 1
      retry loop; kept separate for cleaner module ownership
      — one extra FreeRTOS task is cheap and the merge is
      trivial later if ever needed.
- [x] On success: `BadgeBoops::applyBoopApiSuccess(peerUid,
      &result)` reconciles the `/boops.json` row (sets
      `pairing_id` + status + partner fields) WITHOUT
      bumping `boop_count` (the offline `recordBoop` call
      already counted once). If the reconciled peer is
      still on-screen, `boopStatus.offlineBoop` flips to
      false live.
- [ ] (optional) E.4a: drop boop `kMaxTlvBytes` to 100 —
      deferred (single-line change, unrelated to this
      queue).
- [ ] (optional) E.4b: fire `kickAttendeePrefetch` at
      `finishPaired()` when `fieldRxMask == 0` — deferred
      (different failure mode; belongs in a boop-protocol
      reliability pass).

---

### Part F — Anycast frames (untrusted installation broadcasts)

Problem statement: conference exhibits, kiosks, queue
terminals, and schedule-cue beacons want to tell nearby
badges "I'm here, this is what you just saw / did" without a
full UID handshake.  The current boop types
(`BOOP_EXHIBIT = 0x01`, `BOOP_QUEUE_JOIN = 0x02`,
`BOOP_KIOSK_INFO = 0x03`, `BOOP_CHECKIN = 0x04` —
[`../src/BadgeIR.h`](../src/BadgeIR.h) L40–L50) all still go
through the beacon → handshake → EXCHANGE flow because they
ride the `IR_BOOP_BEACON` wire type that demands a UID in
`words[1..2]`.

Anycast is a PARALLEL, unidirectional, UID-less wire type.
Fire-and-forget.  Explicitly untrusted because there's no
cryptographic binding between the installation's declared id
and what it is.

#### F.1 — Wire format

New frame type `IR_ANYCAST = 0xE0`.  No ACK, no reply.

```
word0       : 0xE0 | actionCode<<8 | ver<<16 | flags<<24
word1       : installationId low 32 bits
word2       : installationId high 16 bits + 16 bits reserved
word3       : seq (per-installation anti-replay / dedup counter)
words[4..]  : TLV payload, 4 bytes/word packing, same TLV
              shape as IR_NOTIFY:
                 0x01 = installation display name (<=24 B)
                 0x02 = short message / caption  (<=80 B)
                 0x10 = visit-credit context blob (action-specific)
                 0x11 = schedule cue (ISO-8601 time string)
```

`installationId` is 48-bit (same size as a badge UID) so it
can share printer format helpers, but it's a flat namespace
owned by whoever programmed the installation — NOT a real
UID, NOT cryptographically bound.  **Never treat it like a
peer UID.**

Protocol version starts at `kAnycastProtocolVer = 0x01`.
No back-compat — anycast doesn't exist yet.

#### F.2 — Action code allowlist

Receiver ONLY executes these codes.  Anything else is
silently dropped (log the code for future debugging).  Each
allowed code has a bounded, user-visible side effect:

```
ANYCAST_ACTION_UNSET         0x00  // drop silently
ANYCAST_ACTION_EXHIBIT_VISIT 0x01  // "you visited Exhibit X"
ANYCAST_ACTION_QUEUE_UPDATE  0x02  // "Queue X is at position N"
ANYCAST_ACTION_BROADCAST_MSG 0x03  // toast a short message
ANYCAST_ACTION_SCHEDULE_CUE  0x04  // "talk starts in N min"
// 0x05..0x0F reserved for future badge-implemented actions
// 0x10..0xFF intentionally unused — never route these
```

Permanently denied (document for future maintainers):

- Anything that edits badge settings / config
- Anything that modifies `/boops.json` or
  `/notifications.json` in ways indistinguishable from real
  peer interactions
- Anything that invokes network calls (no
  "click this anycast-supplied URL")
- Anything that renders without a visible "untrusted source"
  chrome indicator

#### F.3 — Self-echo filter carve-out

[`../src/BadgeIR.cpp`](../src/BadgeIR.cpp) `isSelfEcho`
L58–L103 already has two branches: sender-UID-carrying types
(beacon/DATA) and target-UID-carrying types (NOTIFY/ACK).
Anycast fits neither — `words[1..2]` is the installation id,
not a badge UID.  Add a third branch:

```cpp
if (type == IR_ANYCAST) {
    return false;  // never self-echo; badges don't TX anycast
}
```

Badges don't TX anycast (installations do), so the filter
above is actually just "let it through, never treat it as
our own echo".  If we later add a firmware build for
installation-hardware-running-badge-silicon, we'll have to
revisit, but for now badges are strictly anycast receivers.

#### F.4 — Per-installation rate limiting + dedup

Defense against a hostile or broken installation spamming
the wire:

- In-memory ring buffer of the last 16 `(installationId, seq)`
  pairs seen.  Duplicate (id, seq) pair → drop silently.
- Per-installationId admit counter: no more than one frame
  every 5 s accepted.  Frames arriving faster from the same
  id are dropped and counted (Serial-logged once per minute
  so spam is visible to the developer but doesn't flood).
- Per-action-per-day ceiling: same `(installationId,
  actionCode)` can deliver AT MOST one user-visible side
  effect per calendar day (tracked via a lightweight
  `/anycast_seen.json` that stores a `YYYY-MM-DD:<id>:<code>`
  hash set, capped at ~200 entries).  Prevents an attacker
  from spamming fake visit credits.

#### F.5 — Delivery surface (what the user actually sees)

Separate storage, separate UI chrome — **never** mix anycast
into `/boops.json` or `/notifications.json`.  They'd look
identical to real peer-to-peer interactions otherwise.

New lightweight journal `/anycast_log.json`:

```json
{ "visits": [
  { "installationId": "000abc123456",
    "installationName": "Hydraulics Demo",
    "actionCode":      1,
    "caption":         "Thanks for stopping by!",
    "receivedAtMs":    1234567890 } ] }
```

Surface in the UI via:

- A "Places" or "Visits" row on the main menu (NEW
  `kScreenAnycast`) — simple list view, newest first, each
  row shows installation name + timestamp.
- Entries are tagged `source: "installation"` in the render
  path; the row chrome (color, icon) is deliberately
  different from a thread row so a user can't mistake an
  anycast visit for a real boop with a person.
- `ANYCAST_ACTION_BROADCAST_MSG` additionally flashes a
  brief toast on-screen (reuse
  `SendBoopConfirmScreen`-style layout with an "!" icon)
  but the persistence still goes into the anycast log, NOT
  the notifications store.

#### F.6 — TX support (optional, for testing)

Badges are consumers, not producers, of anycast in
production.  BUT shipping a `BadgeIR::sendAnycast(actionCode,
installationId, ...)` helper behind a compile-time or
`badgeConfig` flag is useful for:

- Developer hardware-in-the-loop tests: one badge pretends
  to be an exhibit, broadcasts, verifies the other badge
  logs it correctly.
- The Jumperless skill's hardware-loop testing — can drive
  a Jumperless GPIO to trigger anycast TX and measure
  reception latency on the receiver.

Gate behind `#ifdef DEBUG_ANYCAST_TX` or a
`badgeConfig.allowAnycastTx` flag, default OFF in production
builds.

#### F.7 — Checklist

- [ ] Add `IR_ANYCAST = 0xE0` + `kAnycastProtocolVer = 0x01`
      + `AnycastActionCode` enum to
      [`../src/BadgeIR.h`](../src/BadgeIR.h).
- [ ] Teach `isSelfEcho` to let anycast frames through
      untouched ([`../src/BadgeIR.cpp`](../src/BadgeIR.cpp)
      L58).
- [ ] Create `src/BadgeAnycast.{h,cpp}`:
      - RX dispatch called from `irTask` alongside
        `irNotifyFrameDispatch`
      - Per-installation rate limiter (ring buffer)
      - Per-day dedup journal (`/anycast_seen.json`)
      - Visit journal (`/anycast_log.json`)
      - Action-code handlers (switch statement; unknown
        codes → drop + log)
- [ ] Wire `irTask` notifyTick to also dispatch
      `IR_ANYCAST` frames ([`../src/BadgeIR.cpp`](../src/BadgeIR.cpp)
      `notifyTick` L574).
- [ ] New `AnycastLogScreen` (mirrors `HomeScreen`'s
      thread-list pattern but reads from
      `BadgeAnycast::visitCount/fetchVisit`).
- [ ] Menu entry "Places" → `kScreenAnycast`, positioned
      after "Messages".
- [ ] Optional TX helper `BadgeIR::sendAnycast(...)` behind
      a debug flag.
- [ ] Update
      [`../codeDocs/BoopSystem.md`](../codeDocs/BoopSystem.md)
      with a new §Anycast section covering the trust model
      and allowlist.

---

## Testing approach

Order: A bring-up → A soak → D → B → C → E → F.  Each phase
has an isolation mode that doesn't need the others.

**Part A — outbox + parallel send**

- **A.1 Parallel send, both rails succeed.**  Short body.
  Expect: one outbox entry, both `ir.state` and `api.state`
  flip to `confirmed` within ~1 s, entry retires, haptic
  fires exactly once.  Peer shows one message (dedup OK).
- **A.2 Parallel send, IR blocked.**  Cover the IR LED.
  Expect: API rail confirms quickly, entry retires on API
  alone.  IR rail marked failed at 2-min ceiling.  Haptic
  fires once when API confirms.
- **A.3 Parallel send, API blocked.**  WiFi off.  Expect: IR
  rail confirms, entry retires.  No API retries fire.
- **A.4 Send with both rails blocked.**  WiFi off + IR LED
  covered.  Expect: entry sits in outbox, periodic IR retries
  log on serial, nothing gets through, retire at 2-min
  ceiling.  No haptic ever fires.
- **A.5 Send, reboot mid-retry.**  WiFi off, cover IR LED,
  submit message, power-cycle the badge within 10 s.  Expect:
  on boot, `BadgeOutbox::begin` reloads the entry, retry
  resumes from `pending`.  Uncover IR LED and watch it deliver
  after the next retry tick.
- **A.6 UID-filter ACK regression.**  Badge A sends IR_NOTIFY
  targeting wrong UID (patch the source UID in a test harness
  or use a third badge).  Expect: receiver never ACKs, no ACK
  frame on the wire, no message appears in receiver's thread.
  Serial log shows self-echo filter drop.

**Part D — Messages menu entry**

- **D.1 Nav.**  Main menu → "Messages": lands on the thread
  list.  Left/back: pops to the main menu (not splash).
- **D.2 Empty state.**  Fresh badge with zero threads: menu →
  Messages renders the "No boops yet" placeholder at Home's
  same message.  Back pops cleanly.
- **D.3 Parity with Home.**  Open a thread from both entry
  points (Home vs Messages).  Scroll, reply, pop back.
  Behavior should be identical — Messages isn't a second copy
  of the list; it's the same screen reached by a different
  menu path.

**Part B — IR multi-frame**

- **B.1 Short message still fast-path.**  Same wire as A.1;
  verify MANIFEST is NOT emitted — only `IR_NOTIFY` (0xD0)
  + `IR_NOTIFY_ACK` (0xD1).
- **B.2 Long message (3 chunks).**  150-char body.  Expect:
  MANIFEST + 3 DATA frames, receiver reassembles, single ACK
  carrying full 32-bit msgToken retires the outbox entry.
- **B.3 Dropped chunk.**  Briefly obstruct IR mid-stream.
  Expect: receiver TXes NEED with bitmap indicating missing
  chunkIdx, sender re-TXes only those chunks, eventual ACK.
- **B.4 Boundary case.**  Send a body exactly at the
  single-frame threshold.  Verify the sender's branch-
  decision lands where we expect and the frame still
  decodes.

**Part C — Multicast**

- **C.1 Multicast 2 peers, both online.**  Compose to A+B,
  verify both get one message each, sender's outbox closes
  both targets, both end up in the same `thr:g<hash>` thread
  on the sender's notifications store.
- **C.2 Multicast 4 peers, one IR-blocked.**  Verify 3
  confirm via IR and 1 via API (assuming WiFi is up).
  Haptic fires exactly once (on first-confirm), not four
  times.

**Part E — Boop-API reliability**

- **E.1 Offline boop → online reconcile.**  WiFi off.
  Complete a boop handshake with a peer.  Verify
  `boopStatus.offlineBoop == true` + entry in
  `/pending_boop_api.json`.  Turn WiFi on.  Within one retry
  tick (~30 s) verify the API POST succeeds, the entry is
  removed, and `/boops.json` is updated with the
  `pairing_id`.
- **E.2 Permanent rejection.**  Force the server to return
  400 (e.g. by editing a local test server to return an
  error).  Verify the failed call is NOT enqueued — a 4xx is
  a permanent reject, retrying forever would be worse than
  dropping.
- **E.3 Reboot with pending entries.**  Complete an offline
  boop, reboot the badge before WiFi comes up, bring WiFi up
  after boot.  Verify the pending entry survives and
  reconciles.

**Part F — Anycast**

- **F.1 Allowlisted action.**  TX an
  `ANYCAST_ACTION_EXHIBIT_VISIT` from a test badge with the
  DEBUG_ANYCAST_TX flag.  Receiver logs it in
  `/anycast_log.json` with `source: "installation"` and the
  entry is visible in the Places menu.
- **F.2 Unknown action.**  TX an anycast with actionCode =
  0xAA (not in the allowlist).  Receiver drops silently;
  serial logs "anycast unknown action 0xAA" once; no journal
  entry is created.
- **F.3 Rate limit.**  TX 5 anycast frames from the same
  installationId within 1 s.  Only the first is accepted;
  the rest are dropped with serial logs.
- **F.4 Per-day dedup.**  TX the same
  `(installationId, actionCode)` pair twice on the same
  day.  Only the first creates a user-visible side effect;
  the second is silently dropped after dedup journal
  consult.  Same pair on the next day is accepted again.
- **F.5 Anycast doesn't leak into notifications.**  After
  any anycast TX, verify `/notifications.json` has zero new
  entries.  Verify `/boops.json` has zero new entries.
  Anycast state lives strictly in `/anycast_log.json` +
  `/anycast_seen.json`.
- **F.6 UID impersonation attempt.**  TX an anycast frame
  with `installationId` set to a real badge UID.  Receiver
  should still treat it as an installation (no trust, no
  merge into that peer's boop/notification thread).  Easy
  visual check: the Places entry shows up tagged as an
  installation, NOT as a new boop from that peer.

## Conventions to preserve

- **`msg:<token>` is THE dedup key.**  Don't invent a new one;
  extend the outbox around it.  The sender's own
  `addOutgoing` record uses the same id, so `addIncoming` on
  the peer side AND `addOutgoing` on our side always agree
  per-message.
- **`kNotifyProtocolVer = 0x02` from day one.**  Nothing has
  shipped on this wire; there is no legacy receiver to
  support.  Don't write fallback code for 0x01.
- **FreeRTOS mutex, NOT portMUX.**  Journal writes hit FAT
  (SPI flash, tens to hundreds of ms) which under a spinlock
  would trip the interrupt watchdog.  See
  [`../src/BadgeBoops.cpp`](../src/BadgeBoops.cpp) L63–L68 for
  the canonical explanation.
- **Core 1 for HTTP/TLS, Core 0 for IR.**  Current allocation.
  API background tasks pin to Core 1; IR retry tick triggered
  from Core 1 but the actual TX call (`irBeginNotify`) already
  serializes onto Core 0 via the pending-notify slot, which is
  fine.
- **Haptic discipline: exactly ONE pulse per send, total.**
  Fires on the first confirmation of the first target to
  come back on either rail.  Multicast doesn't stack pulses;
  IR+API parallel doesn't double-pulse.
- **Anycast is a strictly separate trust domain.**  Anycast
  frames NEVER touch `/notifications.json` or `/boops.json`.
  Anycast data NEVER masquerades as peer-to-peer data in the
  UI.  Any code path that accepts an anycast payload MUST
  only perform actions from the allowlist in F.2, and MUST
  render with distinct chrome so users can't mistake it for
  a real boop or message.
