# Research: Badge Ping Demo — Async Ping Mechanic Showcase

**Branch**: `009-ping-mechanic-demo` | **Date**: 2026-03-20

---

## Decision 1: Contact Storage Design

**Decision**: No NVS contact storage. The server is the authoritative source for the contact list. `GET /api/v1/boops?badge_uuid=<my_uuid>` returns all pairings including partner ticket UUIDs; `GET /api/v1/boops/{id}/partner?badge_uuid=<my_uuid>` provides partner names. The badge fetches both on demand when Messages or Conquest opens.

**Rationale**: The real backend spec exposes `GET /api/v1/boops?badge_uuid=` returning `BoopRecord[]`. Each `BoopRecord` includes `ticket_uuids` (sorted 2-element array of both participants' ticket UUIDs), `boop_count` (pre-computed), and `revoked_at`. This is exactly the data needed — no NVS duplication required. Fetching on demand keeps contact data current (reflects revocations, etc.) and eliminates a class of NVS write bugs.

**`BoopRecord` fields used:**
- `id` (int) — pairing ID, used for per-pairing partner info fetch
- `ticket_uuids` ([str, str]) — sorted; one is mine, one is the partner's `target_ticket_uuid`
- `boop_count` (int) — pre-computed `len(connected_at)` — boops between this specific pair
- `revoked_at` (str or null) — filter out non-null entries; only active pairings shown

**Partner ticket UUID resolution**: Given `BoopRecord.ticket_uuids = [A, B]` (sorted), the partner's ticket UUID is whichever element ≠ my own ticket UUID. Requires the badge to know its own ticket UUID — see Decision 1a below.

**Partner name/company**: Fetched via `GET /api/v1/boops/{id}/partner?badge_uuid=<my_uuid>` per pairing on Messages/Conquest open. N+1 calls are acceptable — conference-scale boop count is small (<50).

**Army size for Conquest**: Count of non-revoked pairings = `len([p for p in pairings if p.revoked_at is None])`. Pre-computed by the server per pairing via `boop_count`; army size = unique peer count, not total boop_count sum.

**Eliminated**: NVS namespace `badge_contacts`, `ContactEntry` struct, `saveContact()`, `loadContacts()`, `boop_cnt` key, `BadgePairing.cpp` NVS write call.

**Alternatives considered**:
- NVS contact cache written at boop time — eliminated; server is already authoritative and badge doesn't retain badge-to-ticket-UUID mapping after boop anyway.
- Single HTTP fetch of boops + no per-partner call — ticket_uuids gives the target for pings but not the name/company for display; `GET /api/v1/boops/{id}/partner` is the only way to get names without storing them.

---

## Decision 1a: My Own Ticket UUID

**Decision**: Store the badge's own `ticket_uuid` in NVS namespace `badge_identity` (key `ticket_uuid`) at enrollment time. Used to resolve partner from `BoopRecord.ticket_uuids`.

**Rationale**: `BoopRecord.ticket_uuids` is sorted and contains both participants. To find the partner, the badge must know which element is itself. The badge hardware UUID is not stored in `BoopRecord` — only ticket UUIDs. The enrollment endpoint `POST /api/v1/link-user-to-badge` returns `ticket_uuid` in the response; this should be persisted to NVS alongside `hmac_secret` and `enrolled`.

**Fallback**: `GET /api/v1/lookup-attendee/<my_badge_uuid>` also returns `ticket_uuid` and can be called at boot if NVS is empty (e.g., pre-spec-009 enrolled badges). Store result in NVS for subsequent calls.

**NVS addition**: Key `ticket_uuid` (string, 37 bytes) in namespace `badge_identity`.

**Alternatives considered**:
- Call `GET /api/v1/lookup-attendee` on every Messages open — works but adds latency each time.
- Use badge hardware UUID as `source_badge_uuid` in pings (already done) and never resolve own ticket UUID — not sufficient; can't resolve partner from `ticket_uuids` without knowing own ticket UUID.

---

## Decision 2: HMAC Authentication (Blocker)

**Decision**: Implement HMAC auth inside `BadgeAPI.cpp` as part of this spec. Without it, `POST /api/v1/pings` returns 401 and the feature does not function.

**Rationale**: Constitution Principle IV states HMAC is required for pings. The constitution specifies the auth scheme precisely: `X-Badge-ID` (badge UUID), `X-Timestamp` (unix seconds), `X-Signature` (lowercase hex HMAC-SHA256 of `badge_uuid_str + unix_timestamp_str`). The HMAC key is stored in NVS (`badge_identity` namespace, key `hmac_secret`, 32-byte blob). The ESP32 has a hardware HMAC peripheral accessible via `esp_hmac_calculate()`.

**Alternatives considered**:
- Skip HMAC, ask backend team to whitelist dev mode — requires backend coordination and doesn't test the full production path.
- Use BYPASS flag — already used for non-auth flows; could extend to skip HMAC but devalues the demo as a validation exercise.
- Defer HMAC to a follow-on spec — would produce a broken spec-009 since pings are the entire feature.

**Implementation approach**: Add `BadgeAPI_attachHMACHeaders(HTTPClient& client)` as an internal helper in `BadgeAPI.cpp`. Called by `sendPing` and `getPings` before the request. Reads `hmac_secret` from NVS, reads current time (NTP-synced `time(nullptr)`), computes HMAC with `esp_hmac_calculate()`.

**NTP dependency**: NTP sync is already performed during WiFi connect in the boot sequence. No new work needed.

---

## Decision 3: Incoming Ping Polling Strategy (C Layer — Messages)

**Decision**: Poll from the main loop on Core 1 when Messages screen is active. Poll interval: 5 seconds. Cursor-based to avoid re-displaying already-seen pings.

**Rationale**: Core 0 is reserved for IR hardware (constitution Principle II / FreeRTOS partition). Adding a second HTTP task on Core 0 risks contention with the IR task's `HTTPClient` usage. Core 1 (main loop) is safe for HTTP calls; the display mutex already gates rendering. A 5-second poll interval matches async messaging expectations without hammering the backend.

**Alternatives considered**:
- FreeRTOS task on Core 0 for ping polling — risks `HTTPClient` stack collision with IR task.
- WebSocket / server-sent events — not supported by the current `HTTPClient` setup; no keep-alive transport.
- Long-polling — would block the display loop; not compatible with the badge UI loop architecture.

**Cursor implementation**: Store `lastPingId` and `lastPingTs` in heap memory while Messages is active. Pass as `before_id`/`before_ts` on next poll. Reset to empty on first poll after entering Messages.

---

## Decision 4: Menu Slot Allocation

**Decision**: Insert "Messages" at index 1, shifting "QR / Pair" to index 2, "Input Test" to index 3, "Apps" to index 4. `MENU_COUNT` becomes 5.

**Rationale**: Messages is a social feature closely related to Boop (index 0), so placing it adjacent is intuitive. "QR / Pair", "Input Test", and "Apps" are lower-frequency actions. Menu label stays `"Messages"`.

**Alternatives considered**:
- Append at index 4 (before Apps) — less intuitive; Boop and Messages are logically related.
- Replace "Input Test" — Input Test is a dev tool; removing it is a behavioral deviation from Firmware-0308 without documented justification. Keep it.

---

## Decision 5: Python Bridge Additions for Conquest App

**Decision**: Add three new functions to the `badge` C extension module:

| Function | Returns | Notes |
|----------|---------|-------|
| `badge.boops()` | str (JSON array) | Calls `BadgeAPI_getBoops` + partner info; returns enriched contact list; empty array `[]` if none |
| `badge.my_uuid()` | str | Badge hardware UUID — used as `source_badge_uuid` in pings and `requester_badge_uuid` in polls |
| `badge.my_ticket_uuid()` | str | Badge's own ticket UUID from NVS — required as `target=` param when polling for incoming pings |

**Rationale**: The Conquest app needs the contact list (with partner ticket UUIDs and names) and its own ticket UUID to construct valid ping poll URLs. Python has no NVS or HTTP access directly (constitution Principle I). These bridges follow the same pattern as `badge.uid()` and `badge.server_url()` in `BadgePython_bridges.cpp`.

**Why `badge.my_ticket_uuid()` is needed**: `GET /api/v1/pings` requires at least one of `source` or `target`. To poll for *incoming* pings (challenges/responses directed at this badge), the app must pass `target=<my_ticket_uuid>`. The hardware UUID alone is insufficient — the server maps it to a ticket UUID internally, but the query param expects a ticket UUID string.

**`badge.boops()` return format:**
```json
[
  {"pairing_id": 42, "ticket": "...", "name": "Alice", "company": "Acme"},
  {"pairing_id": 17, "ticket": "...", "name": "Bob",   "company": "Beta"}
]
```

**Army size**: `len(ujson.loads(badge.boops()))` — count of active pairings returned. No separate `boop_count()` bridge needed.

---

## Decision 6: Conquest Auto-Response Architecture

**Decision**: The Conquest app polls for incoming `conquest_challenge` pings in its event loop. When a challenge arrives, it auto-responds with a return ping — no user interaction required. This matches FR-013.

**Rationale**: Python is single-threaded and synchronous. The app must alternate between: (a) displaying the contact list and waiting for user input, and (b) polling for incoming challenges and auto-responding. A tight poll loop with BTN_RIGHT as a non-blocking escape (checked each iteration) provides the correct behavior.

**Polling timeout**: 20 seconds for awaiting a response (after challenger sends). Defender auto-responds within 5 seconds of receiving (one poll cycle). Total round-trip ceiling: ~25 seconds.

**Simultaneous challenge edge case**: Both badges send a challenge and both receive a challenge at roughly the same time. Each badge auto-responds to the incoming challenge (triggering two response pings). Each badge is waiting for a response; both will receive a response (possibly their own reply and the other's). The app must handle multiple pings arriving and take only the first response, ignoring subsequent ones.

---

## Decision 7: Emoji Set for Messages

**Decision**: Use 8 Unicode emoji encoded as UTF-8 strings, rendered by U8G2 using the `u8g2_font_unifont_t_symbols` or similar symbol font. If the display font does not support emoji glyphs, fall back to short ASCII labels (e.g., `:)`, `<3`, `!`, `lol`, etc.).

**Rationale**: The spec says "on-screen representation depends on what the display font supports." The U8G2 library includes the `u8g2_font_unifont_t_emoticons` font which contains a limited emoji set. Alternatively, short ASCII strings are universally renderable.

**Preliminary emoji set (may be adjusted at build time):**
`♥`, `★`, `✓`, `⚡`, `☺`, `!`, `?`, `…`

If none are renderable: `<3`, `*`, `ok`, `zap`, `hi`, `!!`, `??`, `...`

---

## Resolved Unknowns

| Unknown | Resolution |
|---------|-----------|
| Is `BadgeAPI_sendPing` implemented? | No. Must be added. |
| Is `BadgeAPI_getPings` implemented? | No. Must be added. |
| Is `BadgeAPI_getBoops` implemented? | No. Must be added. (`GET /api/v1/boops?badge_uuid=` exists on server.) |
| Is HMAC auth implemented? | No. Must be implemented as part of this spec. |
| Are contacts persisted in NVS? | Not needed — server is authoritative via `GET /api/v1/boops`. |
| Is boop count tracked? | Server-side: `BoopRecord.boop_count` per pairing; army size = len(active pairings). |
| Can Python read contacts? | New `badge.boops()` bridge calls `BadgeAPI_getBoops` + partner info. |
| Is `PingRecord` struct defined? | No. Must be added to `BadgeAPI_types.h`. |
| Does `BoopRecord` type exist? | No. Must be added to `BadgeAPI_types.h`. |
| Does badge know its own ticket_uuid? | Not currently stored. Must add to `badge_identity` NVS at enrollment. |
