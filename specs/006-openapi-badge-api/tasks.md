# Tasks: OpenAPI Badge API Contract Library

**Input**: Design documents from `/specs/006-openapi-badge-api/`
**Prerequisites**: plan.md ✅, spec.md ✅, research.md ✅, data-model.md ✅, contracts/firmware-endpoints.md ✅, quickstart.md ✅

**Organization**: Tasks grouped by user story. Each phase is independently testable.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (operates on different files, no blocking dependency)
- **[Story]**: User story label — US1, US2, US3, US4
- Exact file paths included in all task descriptions

---

## Phase 1: Setup — Versioned API Artifact

**Purpose**: Author `specs/openapi.json` as the source-of-truth contract artifact. All downstream tasks derive from this file.

- [ ] T001 Inspect registrationScanner backend source files (routes/registration.py, routes/boops/__init__.py, routes/boops/pair.py, routes/pings.py) and hand-author specs/openapi.json covering exactly the 7 firmware-facing endpoints: GET /api/v1/badge/{id}/qr.xbm, GET /api/v1/badge/{id}/info, POST /api/v1/boops, GET /api/v1/boops/status/{workflow_id}, GET /api/v1/boops/{id}/partner, POST /api/v1/pings, GET /api/v1/pings — include components/schemas for BadgeInfoResponse, PairingPendingResponse, PairingConfirmedResponse, PairingStatusResponse, PartnerInfoResponse, PingRecord, PingListResponse; exclude all non-firmware endpoints per Decision 7 in specs/006-openapi-badge-api/research.md
- [ ] T002 Validate specs/openapi.json is well-formed JSON and contains paths and components/schemas entries for all 7 firmware-facing endpoints

**Checkpoint**: specs/openapi.json committed — generator and type system can now be built

---

## Phase 2: Foundational — Script Scaffold + Baseline Inspection

**Purpose**: Establish the gen_badge_api_types.py CLI scaffold and capture the existing BadgeAPI surface before any modifications.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [ ] T003 [P] Read firmware/Firmware-0308-modular/BadgeAPI.h and firmware/Firmware-0308-modular/BadgeAPI.cpp to document current public function signatures, return types (noting all String/raw-JSON returns that must change), and the existing _request() or HTTP transport pattern
- [ ] T004 [P] Read firmware/Firmware-0308-modular/BadgePairing.cpp to locate the inline HTTPClient block inside osUnpairedFlow() that will be replaced in US3

- [ ] T005 Write scripts/gen_badge_api_types.py scaffold: argparse with --spec (default: specs/openapi.json) and --out (default: firmware/Firmware-0308-modular/BadgeAPI_types.h) options; load and validate JSON from --spec (exit 1 if missing or invalid, exit 2 if required firmware endpoints absent); define FIRMWARE_ENDPOINTS and FIRMWARE_SCHEMAS allowlists matching the 7 endpoints; emit skipped-schema warning to stdout for any schema not in the allowlist; structure output sections in deterministic (sorted) order; write a #pragma once guard and a "GENERATED — do not edit by hand" comment block at the top of the output file

**Checkpoint**: Foundation ready — generator scaffold exists; existing API surface documented

---

## Phase 3: User Story 1 — Developer Regenerates API Types (Priority: P1) 🎯 MVP

**Goal**: Running `python scripts/gen_badge_api_types.py` produces a complete, compilable `BadgeAPI_types.h` from `specs/openapi.json`. Output is byte-identical on repeated runs.

**Independent Test**: Run `gen_badge_api_types.py` against the committed `specs/openapi.json`; verify `firmware/Firmware-0308-modular/BadgeAPI_types.h` contains struct definitions for all 9 result types; run twice and confirm `diff` produces no output (determinism).

- [ ] T006 [P] [US1] Implement size-constants section in scripts/gen_badge_api_types.py: emit all 9 `#define` constants (BADGE_FIELD_NAME_MAX=64, BADGE_FIELD_TYPE_MAX=16, BADGE_UUID_MAX=37, BADGE_WORKFLOW_ID_MAX=96, BADGE_PING_ACTIVITY_MAX=32, BADGE_PING_DATA_MAX=256, BADGE_PING_CURSOR_MAX=128, BADGE_PING_TIMESTAMP_MAX=32, BADGE_PINGS_MAX_RECORDS=8) with brief rationale comments matching specs/006-openapi-badge-api/data-model.md
- [ ] T007 [P] [US1] Implement BadgeBoopStatus enum emission in scripts/gen_badge_api_types.py: emit `typedef enum { BOOP_STATUS_UNKNOWN=0, BOOP_STATUS_PENDING=1, BOOP_STATUS_CONFIRMED=2, BOOP_STATUS_NOT_REQUESTED=3 } BadgeBoopStatus;` with value comments, matching specs/006-openapi-badge-api/data-model.md
- [ ] T008 [P] [US1] Implement BadgeAPIResult base struct emission in scripts/gen_badge_api_types.py: emit `typedef struct { bool ok; int httpCode; } BadgeAPIResult;` with the zero-field invariant comment
- [ ] T009 [P] [US1] Implement struct emission for BadgeInfoResult, FetchQRResult, FetchBadgeXBMResult, ProbeResult in scripts/gen_badge_api_types.py: field layout and types must exactly match specs/006-openapi-badge-api/data-model.md; FetchQRResult and FetchBadgeXBMResult use heap-allocated `uint8_t* buf` with accompanying `int len` (with caller-must-free comment)
- [ ] T010 [P] [US1] Implement struct emission for BoopResult, BoopStatusResult, BoopPartnerResult in scripts/gen_badge_api_types.py: status field typed as BadgeBoopStatus enum; all char arrays sized by their BADGE_* constant; pairingId as int; all fields and sizes must match specs/006-openapi-badge-api/data-model.md
- [ ] T011 [P] [US1] Implement struct emission for PingRecord, SendPingResult, GetPingsResult in scripts/gen_badge_api_types.py: PingRecord fields sized by BADGE_* constants; GetPingsResult uses `PingRecord records[BADGE_PINGS_MAX_RECORDS]` fixed-capacity array plus `int count` and `char next_cursor[BADGE_PING_CURSOR_MAX]`; all fields must match specs/006-openapi-badge-api/data-model.md
- [ ] T012 [US1] Run `python scripts/gen_badge_api_types.py` from repo root to write firmware/Firmware-0308-modular/BadgeAPI_types.h; inspect the output and confirm all 9 result types and the BadgeBoopStatus enum are present (depends on T006–T011)
- [ ] T013 [US1] Verify determinism: run `python scripts/gen_badge_api_types.py` twice and confirm `diff firmware/Firmware-0308-modular/BadgeAPI_types.h <(python scripts/gen_badge_api_types.py)` produces no output (SC-004)

**Checkpoint**: US1 complete — `gen_badge_api_types.py` produces a correct, deterministic `BadgeAPI_types.h`; firmware can now include it

---

## Phase 4: User Story 2 — Firmware Caller Receives Typed Data (Priority: P2)

**Goal**: All public BadgeAPI functions return typed result structs. No public signature returns `String`, raw JSON, or an untyped pointer. Status fields are `BadgeBoopStatus` enums. All string fields are bounded char arrays.

**Independent Test**: Read `BadgeAPI.h` and grep for `String` — zero matches in public signatures. Read all public function return types and verify each is one of the 9 result structs from `BadgeAPI_types.h`.

- [ ] T014 [US2] Update firmware/Firmware-0308-modular/BadgeAPI.h: add `#include "BadgeAPI_types.h"` at the top; update all public function declarations to use the typed return types from data-model.md — `getBadgeInfo` → `BadgeInfoResult`, `getQRBitmap` (renamed from `fetchQR`) → `FetchQRResult`, `fetchBadgeXBM` → `FetchBadgeXBMResult`, `createBoop` → `BoopResult`, `getBoopStatus` → `BoopStatusResult`, `cancelBoop` → `APIResult` (unchanged); remove any `String` types from public signatures; do NOT add the new US4 stubs yet (depends on T012)
- [ ] T015 [US2] Refactor firmware/Firmware-0308-modular/BadgeAPI.cpp: add a `_request()` private transport helper with signature `static bool _request(const char* method, const char* url, const char* body, String& responseOut, int& httpCodeOut, bool needsAuth = false)`; add auth stub comment block inside: `// AUTH STUB: when needsAuth == true, inject X-Badge-ID, X-Timestamp, X-Signature headers here — currently a no-op` (FR-010, Decision 6 in research.md); use this helper in all HTTP calls (depends on T014)
- [ ] T016 [US2] Refactor `BadgeAPI::getBadgeInfo()` in firmware/Firmware-0308-modular/BadgeAPI.cpp to return `BadgeInfoResult`: populate name, title, company, attendee_type as null-terminated char arrays using `strncpy(..., sizeof(field)-1)` per the buffer safety contract; zero all fields on ok=false (depends on T015)
- [ ] T017 [US2] Refactor `BadgeAPI::getQRBitmap()` (renamed from fetchQR) in firmware/Firmware-0308-modular/BadgeAPI.cpp to return `FetchQRResult`: allocate buf with `malloc()`; set len; set ok=false and len=0 on any failure; update function name to match new declaration in BadgeAPI.h (depends on T015)
- [ ] T018 [US2] Refactor `BadgeAPI::fetchBadgeXBM()` in firmware/Firmware-0308-modular/BadgeAPI.cpp to return `FetchBadgeXBMResult`: populate buf (malloc'd), len, and assignedRole from JSON bitmap array; zero all fields on failure (depends on T015)
- [ ] T019 [US2] Refactor `BadgeAPI::createBoop()` in firmware/Firmware-0308-modular/BadgeAPI.cpp to return `BoopResult`: parse HTTP 202 → `BOOP_STATUS_PENDING` + populate workflowId; parse HTTP 200 → `BOOP_STATUS_CONFIRMED` + populate pairingId + partner char arrays; use `strncpy` with bounds for all char fields; set needsAuth=true on the _request() call (auth stub inactive); zero all fields on ok=false (depends on T015)
- [ ] T020 [US2] Refactor `BadgeAPI::getBoopStatus()` in firmware/Firmware-0308-modular/BadgeAPI.cpp to return `BoopStatusResult`: map "pending"/"confirmed"/"not_requested" strings to BadgeBoopStatus enum values; populate workflowId when PENDING, pairingId + partner fields when CONFIRMED; use `strncpy` with bounds; zero all fields on ok=false (depends on T015)
- [ ] T021 [US2] Update all call sites in firmware/Firmware-0308-modular/ that use the renamed or type-changed functions (any .cpp or .ino files that call fetchQR, getBadgeInfo, createBoop, getBoopStatus): replace String comparisons for status with BadgeBoopStatus enum comparisons; update field accesses to match new struct layouts; do NOT change BadgePairing.cpp HTTPClient block yet (that is US3)

**Checkpoint**: US2 complete — `BadgeAPI.h` has zero String return types; all callers use typed struct fields; firmware should compile

---

## Phase 5: User Story 3 — Activation Polling Encapsulated (Priority: P3)

**Goal**: The badge activation polling loop in `BadgePairing.cpp::osUnpairedFlow()` calls `BadgeAPI::probeBadgeExistence()` instead of constructing an HTTP client inline. No direct `HTTPClient` usage remains in `BadgePairing.cpp` or `Firmware-0308-modular.ino`.

**Independent Test**: `grep -n "HTTPClient" firmware/Firmware-0308-modular/BadgePairing.cpp` → zero results. Build succeeds. Badge activation polling behavior (display messages "conn err", "HTTP 404", trigger `fetchBadgeXBM` on 200) is unchanged.

- [ ] T022 [US3] Add `ProbeResult probeBadgeExistence(const char* uid)` declaration to firmware/Firmware-0308-modular/BadgeAPI.h (depends on T014)
- [ ] T023 [US3] Implement `BadgeAPI::probeBadgeExistence()` in firmware/Firmware-0308-modular/BadgeAPI.cpp: call GET /api/v1/badge/{uid}/info via `_request()`; set ok=true only if httpCode==200; do NOT parse any JSON fields — populate only ok and httpCode in ProbeResult; set needsAuth=false (depends on T015, T022)
- [ ] T024 [US3] Refactor `osUnpairedFlow()` in firmware/Firmware-0308-modular/BadgePairing.cpp: remove the inline HTTPClient block; replace with `auto probe = BadgeAPI::probeBadgeExistence(uid)`; preserve all existing display error messages exactly — `probe.ok=false, httpCode==-1` → "conn err"; `probe.ok=false, httpCode==404` → "HTTP 404"; `probe.ok=true` → call `BadgeAPI::fetchBadgeXBM()` as before (SC-006) (depends on T023)
- [ ] T025 [US3] Verify `grep -n "HTTPClient" firmware/Firmware-0308-modular/BadgePairing.cpp` returns zero matches and `grep -n "HTTPClient" firmware/Firmware-0308-modular/Firmware-0308-modular.ino` returns zero matches (SC-002)

**Checkpoint**: US3 complete — zero raw HTTP calls outside BadgeAPI module; activation polling behavior unchanged

---

## Phase 6: User Story 4 — New Endpoint Stubs (Priority: P4)

**Goal**: `getBoopPartner`, `sendPing`, and `getPings` are callable BadgeAPI functions with typed result structs. The firmware compiles with all three symbols available. No higher-level UI code is required to call them.

**Independent Test**: `grep -n "getBoopPartner\|sendPing\|getPings" firmware/Firmware-0308-modular/BadgeAPI.h` returns 3 matches. `./build.sh` succeeds.

- [ ] T026 [US4] Add three new declarations to firmware/Firmware-0308-modular/BadgeAPI.h: `BoopPartnerResult getBoopPartner(int pairingId, const char* myUID)`, `SendPingResult sendPing(const char* sourceBadgeUuid, const char* targetTicketUuid, const char* activityType, const char* dataJson)`, `GetPingsResult getPings(const char* requesterBadgeUuid, const char* source, const char* target, const char* activityType, int limit, const char* beforeTs, const char* beforeId)` — signatures must exactly match contracts/firmware-endpoints.md (depends on T014)
- [ ] T027 [US4] Implement `BadgeAPI::getBoopPartner()` in firmware/Firmware-0308-modular/BadgeAPI.cpp: build URL `/api/v1/boops/{pairingId}/partner?badge_uuid={myUID}`; GET via `_request()`; on HTTP 200 parse pairingId, partnerName, partnerTitle, partnerCompany, partnerAttendeeType into BoopPartnerResult using strncpy with bounds; zero all fields on ok=false (depends on T015, T026)
- [ ] T028 [US4] Implement `BadgeAPI::sendPing()` in firmware/Firmware-0308-modular/BadgeAPI.cpp: POST to `/api/v1/pings` with JSON body `{source_badge_uuid, target_ticket_uuid, activity_type, data: <dataJson>}`; on HTTP 200 parse PingRecord fields (id, source_ticket_uuid, target_ticket_uuid, activity_type, data, created_at, updated_at) using strncpy with bounds per buffer safety contract; truncate data to BADGE_PING_DATA_MAX-1 with null termination; set needsAuth=true (inactive stub); zero all fields on ok=false (depends on T015, T026)
- [ ] T029 [US4] Implement `BadgeAPI::getPings()` in firmware/Firmware-0308-modular/BadgeAPI.cpp: build URL `/api/v1/pings?requester_badge_uuid=...&source=...` with optional type, limit, before_ts, before_id query params (skip null params); GET via `_request()`; on HTTP 200 iterate `events` JSON array up to BADGE_PINGS_MAX_RECORDS, populate each PingRecord using strncpy with bounds; set count to actual records parsed; copy next_cursor (null → empty string); set needsAuth=true (inactive stub); zero all fields on ok=false (depends on T015, T026)

**Checkpoint**: US4 complete — three new functions declared and implemented; firmware compiles with all 10 BadgeAPI functions (7 named + cancelBoop + fetchBadgeXBM + probeBadgeExistence)

---

## Phase 7: Polish & Verification

**Purpose**: Confirm all success criteria (SC-001 through SC-006) are met.

- [ ] T030 Run `cd firmware/Firmware-0308-modular && ./build.sh` and verify it exits 0 producing a valid .bin artifact — report sketch size output (SC-001)
- [ ] T031 [P] Run `grep -n "String" firmware/Firmware-0308-modular/BadgeAPI.h` and verify zero matches in public function signatures (SC-003)
- [ ] T032 [P] Run `grep -n "HTTPClient" firmware/Firmware-0308-modular/BadgePairing.cpp firmware/Firmware-0308-modular/Firmware-0308-modular.ino` and verify zero matches (SC-002)
- [ ] T033 [P] Run `grep -n "getQRBitmap\|getBadgeInfo\|createBoop\|getBoopStatus\|getBoopPartner\|sendPing\|getPings" firmware/Firmware-0308-modular/BadgeAPI.h` and verify all 7 names appear (SC-005)
- [ ] T034 [P] Verify determinism: run `python scripts/gen_badge_api_types.py && diff firmware/Firmware-0308-modular/BadgeAPI_types.h <(python scripts/gen_badge_api_types.py)` from repo root — confirm no diff output (SC-004)

---

## Dependencies

```
T001 → T002
T002 → T003, T004, T005
T005 + T006 + T007 + T008 + T009 + T010 + T011 → T012 → T013
T012 → T014 → T015 → T016, T017, T018, T019, T020, T021
T015 → T022 → T023 → T024 → T025
T014 → T026 → T027, T028, T029
T021 + T025 + T029 → T030 → T031, T032, T033, T034
```

**US independence**: US3 (T022–T025) and US4 (T026–T029) can proceed in parallel once US2 (T014–T021) is complete. US1 (T006–T013) is independent after Phase 2.

## Parallel Execution Examples

**Within US1** (after T005): T006, T007, T008, T009, T010, T011 can all be implemented in parallel (all different function bodies within gen_badge_api_types.py, no inter-dependency).

**Within Verification** (after T030): T031, T032, T033, T034 are independent read-only checks.

**US3 vs US4** (after T021): T022–T025 and T026–T029 operate on non-overlapping call sites and can proceed in parallel.

## Implementation Strategy

**MVP** (US1 only): Author `specs/openapi.json` → write and run `gen_badge_api_types.py` → confirm deterministic output. Deliverable: committed `BadgeAPI_types.h` with all 9 structs. No firmware changes required.

**Incremental delivery order**: US1 → US2 → US3 → US4. Each phase leaves the firmware in a buildable state. US3 and US4 are pure additions (no behavior change) and can be deferred independently.

## Summary

| Phase | Story | Tasks | Key Deliverable |
|-------|-------|-------|-----------------|
| 1 — Setup | — | T001–T002 | specs/openapi.json |
| 2 — Foundational | — | T003–T005 | scripts/gen_badge_api_types.py scaffold |
| 3 — US1 (P1) | Regenerate types | T006–T013 | firmware/Firmware-0308-modular/BadgeAPI_types.h |
| 4 — US2 (P2) | Typed public API | T014–T021 | BadgeAPI.h/.cpp fully typed |
| 5 — US3 (P3) | Activation polling | T022–T025 | BadgePairing.cpp uses probeBadgeExistence() |
| 6 — US4 (P4) | New stubs | T026–T029 | getBoopPartner, sendPing, getPings callable |
| 7 — Polish | — | T030–T034 | All SC-001–SC-006 verified |

**Total**: 34 tasks | **Parallel opportunities**: 12 tasks marked [P] | **MVP scope**: T001–T013 (US1 only)
