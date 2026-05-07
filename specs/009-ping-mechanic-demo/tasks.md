# Tasks: Badge Ping Demo — Async Ping Mechanic Showcase

**Input**: Design documents from `/specs/009-ping-mechanic-demo/`
**Prerequisites**: plan.md ✅, spec.md ✅, research.md ✅, data-model.md ✅, contracts/ ✅, quickstart.md ✅

**Tests**: No test tasks generated — manual verification per quickstart.md only (no automated tests requested).

**Organization**: Tasks are grouped by user story. Foundation phase (Phase 2) blocks both US1 and US2 and must complete before either story begins.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel with other [P] tasks in the same phase (different files)
- **[Story]**: Which user story this task belongs to (US1, US2, US3)
- File paths are relative to repo root

---

## Phase 1: Setup (Type Declarations & Config)

**Purpose**: Declare all new types, function signatures, and constants before any implementation. All four tasks touch different files and can run in parallel.

- [x] T001 [P] Add `saveMyTicketUUID` / `loadMyTicketUUID` function prototypes to `firmware/Firmware-0308-modular/BadgeStorage.h`
- [x] T002 [P] Add `BoopRecord`, `GetBoopsResult`, `PingRecord`, `SendPingResult`, `GetPingsResult` structs and `PING_TYPE_EMOJI` / `PING_TYPE_CHALLENGE` / `PING_TYPE_RESPONSE` / `PING_FETCH_MAX` constants to `firmware/Firmware-0308-modular/BadgeAPI_types.h`
- [x] T003 [P] Add `getBoops`, `sendPing`, `getPings` function declarations to `firmware/Firmware-0308-modular/BadgeAPI.h`
- [x] T004 [P] Add `#define MSG_POLL_INTERVAL_MS 5000` to `firmware/Firmware-0308-modular/BadgeConfig.h.example`

**Checkpoint**: All headers declared — implementation can begin

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure required by BOTH US1 and US2. No user story work can start until this phase completes.

**⚠️ CRITICAL**: US1 and US2 both depend on ticket_uuid NVS storage and all three BadgeAPI functions.

- [x] T005 Implement `saveMyTicketUUID` and `loadMyTicketUUID` in `firmware/Firmware-0308-modular/BadgeStorage.cpp` — write/read key `ticket_uuid` (string, 37 bytes) in NVS namespace `badge_identity` (depends T001)
- [x] T006 [P] Write `ticket_uuid` to NVS at enrollment time in `firmware/Firmware-0308-modular/BadgePairing.cpp` — call `BadgeStorage::saveMyTicketUUID(response.ticket_uuid)` alongside existing `hmac_secret` / `enrolled` writes (depends T005)
- [x] T007 [P] Add boot fallback for `ticket_uuid` in `firmware/Firmware-0308-modular/Firmware-0308-modular.ino` — if `loadMyTicketUUID` returns false after WiFi connects, call `GET /api/v1/lookup-attendee/<badge_uuid>`, parse `ticket_uuid` field, and call `saveMyTicketUUID` (depends T005)
- [x] T008 Implement `attachHMACHeaders(HTTPClient& client, const char* badge_uuid)` as a file-scope static helper in `firmware/Firmware-0308-modular/BadgeAPI.cpp` — reads `hmac_secret` blob from NVS, reads `time(nullptr)`, computes HMAC-SHA256 via `esp_hmac_calculate`, sets `X-Badge-ID` / `X-Timestamp` / `X-Signature` headers; no-op if `hmac_secret` absent (depends T002)
- [x] T009 Implement `BadgeAPI::getBoops(const char* badge_uuid)` → `GetBoopsResult` in `firmware/Firmware-0308-modular/BadgeAPI.cpp` — `GET /api/v1/boops?badge_uuid=<id>`; filter out records where `revoked_at` is non-empty before populating `boops[]`; no HMAC required (depends T002)
- [x] T010 Implement `BadgeAPI::sendPing(source_badge_uuid, target_ticket_uuid, activity_type, data_json)` → `SendPingResult` in `firmware/Firmware-0308-modular/BadgeAPI.cpp` — `POST /api/v1/pings`; call `attachHMACHeaders` before request; return `ok=false` + `httpCode` on 403/422/503/network error (depends T008, T009)
- [x] T011 Implement `BadgeAPI::getPings(requester_badge_uuid, target_ticket_uuid, activity_type, limit, before_ts, before_id)` → `GetPingsResult` in `firmware/Firmware-0308-modular/BadgeAPI.cpp` — `GET /api/v1/pings`; call `attachHMACHeaders`; build query string with required `target` param; populate `nextCursorTs`/`nextCursorId` from response (depends T008, T009)

**Checkpoint**: Foundation complete — US1 and US2 can now proceed independently

---

## Phase 3: User Story 1 — Messages: C-layer async ping (Priority: P1) 🎯 MVP

**Goal**: Add a "Messages" menu item where badge holders browse booped contacts, send one of 8 emoji as an async ping, and see incoming emoji from others.

**Independent Test**: Two previously-booped badges. Badge A opens Messages, picks Badge B, sends an emoji. Badge B displays the emoji and sender name within ~10 seconds. Verify ping record at `GET /api/v1/pings?type=emoji`.

- [x] T012 [US1] Update `firmware/Firmware-0308-modular/BadgeMenu.h` — change `MENU_COUNT` from `4` to `5`; add `MENU_MESSAGES` constant at index 1 (shifting QR_PAIR → 2, INPUT_TEST → 3, APPS → 4)
- [x] T013 [US1] Update `firmware/Firmware-0308-modular/BadgeMenu.cpp` — insert `"Messages"` label at index 1 in the menu items array; shift remaining entries accordingly (depends T012)
- [x] T014 [US1] Implement Messages screen state machine — extracted to `firmware/Firmware-0308-modular/BadgeMessages.h/.cpp` (396 lines, per the optional extraction path noted in tasks); declares `MessagesState` enum and all state variables; implements LOADING, CONTACT_LIST, EMOJI_PALETTE, SENDING, SENT, ERROR, INCOMING states (depends T003, T009, T011, T012)
- [x] T015 [US1] Implement EMOJI_PALETTE + SENDING + SENT + ERROR screen states — implemented in `BadgeMessages.cpp` (depends T014)
- [x] T016 [US1] Implement incoming ping poll + INCOMING sub-state — implemented in `BadgeMessages.cpp` (depends T015, T004)
- [x] T017 [US1] Wire Messages screen entry/exit to main `loop()` in `firmware/Firmware-0308-modular/Firmware-0308-modular.ino` (depends T016)

**Checkpoint**: Build and flash. Run Messages quickstart test. US1 is independently verifiable.

---

## Phase 4: User Story 2 — Conquest App: Python-layer async battle (Priority: P2)

**Goal**: A MicroPython app in the Apps menu where badge holders challenge each other using boop count as army size; auto-response and async result display.

**Independent Test**: Two previously-booped badges running Conquest. Badge A selects Badge B and confirms. Badge B auto-responds. Badge A shows battle outcome with both army sizes.

- [x] T018 [US2] Implement `badge.boops()` C bridge in `firmware/Firmware-0308-modular/BadgePython_bridges.cpp` — calls `BadgeAPI_getBoops(my_badge_uuid)` then per-pairing `GET /api/v1/boops/{id}/partner`; resolves partner ticket UUID from `BoopRecord.ticket_uuids` using own ticket UUID from NVS; returns JSON array string `[{"pairing_id":N,"ticket":"...","name":"...","company":"..."},...]`; returns `"[]"` on error or empty (depends T009, T005)
- [x] T019 [US2] Implement `badge.my_uuid()` and `badge.my_ticket_uuid()` C bridges in `firmware/Firmware-0308-modular/BadgePython_bridges.cpp` — `badge.my_uuid()` returns hardware UID string (reuse `BadgeUID::getUID()` pattern); `badge.my_ticket_uuid()` calls `BadgeStorage::loadMyTicketUUID`, returns empty string if not stored (depends T018, T005)
- [x] T020 [US2] Create `firmware/Firmware-0308-modular/apps/conquest.py` — implement app entry point: call `badge.boops()`, parse with `ujson`, compute `army_size = len(boops)`; if empty → render EMPTY screen ("No contacts. / Boop someone / first!") with BTN_RIGHT exit; otherwise → LIST screen (depends T019)
- [x] T021 [US2] Implement LIST screen in `firmware/Firmware-0308-modular/apps/conquest.py` — render "Conquest / Army: N / > Name (Company) / ..." scrollable with UP/DOWN wrapping; passive 3s poll for incoming `conquest_challenge` pings using `badge.http_get(POLL_CHALLENGES)` — on challenge received: auto-send `conquest_response` ping via `badge.http_post(SEND_URL, body)` with `{"army_size": my_army}`, flash "Challenge received!" for 1.5s, return to LIST; BTN_RIGHT: `badge.exit()`; confirm (BTN_DOWN on selected): → SENDING; call `badge.gc_collect()` each loop iteration (depends T020)
- [x] T022 [US2] Implement SENDING screen in `firmware/Firmware-0308-modular/apps/conquest.py` — render "Conquest / Challenging / \<name\>..."; POST `conquest_challenge` ping via `badge.http_post(SEND_URL, body)` with `{"source_badge_uuid": MY_UUID, "target_ticket_uuid": target_ticket, "activity_type": "conquest_challenge", "data": {"army_size": my_army}}`; on success → WAITING; on error → brief error display + back to LIST; record `challenge_sent_at = time.ticks_ms()` (depends T021)
- [x] T023 [US2] Implement WAITING screen in `firmware/Firmware-0308-modular/apps/conquest.py` — render "Conquest / Waiting for / \<name\>... / [Ns]"; poll `badge.http_get(POLL_RESPONSES)` every 3s; on response received → RESULT; on `time.ticks_diff(ticks_ms(), challenge_sent_at) > 20000` → display "No response" briefly + back to LIST; BTN_RIGHT: back to LIST (depends T022)
- [x] T024 [US2] Implement RESULT screen and battle resolution in `firmware/Firmware-0308-modular/apps/conquest.py` — parse `their_army` from response ping `data.army_size`; compare: if `my_army > their_army` → "VICTORY!", if `my_army < their_army` → "Defeated.", if equal → "DRAW!"; render with "You: N  They: M" (or "Both: N" for draw); display 4s then auto-return to LIST; BTN_RIGHT: immediate return to LIST (depends T023)

**Checkpoint**: Build and flash. Run Conquest quickstart test. US2 is independently verifiable.

---

## Phase 5: User Story 3 — Cross-feature session integrity (Priority: P3)

**Goal**: Verify that Messages and Conquest operate correctly in the same session with no shared-state corruption between modes.

**Independent Test**: On one badge: complete a Messages send, then launch Conquest and run a full challenge/response cycle. Neither feature should crash or produce incorrect data from the other.

- [x] T025 [US3] Build and flash — run `./build.sh -n` from `firmware/Firmware-0308-modular/` confirming zero errors, then `./flash.py` to both badges (depends T024)
- [ ] T026 [US3] Verify feature via badge serial logs — run `./serial_log.py` from `firmware/Firmware-0308-modular/` with both badges connected; run two-badge test per `specs/009-ping-mechanic-demo/quickstart.md`; confirm log evidence in `logs/BADGE-A.log` + `logs/BADGE-B.log` of (1) emoji ping sent + received, (2) Conquest challenge sent + auto-response received + battle result displayed, (3) both features work in the same session; implementation is NOT complete until logs confirm correct behavior (depends T025)

**Checkpoint**: All three user stories verified end-to-end

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Dead code removal, audit, and documentation updates

- [x] T027 [P] Scan and remove stale references across `firmware/Firmware-0308-modular/` — grep for `badge_contacts`, `boop_cnt`, `ContactEntry`, `badge.contacts()`, `badge.boop_count()`; delete any remaining declarations, implementations, or call sites
- [x] T028 [P] Verify `specs/openapi.json` matches all endpoints called in `firmware/Firmware-0308-modular/BadgeAPI.cpp`; if `BoopRecord` / `PingRecord` structs diverged from OpenAPI schema, regenerate `firmware/Firmware-0308-modular/BadgeAPI_types.h` via `python3 scripts/gen_badge_api_types.py`
- [x] T029 [P] Run `.specify/scripts/bash/update-agent-context.sh claude` from repo root to update `CLAUDE.md` with spec-009 technology entries
- [x] T030 Add `// spec-009` markers to all new functions and constants in modified source files: `BadgeStorage.h/.cpp`, `BadgeAPI_types.h`, `BadgeAPI.h/.cpp`, `BadgePython_bridges.cpp`, `BadgeMenu.h/.cpp`
- [x] T031 Constitution compliance audit — re-read all 7 constitution principles against the final diff; confirm no silent deviations; document any intentional ones (Python HMAC gap) in the commit message body

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 (Setup)**: No dependencies — all 4 tasks start immediately and run in parallel
- **Phase 2 (Foundational)**: Depends on Phase 1 — BLOCKS US1 and US2
  - T005 depends on T001
  - T006, T007 depend on T005 (different files → parallel with each other)
  - T008 depends on T002
  - T009 depends on T002
  - T010 depends on T008, T009 (same file — sequential)
  - T011 depends on T008, T009 (same file — sequential after T010)
- **Phase 3 (US1)**: Depends on Phase 2 complete
  - T012 → T013 (T013 needs constant from T012)
  - T014 → T015 → T016 → T017 (same file — sequential)
- **Phase 4 (US2)**: Depends on Phase 2 complete
  - T018 → T019 (same file — sequential)
  - T019 → T020 → T021 → T022 → T023 → T024 (same file — sequential)
- **Phase 5 (US3)**: Depends on Phase 3 AND Phase 4 complete
- **Phase 6 (Polish)**: Depends on Phase 5 — T027/T028/T029 parallel; T030/T031 sequential after

### User Story Dependencies

- **US1 (P1)**: Unblocked by Phase 2 only — no dependency on US2
- **US2 (P2)**: Unblocked by Phase 2 only — no dependency on US1
- **US3 (P3)**: Requires US1 AND US2 complete

### Parallel Opportunities

**Phase 1** — all 4 tasks:
```
T001  T002  T003  T004   (different files, no deps)
```

**Phase 2** — after T001/T002 complete:
```
T006  T007             (BadgePairing.cpp vs .ino — different files)
T008 → T009 → T010 → T011   (all BadgeAPI.cpp — sequential)
```

**Phase 3 + Phase 4** — after Phase 2:
```
US1: T012 → T013 → T014 → T015 → T016 → T017
US2: T018 → T019 → T020 → T021 → T022 → T023 → T024
(can be assigned to separate developers)
```

**Phase 6** — after Phase 5:
```
T027  T028  T029   (different files/concerns — parallel)
```

---

## Implementation Strategy

### MVP First (US1 Only)

1. Complete Phase 1 (Setup — all parallel, fast)
2. Complete Phase 2 (Foundational — HMAC + API functions + storage)
3. Complete Phase 3 (US1 — Messages)
4. **STOP and VALIDATE**: Flash two badges, run Messages test
5. Demo-ready: emoji ping end-to-end visible on hardware

### Incremental Delivery

1. Phase 1 + Phase 2 → Foundation verified by build
2. Phase 3 → US1 (Messages) demo-able independently
3. Phase 4 → US2 (Conquest) demo-able independently
4. Phase 5 → Combined session verified
5. Phase 6 → Polish + audit before merge

### Critical Completion Gate

Implementation is **NOT complete** until serial logs confirm the feature works on hardware:

```bash
# 1. Build
cd firmware/Firmware-0308-modular/
./build.sh -n 2>&1 | tail -5
# Must end with: "Sketch uses ... bytes" — zero errors

# 2. Flash
./flash.py

# 3. Read logs and verify:
./serial_log.py   # auto-detects both badges, writes logs/BADGE-A.log + logs/BADGE-B.log
# Confirm: emoji ping sent + received, conquest challenge/response cycle, no crashes
```

Build success alone is insufficient. Log evidence required.

---

## Notes

- T006 and T007 are both small additions to different files — fast parallel wins
- `attachHMACHeaders` (T008) is the key unlocker: without it, `sendPing`/`getPings` return 401
- Messages screen state machine (T014–T017) is the largest single task; extract to `BadgeMessages.h/.cpp` if it grows beyond ~150 lines
- Python HMAC gap is a known limitation (plan.md §Constitution Check): Conquest sends unauthenticated pings; test against a backend that permits `conquest_*` types without HMAC for the demo
- `badge.my_uuid()` is an alias for the existing `badge.uid()` — can delegate internally rather than re-implementing
