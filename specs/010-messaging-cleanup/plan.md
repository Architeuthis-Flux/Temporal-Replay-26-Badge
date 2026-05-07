# spec-010: Messaging Cleanup — Implementation Plan

Full rewrite of the Zigmoji and Messages apps; minor surgical changes elsewhere
to support API-only messaging, a hard identity gate, and clearer naming.

## Context

Today's messaging stack conflates three separate primitives:

- **Boop** (IR pairing) — `POST /api/v1/boops`, mutual-consent handshake.
- **Message** (text) — uses `BadgeAPI::sendPing` with `activity_type="notification"` plus an IR fallback rail.
- **Zigmoji** — uses `BadgeAPI::sendPing` with `activity_type="zigmoji"`, API-only.

The naming, the dual-rail (IR + API) state machine, and the mixed-mode
`ContactsScreen` (browse + zigmoji-pick + message-pick all in one screen with
boolean flags) make the code harder to reason about than the feature set
warrants. This rewrite simplifies on all three axes: API-only for everything
post-pairing, "boop" reserved exclusively for the pairing handshake, two
self-contained apps each owning their own state.

## Design decisions (resolved during grilling)

1. **Apps are native C++ screens** — Python `/apps/` system stays for community contributions; messaging features stay native.
2. **Main menu shrinks to 4 items** — `Boop / Zigmoji / Messages / Settings`. No `Contacts`, no `New Msg`, no `QR` row.
3. **Boot flow gates on identity** — GUI boot branches directly: paired → `MainMenu`; unpaired → `QR`. Unpaired user cannot reach the menu.
5. **API-only post-pairing** — IR rail dropped for messages and zigmojis. `/api/v1/pings` is the sole transport.
6. **`BadgeOutbox` deleted** — replaced by inline FreeRTOS task per send (Option α).
7. **Activity-type rename** — `"notification"` → `"message"` on send + receive.
8. **Backend `/api/v1/boops` paginated** — keyset cursor, server-side recency sort, client-supplied `limit`.
9. **Keyboard layout: K1 (QWERTY) default + K2 (ABC) toggle** — in Settings via existing `kKbLayout` setting.
10. **Joystick keyboard** — extends the existing joystick mouse overlay (`mp_api_mouse.cpp`); promoted to shared `ui/MouseOverlay`. IMU tilt/gyro input was cut after hardware testing.
11. **Zigmoji inbox is a queue, not a history** — Z3+L3 hybrid: smart landing (queue empty → recipient picker; queue non-empty → playback animation). Each played zigmoji is drained from the queue. Bounded FIFO ~10. Persisted to `/zigmojis.json`.
12. **`PeerPickerScreen` is shared** — both apps push it to pick a recipient. Reads from `/boops.json`, recency-sorted, active-only, name + company row.
13. **Notification poll: 60s base + 5s foreground accelerator** — default-on. `BADGE_ENABLE_PING_POLL` macro removed.
14. **Send failure UX** — V1 corner glyph (sending=dot, sent=✓, failed=!) on each bubble. R1 manual retry via long-press LEFT. Pre-send WiFi check marks failed-pre-send instantly.
15. **Haptic suppression** — focused thread receives a message → no buzz; different thread / list view / out-of-app → buzz.

## PR strategy

Three PRs, sized for review:

- **PR 1 — Foundation infra.** Pure refactors, no user-visible behavior change.
- **PR 2 — Rewrite.** New apps, new send path, deletions of obsoleted code.
- **PR 3 — Boot flow + cleanup + docs.** Identity gating, menu reshuffle, file deletions, documentation.

Backend agent (Phase 0) runs in parallel with PR 1 and lands before PR 2 ships.

---

## Phase 0 — Backend (✅ SHIPPED)

The backend has landed both changes. Live at the production deploy; spec
snapshot at `docs/openapi.json` on `temporal-community/replayBadgeBackend`
main branch (and on the live `brooklyn.party/openapi.json` once auth is
sorted out).

### Confirmed wire shape from the backend agent

**`GET /api/v1/boops` — paginated:**

```
Query params:
  badge_uuid OR ticket_uuid     (exactly one, unchanged)
  active=true|false|all         (default "true")
  limit=N                       (default 25, server caps at 500;
                                 firmware sends limit=30 = kMaxContacts)
  before_ts=<ISO8601>           (cursor — required together with before_id)
  before_id=<int>               (cursor — pairings.id, INTEGER)

Sort (server-side, fixed):
  MAX(connected_at) DESC, id DESC
  → strict total order; concurrent inserts during pagination cause
    no duplicates and no skips.

Response 200:
{
  "pairings": [
    {
      "id": 42,                              // int
      "ticket_uuids":  ["<uuid-a>", "<uuid-b>"],
      "badge_uuids":   ["<hex-or-empty>", "<hex-or-empty>"],
      "partner_name":   "Alex Kim",          // ALWAYS PRESENT, "" if unknown
      "partner_company":"Temporal",          // ALWAYS PRESENT, "" if unknown
      "connected_at":  ["2026-04-27T10:00:00Z", ...],
      "boop_count":    3,
      "revoked_at":    null,
      "created_at":    "...",                // NEW
      "updated_at":    "..."                 // NEW
    }
  ],
  "next_cursor": {                           // null when no more pages
    "before_ts": "2026-04-27T10:00:00Z",
    "before_id": 42                          // INTEGER for boops
  }
}
```

**`GET /api/v1/pings` — cursor shape changed (BREAKING):**

```
Was:  next_cursor: "<base64-encoded JSON blob>"
Now:  next_cursor: {
        before_ts: "<ISO8601>",
        before_id: "<UUID-string>"            // STRING for pings (uuid)
      } | null
```

Same shape as the boops cursor, except `before_id` is a UUID string (not an
int) since `pings.id` is a UUID. Pass `before_ts` + `before_id` back as
query params unchanged. **No more base64 decode step.**

**`POST /api/v1/pings` — new caps (informational, no firmware change):**

- `data` JSONB capped at 4 KB serialized; oversize → 422 with
  `data_too_large`. Firmware never generates payloads near this size.
- Quart-level 64 KB `MAX_CONTENT_LENGTH` ceiling on any request body
  → 413 if exceeded. Same — firmware never approaches this.
- Spec-010 firmware treats `data` as an app-owned payload. Staging can be
  reset, so PR 2 uses clean unversioned shapes:
  message `{ "id": "<client-token>", "body": "<text>" }`;
  zigmoji `{ "key": "<palette-key>" }`.

**Activity-type rename `"notification"` → `"message"`:** backend accepts
BOTH strings during transition. Firmware switches in PR 2; backend stops
accepting `"notification"` only after PR 2 lands on all badges.

---

## Phase 1 — Foundation infra (PR 1)

Zero-behavior-change refactors. Lands first so PR 2 can build on it.

### 1a. Backend-contract consumer updates (NEW — added after Phase 0 shipped)

The local `specs/openapi.json` is currently stale (still shows
`partner_name: nullable: true` and `next_cursor` as a base64 string). Sync
it from the live spec, then regenerate the C++ types.

| Action | Path | Notes |
|---|---|---|
| Sync | `specs/openapi.json` | `curl -A "Mozilla/5.0" https://brooklyn.party/openapi.json -o specs/openapi.json` (Anthropic's WebFetch hits Cloudflare's bot guard with HTTP 403; a browser UA gets HTTP 200). Or fetch `docs/openapi.json` from `temporal-community/replayBadgeBackend` main. Replace the local file in full |
| Regenerate | `firmware/src/api/BadgeAPI_types.h` | Run `python3 scripts/gen_badge_api_types.py`. Picks up: `BoopRecord` adds `created_at` / `updated_at`; `partner_name` / `partner_company` lose `nullable` flag (now always-present strings, default `""`); `NextCursor` and `PingNextCursor` become structured `{before_ts, before_id}` types — `before_id` is `int` for boops, `string` (UUID) for pings |
| Edit | `firmware/src/api/BadgeAPI.h` + `BadgeAPI.cpp` | `getBoops` signature gains `int limit, const char* before_ts, const char* before_id`. Builds query string (URL-encode cursor values). Parses `next_cursor` as a JSON object: `doc["next_cursor"]["before_ts"]` (string) + `doc["next_cursor"]["before_id"]` (int) |
| Edit | `firmware/src/api/BadgeAPI_types.h` | `GetBoopsResult` gains `bool has_next_cursor`, `char next_before_ts[24]`, `int next_before_id`. Drop `BADGE_PING_CURSOR_MAX = 128` (single buffer for old base64 blob); replace with `bool has_next_cursor` + `char next_before_ts[24]` + `char next_before_id[37]` (UUID-sized) on `GetPingsResult` |
| Edit | `firmware/src/api/BadgeAPI.cpp` (`getPings` parser) | The `_copyStr(r.next_cursor, ..., doc["next_cursor"] | "")` at line 752 currently treats `next_cursor` as a flat string — switch to parse the object: `doc["next_cursor"]["before_ts"]` and `doc["next_cursor"]["before_id"]`. Today's caller (`WiFiService::pollNotifications`) doesn't use the cursor — it just fetches latest N — so this is parser-only correctness, not a behavior change |
| Edit | `firmware/src/boops/BadgeBoops.cpp` (`syncWithApi`) | One-shot fetch → paginated loop. Send `limit=kMaxContacts` (=30) on each page. Accumulate across pages. Stop when `next_cursor` is null. Hard cap at 3 pages (~90 records) as a safety stop. Write merged result to `/boops.json` once at the end |
| Edit | `firmware/src/boops/BadgeBoops.cpp` | Update comment on `BADGE_BOOPS_MAX_RECORDS` (or equivalent constant): now a per-page cap, not a total cap |
| Edit | `firmware/src/boops/BadgeBoops.cpp` (sync logic) | Use `partner_name` / `partner_company` directly from each row — they're guaranteed present strings (`""` if no registration data). No more synthesizing from UUIDs or per-row backfill. Drop the existing name-backfill path if it exists |

### 1b. Original Phase 1 refactors

| Action | Path | Notes |
|---|---|---|
| Create | `firmware/src/infra/JsonEscape.{h,cpp}` | Move `jsonEscapeString` from `BadgeOutbox.cpp:343` |
| Create | `firmware/src/ui/MouseOverlay.{h,cpp}` | Promote state + composite hook from `mp_api_mouse.cpp`; native composer uses joystick-only cursor movement |
| Edit | `firmware/src/micropython/badge_mp_api/mp_api_mouse.cpp` | Replace internal state with thin wrappers calling `MouseOverlay::*` — Python API surface unchanged |
| Edit | `firmware/src/boops/BadgeBoops.{h,cpp}` | Add `lookupPeerByTicket(ticket, outUid, outName, ...)` and `listActivePeers(callback)` — single owner of `/boops.json` parse |
| Edit | `firmware/src/boops/BadgeBoops.h` | Move `kMaxContacts` here as canonical |
| Edit | `firmware/src/api/WiFiService.{h,cpp}` | Add `pushForeground()` / `popForeground()` ref-count pattern |
| Create | `firmware/src/messaging/MessageStatusGlyph.{h,cpp}` | `draw(d, x, y, status)` helper for V1 corner indicators |
| Edit | `firmware/src/infra/BadgeConfig.cpp` | `notify_poll` default `0` → `60000` |
| Edit | `firmware/src/infra/BadgeConfig.cpp` + `WiFiService.cpp` | Drop `BADGE_ENABLE_PING_POLL` macro; runtime config is the only gate |
| Edit | `firmware/src/api/WiFiService.cpp` | `resolveSenderFromBoops` → call `BadgeBoops::lookupPeerByTicket` (~30 lines deleted) |
| Edit | `firmware/src/screens/ContactsScreen.cpp` | `loadEntries` → call `BadgeBoops::listActivePeers` (transient — ContactsScreen deleted in Phase 7) |

### Verification gate

- `firmware/build.sh echo -n` clean. Same for charlie + delta envs.
- Manual smoke: boot a paired badge, confirm Contacts list still loads with names from new sync, confirm zigmoji send still works (existing flows untouched), confirm `getPings` poll doesn't crash on the new object-shaped `next_cursor` (it should silently parse-and-ignore, since the caller passes `nullptr` cursors).
- No new lint warnings.

---

## Phase 2 — Inbox layer (PR 2 starts)

| Action | Path | Notes |
|---|---|---|
| Rename | `BadgeNotifications.{h,cpp}` → `messaging/MessageInbox.{h,cpp}` | Drop `kMaxParticipants` and `participantUids/Names` arrays from `ThreadSummary` (saves ~560 bytes across 8-thread cap). Force 1-to-1 |
| Create | `firmware/src/messaging/ZigmojiInbox.{h,cpp}` | Bounded FIFO queue (max 10), persisted to `/zigmojis.json` via `BadgeJournal` atomic-write helper. API: `enqueue`, `peek(idx)`, `pop`, `count`, `clear` |
| Edit | `firmware/src/api/WiFiService.cpp` | `kPingHandlers[]` table: rename `"notification"` handler → `"message"`. Zigmoji handler now writes to `ZigmojiInbox::enqueue` |
| Edit | `firmware/src/api/WiFiService.cpp` | `pollNotifications` does focused-thread haptic suppression: read `MessageInbox::focusedThreadId()`; if incoming lands in focused thread, suppress haptic |
| Edit | `firmware/src/messaging/MessageInbox.cpp` | Add `static char s_focusedThreadId[kThreadIdMax]`; setter/getter; `addIncoming` checks before haptic |

### Verification gate

- Build clean.
- Send a message between two paired badges; receiver buzzes + bubble appears.
- Send a zigmoji; receiver buzzes; queue increments; thread store no longer receives zigmojis.

---

## Phase 3 — Send path rewrite (continues PR 2)

| Action | Path | Notes |
|---|---|---|
| Create | `firmware/src/messaging/SendMessage.{h,cpp}` | New module replacing `SendBoopBackground`. API: `sendNow(peerUidHex, peerTicket, peerName, body)` returns immediately, spawns one-shot FreeRTOS task at priority 1, pinned to Core 1, 8KB stack. Pre-send WiFi check: if `WiFi.status() != WL_CONNECTED`, mark `failed-pre-send` instantly, no task spawn. Task: build JSON via `JsonEscape`, call `BadgeAPI::sendPing` with `activity_type="message"`, on response update message status in inbox, fire haptic on first sent confirmation |
| Edit | `firmware/src/messaging/MessageInbox.{h,cpp}` | Add `MessageStatus { Sending, Sent, Failed, FailedNoWifi }` enum to `Message`. Add `markStatus(threadId, msgId, status)`. `addOutgoing` defaults to `Sending` |
| Delete | `firmware/src/messaging/BadgeOutbox.{h,cpp}` | ~600 lines |
| Delete | `firmware/src/screens/SendBoopBackground.{h,cpp}` | |
| Delete | `firmware/src/screens/SendBoopConfirmScreen.{h,cpp}` | |
| Edit | `firmware/src/ui/GUI.cpp` | Remove `sSendBoopConfirm` registration |
| Edit | `firmware/src/screens/Screen.h` | Remove `kScreenSendBoopConfirm` |

### Verification gate

- Build clean — fix any orphaned `SendBoopBackground::*` call sites surfaced by the compiler.
- Send a message: appears immediately as "sending" (dot glyph), transitions to "sent" (✓) within ~500ms.
- Pull WiFi mid-send: existing message marked "failed".
- WiFi down at send time: marked `failed-pre-send` instantly with no-wifi indicator.

---

## Phase 4 — Shared widgets (continues PR 2)

| Action | Path | Notes |
|---|---|---|
| Create | `firmware/src/screens/PeerPickerScreen.{h,cpp}` | Pushed with callback `void(const PeerPickResult&)`. Reads via `BadgeBoops::listActivePeers`. Recency-sorted (server-baked into `/boops.json`). Render: name primary, company secondary right-aligned. Joystick + UP/DOWN nav. RIGHT = pick + pop + fire callback. LEFT = cancel + pop. Empty state: "No contacts yet — Boop someone to add them" |
| Create | `firmware/src/screens/MessageComposerScreen.{h,cpp}` | Pushed with target peer + onDone callback. `onEnter`: enable `MouseOverlay`, `pushForeground`. `onExit`: disable, popForeground. Render: keyboard cells from `TextInputLayouts::*` (K1/K2 driven by `kKbLayout`), draft text strip at top, status row at bottom. Cursor driven by joystick. RIGHT = type letter under cursor; UP = submit; DOWN = space; LEFT = backspace. Hit-test cell at cursor X/Y |
| Edit | `firmware/src/screens/Screen.h` | Add `kScreenPeerPicker`, `kScreenMessageComposer` |
| Edit | `firmware/src/ui/GUI.cpp` | Register both new screens |
| Edit | `firmware/src/screens/TextInputLayouts.{h,cpp}` | Verify `getCellAt(layout, x, y, outChar)` and `getCellRect(layout, char, ...)` accessors exist; add if not |

### Verification gate

- Build clean.
- Push `PeerPickerScreen` from a temporary debug entry; nav, RIGHT fires callback, LEFT cancels.
- Push `MessageComposerScreen`; joystick moves cursor; RIGHT types letters; cells hit-tested correctly; UP submits.

---

## Phase 5 — Apps (completes PR 2)

| Action | Path | Notes |
|---|---|---|
| Rename | `screens/HomeScreen.{h,cpp}` → `screens/MessagesScreen.{h,cpp}` | Lands as Messages app. Add "+ New" row at index 0 of thread list. RIGHT on "+ New" → push `PeerPickerScreen` → push `MessageComposerScreen`. Drop the chrome XBM (Graphics_Base) — was a home-screen-only motif. `onEnter` → `pushForeground`; `onExit` → `popForeground` |
| Rewrite | `firmware/src/screens/ThreadDetailScreen.{h,cpp}` | Render bubbles with `MessageStatusGlyph` in top-right corner. Long-press LEFT on `Failed` bubble → `SendMessage::sendNow` again with same body, transition glyph to `Sending`. `onEnter` → `MessageInbox::setFocusedThreadId(threadId)`; `onExit` clears it |
| Rewrite | `firmware/src/screens/ZigmojiScreen.{h,cpp}` | Multi-state: `Playback / PickPeer / Palette / Sending / Done`. `onEnter`: queue non-empty → `Playback`; else → `PickPeer`. **Playback**: render queue head's animated XBM full-screen + sender name caption in FONT_TINY at bottom. Auto-advance every 2500ms. RIGHT advances early. LEFT skips remaining (drains queue). On each advance: `ZigmojiInbox::pop()`. Empty → `PickPeer`. **PickPeer**: push `PeerPickerScreen`, callback stores the recipient and enters `Palette`. **Palette**: 4×2 grid for that recipient; selected zigmoji animates while the rest stay static. RIGHT = send selected zigmoji via API-only task. LEFT clears the recipient and returns to `PickPeer`. **Sending**: spinner + sender. **Done**: "Sent!" with auto-return to `PickPeer`. `onEnter`/`onExit` push/pop foreground accelerator |
| Edit | `firmware/src/ui/GUI.cpp` | Update `kBadgeMenuItems[]`: drop "Zigmoji" old wiring (still keep `Zigmoji` entry but it pushes the new ZigmojiScreen directly), drop "Contacts", drop "New Msg" — main menu becomes `Boop / Zigmoji / Messages / Settings`. Wire `messagesLabel` (exists) and a parallel `zigmojiLabel` callback that reads `ZigmojiInbox::count()` |
| Edit | `firmware/src/boops/BadgeBoops.{h,cpp}` | Add `int countUniqueActivePairings()` — walks `/boops.json`, returns the number of distinct active (`revoked != true`) pairing rows. Per-pair `boop_count` (= `len(connected_at)`) stays untouched in the journal; the new helper is the badge-level metric ("how many different people have I booped"). |
| Edit | `firmware/src/screens/BoopScreen.cpp` | Wherever the user-visible total boop count is shown (post-confirm summary, status line, etc.), call `BadgeBoops::countUniqueActivePairings()` instead of summing per-pair `boop_count` values. Identify the exact callsite during implementation — do a final grep for any local `boop_count`/`boopCount` aggregation across the badge UI and replace with the helper. |

### Verification gate

- Build clean.
- Boot → main menu shows 4 items.
- Messages → thread list with "+ New" → pick → compose with joystick → submit → bubble shows sending → sent.
- Zigmoji with empty queue → peer picker → recipient-specific palette.
- Zigmoji with N unread → playback animation cycles N items + senders → drain → peer picker.
- Send zigmoji from recipient-specific palette → send → returns to recipient choice.
- Long-press LEFT on a failed bubble retries.
- While inside Messages: poll runs at 5s; outside: 60s (verify with serial log timestamps).
- While viewing thread A: incoming in thread A doesn't buzz; in thread B does.
- Boot a badge with N distinct pairings, M repeat boops with one of them. The UI's "boop count" displays N, not N+M. Confirm the per-pair `boop_count` in the journal is unchanged (still tracks repeats, just not surfaced at badge level).

---

## Phase 6 — Boot flow + identity gating (PR 3 starts)

| Action | Path | Notes |
|---|---|---|
| Delete | `firmware/src/screens/SplashScreen.{h,cpp}` | No branded boot interstitial; `GUIManager::begin` owns the identity gate directly |
| Edit | `firmware/src/ui/GUI.cpp` | `GUIManager::begin` starts on `MainMenu` when paired and `QR` when unpaired. Delete the old splash registration/push path |
| Edit | `firmware/src/screens/QRScreen.cpp` | LEFT button while `badgeState != BADGE_PAIRED`: no-op + `Haptics::shortPulse()`. Once paired: LEFT → `replaceScreen(MainMenu)` |
| Edit | `firmware/src/ui/GUI.cpp` | Drop "QR / Pair" entry from `kBadgeMenuItems[]` |

### Verification gate

- Boot a paired badge: main menu appears immediately.
- Boot an unpaired badge (factory reset): QR appears immediately. LEFT does nothing. Pair via web → QR auto-replaces with MainMenu.
- Confirm no `REPLAY 26` splash/interstitial appears during boot.

---

## Phase 7 — Cleanup (continues PR 3)

| Action | Path | Notes |
|---|---|---|
| Delete | `firmware/src/screens/ContactsScreen.{h,cpp}` | |
| Delete | `firmware/src/screens/ContactDetailScreen.{h,cpp}` | |
| Delete | `firmware/src/screens/ContactFieldViewScreen.{h,cpp}` | |
| Delete | `firmware/src/ir/BadgeIRNotifyStream.cpp` | Already `#if 0`'d — formal removal |
| Edit | `firmware/src/ui/GUI.cpp` | Drop `sContacts`, `sContactDetail`, `sContactFieldView` static instances + their `registerScreen` calls. Drop `kScreenContacts`, `kScreenContactDetail`, `kScreenContactFieldView` from `Screen.h`. Drop `onZigmojiSelected` and `onNewMessageSelected` callbacks |
| Verify | grep | Search `firmware/src/` for: `BadgeOutbox`, `SendBoopBackground`, `SendBoopConfirm`, `ContactsScreen`, `ContactDetailScreen`, `ContactFieldViewScreen`, `BadgeIRNotifyStream`. Should find zero matches |

### Verification gate

- Build clean.
- Final grep for dead references is empty.

---

## Phase 8 — Documentation (completes PR 3)

| Action | Path | Notes |
|---|---|---|
| Edit | `CLAUDE.md` | Menu Structure section: full rewrite to 4-item menu. Module Reference `messaging/` row: `MessageInbox`, `ZigmojiInbox`, `SendMessage`, `BadgeJournal`. Gotchas section: drop the IR_NOTIFY note; add a one-liner about the 60s/5s poll cadence and foreground accelerator |
| Edit | `firmware/src/README.md` | Topical folder map row for `messaging/` updated. "How do I…" recipe table — new entries: *send a message*, *receive a zigmoji*, *use the message keyboard*, *add a new ping `activity_type`*. Drop recipes referencing Contacts |
| Edit | `specs/openapi.json` | Backend agent lands their changes here. Run `python3 scripts/gen_badge_api_types.py` to regenerate `firmware/src/api/BadgeAPI_types.h` |
| Create | `specs/010-messaging-cleanup/spec.md` | Captures the design: rationale, decisions (boop/ping naming, IR-only-pairing, ephemeral zigmojis with playback queue, joystick keyboard, V1+R1 status), API contract changes, file inventory delta, test plan |
| Edit | `specs/009-ping-mechanic-demo/` | Add a `SUPERSEDED.md` or top-of-spec banner pointing to spec-010 |
| Edit | Per-file headers | Every new/renamed file: `MessageInbox.h`, `ZigmojiInbox.h`, `SendMessage.h`, `PeerPickerScreen.h`, `MessageComposerScreen.h`, `MouseOverlay.h`, `JsonEscape.h`, `MessageStatusGlyph.h`. Match `BadgeNotifications.h` style: Model / Threading / Persistence / Lifecycle blocks |

### Verification gate

- All three docs read coherently to a coworker who didn't see this design conversation.
- spec.md compiles a clear "why" for the design.
- Per-file headers explain enough that future-you doesn't reverse-engineer intent from method bodies.

---

## End-to-end verification (after all three PRs)

Real hardware, paired pair of badges:

1. Boot unpaired badge → forced to QR. Pair → main menu.
2. Boot paired badge → main menu directly (no QR).
3. Boop pair with second badge → both `/boops.json` updated → both PeerPickers show each other.
4. Send text from A → appears in B's MessagesScreen unread + buzz; B opens thread → glyphs show ✓ on A's bubble after first poll on A.
5. Send zigmoji from A → B's main menu shows `Zigmoji (1)` + buzz; B opens app → playback animation cycles, sender name shown, drains, lands on recipient choice.
6. WiFi disconnect on A → send fails with `failed-pre-send` glyph. Reconnect, long-press LEFT → retries → ✓.
7. While viewing A↔B thread on A: B sends a message; A's haptic does NOT fire; bubble appears.
8. While viewing thread list on A: B sends a message; A's haptic DOES fire.
9. Inside Messages app for 30s: serial log confirms 5s poll cadence. Exit to menu: cadence drops to 60s.
10. Inside MessageComposer: joystick moves cursor; RIGHT types letter under cursor; UP submits.
11. Settings → Keyboard layout: cycle QWERTY/ABC; new MessageComposer entry uses selected layout.
12. Battery: 8-hour idle test with WiFi connected + 60s polling — capture mAh delta; expect ~5-8 mAh.

---

## Out of scope

The following are deliberately NOT changed:

- `BoopScreen` and the IR pairing handshake — unchanged.
- `BadgeBoops` journal/protocol/handlers/feedback split — recent refactor, stays.
- `TextInputScreen` — kept as-is for badge-info edit and other non-message text input. The new `MessageComposerScreen` is a parallel screen, not a replacement.
- `BadgePendingBoopApi` — offline pairing reconciler, unrelated to messaging.
- The MicroPython runtime, Python `/apps/` system, and existing Python HAL surface.
- Any LED matrix / haptic pattern changes beyond the existing `Haptics::shortPulse()` calls.
