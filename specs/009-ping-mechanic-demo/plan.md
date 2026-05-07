# Implementation Plan: Badge Ping Demo — Async Ping Mechanic Showcase

**Branch**: `009-ping-mechanic-demo` | **Date**: 2026-03-20 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/009-ping-mechanic-demo/spec.md`

---

## Summary

Two new features demonstrating the async ping mechanic end-to-end:

1. **Messages** (C layer, new menu item): Browse booped contacts, send one of 8 emoji as a directed async ping, receive incoming emoji from others — all via `POST/GET /api/v1/pings`.
2. **Conquest** (Python layer, Apps menu): Warrior's Way-style army battle. Your boop count is your army. Challenge a contact via a `conquest_challenge` ping; opponent auto-responds; higher boop count wins.

Both features require foundational work not yet present: `BadgeAPI::getBoops`/`sendPing`/`getPings`, HMAC auth in `BadgeAPI.cpp`, and own ticket UUID stored in NVS.

---

## Technical Context

**Language/Version**: Arduino C++ (C++17, ESP32 Arduino core 3.x) + MicroPython v1.27.0 embed port
**Primary Dependencies**: U8G2, ArduinoJson 6.x, HTTPClient, WiFi, Preferences, IRremote, MicroPython embed — all existing; no new library dependencies
**Storage**: ESP32 NVS via `Preferences` library — adding `ticket_uuid` key to existing `badge_identity` namespace only; no new namespaces
**Testing**: Manual verification per quickstart.md checklist; `./build.sh -n` must succeed
**Target Platform**: ESP32-S3-MINI-1 (XIAO), Arduino environment
**Project Type**: Embedded firmware feature + MicroPython app
**Performance Goals**: Send confirmation within 2s of user confirm (SC-004); error surfaced within 10s (SC-002)
**Constraints**: 128×64 OLED (U8G2); synchronous HTTP (no async library); Core 0 reserved for IR; Python heap 128KB; no OTA
**Scale/Scope**: 2,000-unit conference deployment; no backend changes required

---

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked post-design below.*

### Pre-Design Check

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Arduino C++ First | ✅ PASS | C++ owns firmware logic; Conquest runs as a Python app (permitted exception) |
| II. Firmware-0308 Behavioral Source | ✅ PASS | Additive only — new menu item + new app; existing behavior unchanged |
| III. Credentials-at-Build | ✅ PASS | No new secrets; HMAC key already in NVS; BadgeConfig.h pattern unchanged |
| IV. Backend Contract Compliance | ⚠️ CONDITIONAL | HMAC auth required for pings (not yet implemented) — must be resolved in this spec |
| V. Reproducible Build | ✅ PASS | No new library dependencies; all existing pinned dependencies |
| VI. Hardware Safety | ✅ PASS | No GPIO changes; no eFuse writes |
| VII. API Contract Library | ✅ PASS | `sendPing` + `getPings` are listed in Principle VII's function table; being implemented here |

**Blocking gate (Principle IV)**: HMAC auth must be implemented in `BadgeAPI.cpp` before pings can be used against the production backend. Resolved in research.md Decision 2.

### Post-Design Check

| Principle | Status | Resolution |
|-----------|--------|-----------|
| IV. Backend Contract Compliance | ✅ RESOLVED | HMAC added as internal helper in `BadgeAPI.cpp`; callers unaffected |
| VII. API Contract Library | ✅ PASS | New functions conform to ABI rules (typed result structs, no raw JSON/String) |

**Python HMAC gap**: Python-layer `badge.http_post` does NOT attach HMAC headers. The Conquest app sends unauthenticated pings. Backend may reject with 401. Mitigation: test with a backend that permits the `conquest_*` activity types without HMAC for the demo, or add `badge.http_post_auth()` bridge in a follow-on. This is a **known limitation**, not a constitution violation.

---

## Project Structure

### Documentation (this feature)

```text
specs/009-ping-mechanic-demo/
├── plan.md              ← This file
├── research.md          ← Phase 0 output
├── data-model.md        ← Phase 1 output
├── quickstart.md        ← Phase 1 output
├── contracts/
│   ├── badge-api-ping-functions.md   ← BadgeAPI sendPing/getPings + HMAC
│   ├── badge-storage-additions.md    ← ticket_uuid NVS (own ticket UUID only)
│   ├── python-bridge-additions.md    ← badge.boops(), badge.my_uuid(), badge.my_ticket_uuid()
│   ├── messages-screen-ui.md         ← Messages screen states + inputs
│   └── conquest-app-ui.md            ← Conquest app states + logic
└── tasks.md             ← Phase 2 output (/speckit.tasks — not yet created)
```

### Source Code

```text
firmware/Firmware-0308-modular/
├── BadgeAPI_types.h         MODIFY — add BoopRecord, GetBoopsResult, PingRecord,
│                                     SendPingResult, GetPingsResult, PING_TYPE_* constants
├── BadgeAPI.h               MODIFY — declare getBoops(), sendPing(), getPings()
├── BadgeAPI.cpp             MODIFY — implement getBoops, sendPing, getPings,
│                                     attachHMACHeaders (internal)
├── BadgeStorage.h           MODIFY — add saveMyTicketUUID(), loadMyTicketUUID()
├── BadgeStorage.cpp         MODIFY — implement ticket_uuid NVS read/write
├── BadgeMenu.h              MODIFY — MENU_COUNT 4→5, add MENU_MESSAGES constant
├── BadgeMenu.cpp            MODIFY — insert "Messages" at index 1; shift existing items
├── Firmware-0308-modular.ino MODIFY — add Messages screen handler in loop();
│                                     boot: fallback lookup-attendee for ticket_uuid
├── BadgeConfig.h.example    MODIFY — add MSG_POLL_INTERVAL_MS constant
├── BadgePython_bridges.cpp  MODIFY — add badge.boops(), badge.my_uuid(), badge.my_ticket_uuid()
└── apps/
    └── conquest.py          CREATE — Conquest MicroPython app
```

**Structure Decision**: Single firmware project with in-place modifications. No new source files needed except `conquest.py`. Messages screen logic is inline in `Firmware-0308-modular.ino` or extracted to `BadgeMessages.h/.cpp` if it grows beyond ~150 lines.

---

## Complexity Tracking

No constitution violations requiring justification.

---

## Implementation Phases

### Foundation (must complete first — blocks both features)

1. **Own ticket UUID in NVS** — store `ticket_uuid` in `badge_identity` at enrollment; add `BadgeStorage::saveMyTicketUUID` / `loadMyTicketUUID`; fallback lookup at boot
2. **BadgeAPI types** — `BoopRecord`, `GetBoopsResult`, `PingRecord`, `SendPingResult`, `GetPingsResult` in `BadgeAPI_types.h`
3. **HMAC auth helper** — `attachHMACHeaders()` in `BadgeAPI.cpp`
4. **`getBoops`** — `GET /api/v1/boops?badge_uuid=` in `BadgeAPI.cpp`; filters revoked
5. **`sendPing` / `getPings`** — `BadgeAPI.cpp` implementations
6. **Python bridges** — `badge.boops()` (calls getBoops + partner info), `badge.my_uuid()`, `badge.my_ticket_uuid()`

### Messages (C Layer)

7. **Menu change** — `MENU_COUNT` 4→5, insert "Messages" at index 1
8. **Messages screen** — contact list, emoji palette, send flow, error display
9. **Incoming ping display** — poll in Messages loop (`target=my_ticket_uuid`), display INCOMING sub-state

### Conquest App (Python Layer)

10. **Conquest app** — `conquest.py`: LIST/SENDING/WAITING/RESULT screens, auto-respond logic

### Integration

11. **Build verification** — `./build.sh -n` succeeds, no errors
12. **Manual test** — two-badge verification per quickstart.md

### Polish & Audit

13. **Dead code removal** — scan for any stale ContactEntry/boop_cnt/badge.contacts()/badge.boop_count() references left in firmware or apps; remove
14. **openapi.json sync** — verify `specs/openapi.json` matches all endpoints actually called by `BadgeAPI.cpp` after implementation; regenerate `BadgeAPI_types.h` via `scripts/gen_badge_api_types.py` if structs diverged
15. **CLAUDE.md update** — run `.specify/scripts/bash/update-agent-context.sh claude` to reflect spec-009 technologies in agent context
16. **Code comments** — add `// spec-009` markers to new functions/constants so their origin is traceable; no other comment additions
17. **Constitution compliance audit** — re-read all 7 principles against the final diff; confirm no silent deviations; document any intentional ones in commit message

---

## Key Risks

| Risk | Mitigation |
|------|-----------|
| HMAC key not present in NVS (badge not enrolled) | `attachHMACHeaders` is a no-op if key absent; badge sends unauthenticated; backend returns 401; Messages shows error. Acceptable for dev flow. |
| Python HMAC gap (Conquest sends unauthenticated pings) | Backend team to confirm whether `conquest_*` pings are permissible without HMAC for demo, or accept 401 error state as known limitation |
| Own ticket UUID missing on pre-spec-009 badges | Badges enrolled before spec-009 won't have `ticket_uuid` in NVS. Fallback: call `GET /api/v1/lookup-attendee/<machine_id>` at boot and store result |
| Emoji font support on display | U8G2 font selection may not support all emoji. Fallback ASCII labels defined in contracts |
| Simultaneous Conquest challenges | Both badges auto-respond to each other's challenge; app takes first response, ignores subsequent. Logic defined in conquest-app-ui.md |
