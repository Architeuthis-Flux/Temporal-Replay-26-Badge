# Feature Specification: Modularize Arduino Badge Firmware + API SDK

**Feature Branch**: `004-modularize-arduino-firmware`
**Created**: 2026-03-11
**Status**: Draft

## Context

**Primary source**: `firmware/Firmware-0308/Firmware-0308.ino` — the authoritative 1626-line monolithic badge firmware (rebased from origin/main). This is what gets split.

**Firmware-0308 is a complete rewrite** from earlier sketches with significant new capabilities:
- **U8G2** display library (replaced SSD1306Wire)
- **ArduinoJson** + **Preferences (NVS)** for persistence
- **Badge state machine**: `UNPAIRED → PAIRED → DEMO`
- **IR protocol**: role-encoded NEC address (`ROLE | PKT_TYPE`), roles for Attendee/Staff/Vendor/Speaker, packet types UID/PING/MSG/ACK
- **Dual-core FreeRTOS**: Core 0 owns all IR hardware + HTTP; Core 1 owns display rendering + input polling; display mutex synchronizes them
- **IR phase state machine**: `IDLE → LISTENING → SENDING → WAITING → INCOMING → PAIR_CONSENT → PAIRED_OK / PAIR_FAILED / PAIR_CANCELLED / ...`
- **Modal dialog system**: blocking consent UI for pairing confirmation (runs on Core 0, gates Core 1 via `modalActive`)
- **Tilt nametag** with 1.5s hold gate and contrast-fade flip transition
- **NVS persistence**: pairing state, role, and QR bitmap cache survive reboots
- **API endpoints inline**: `submitPairing`, `fetchQR`, `fetchBadgeXBM` are embedded directly in the monolith

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Developer modifies one subsystem without touching anything else (Priority: P1)

A developer wants to tune the IR pairing timeout or add a new `IrPhase` state. With a 1626-line monolith, every subsystem is interleaved. After modularization, each concern lives in its own file and can be found, read, and changed in isolation.

**Why this priority**: This is the core value of the split — any single module should be understandable and changeable without reading the rest.

**Independent Test**: Open the IR module file. Verify it contains the `IrPhase` enum, `IrStatus` struct, `irTask`, `submitPairing`, and related helpers — and nothing from display, NVS, or input. Change the `RX_TIMEOUT` or add a new phase. Confirm no other file needs editing.

**Acceptance Scenarios**:

1. **Given** the modularized firmware, **When** a developer opens the IR module, **Then** they see only IR state, phase management, protocol constants, TX/RX logic, and the `submitPairing` backend call — no display draws, no button handling.
2. **Given** the modularized firmware, **When** a developer opens the display module, **Then** they see only U8G2 init, render functions, modal drawing, and the display mutex — no IR logic or NVS reads.

---

### User Story 2 - Developer configures the badge for a new event deployment (Priority: P2)

Before flashing badges for a new event, a developer needs to set WiFi credentials, server URL, PAIRING_TIMEOUT_MS, badge role, and IR NEC address bits. These are currently scattered across the top of the monolith. After modularization, all of this is in a single config file.

**Why this priority**: Deployment configuration must be fast and safe. Editing a 1600-line file to change a URL risks accidental breakage.

**Independent Test**: Open only the config file. Update `WIFI_SSID`, `SERVER_URL`, and `PAIRING_TIMEOUT_MS`. Compile and flash. Verify no logic files were touched.

**Acceptance Scenarios**:

1. **Given** the modularized firmware, **When** a developer opens the config file, **Then** all deployment constants (WiFi, server URL + endpoint paths, timeouts, pin assignments, IR role, bypass flag) are present in one place.
2. **Given** updated config, **When** firmware is flashed, **Then** it connects to the new network and endpoint with no changes to logic modules.

---

### User Story 3 - Firmware interacts with the backend through clean named functions (Priority: P2)

After an IR pairing, the firmware calls `BadgeAPI::createBoop(myUID, theirUID)` and gets back a typed result. It polls via `BadgeAPI::getBoopStatus(workflowId)`. These calls now live inline in `submitPairing()` inside the IR task — HTTP strings, JSON parsing, and polling logic tangled with IR state transitions. After extraction into a `BadgeAPI` module, the IR module calls named functions and receives structured results.

**Why this priority**: The API interaction is the primary online feature of the badge. Keeping it inline in the IR task makes both the IR logic and the API logic harder to test, modify, and review independently.

**Independent Test**: Include only the `BadgeAPI` module and a WiFi connection in a minimal test sketch. Call each function, verify it produces the correct HTTP request, parses the response correctly, and closes the connection in both success and error cases.

**Acceptance Scenarios**:

1. **Given** a connected badge, **When** `BadgeAPI::getBadgeInfo(uid)` is called, **Then** it returns name, title, company, and attendee type.
2. **Given** two UIDs from a completed IR exchange, **When** `BadgeAPI::createBoop(myUID, theirUID)` is called, **Then** it returns `{workflowId, status}` with no raw HTTP in the caller.
3. **Given** a pending workflow ID, **When** `BadgeAPI::getBoopStatus(workflowId, myUID)` is called, **Then** it returns status ("pending"/"confirmed"/"not_requested").
4. **Given** a network error, **When** any API function is called, **Then** it returns a failure result, the HTTP connection is closed, and the IR task continues without crashing.

---

### User Story 4 - NVS persistence is a standalone concern (Priority: P3)

Badge state (paired/unpaired), assigned role, and cached QR bits are saved and loaded via NVS. This logic is currently inline in `nvsSaveQR`, `nvsLoadQR`, `nvsLoadState`, and `nvsSavePaired`. After modularization, a `BadgeStorage` module owns all persistence — any other module that needs to read or write state calls into it.

**Why this priority**: NVS logic is stable and separate by nature; isolating it makes the boot sequence and pairing flow easier to follow.

**Independent Test**: In a minimal sketch with only the storage module, call save and load functions, power-cycle the device, and verify the correct values are restored.

**Acceptance Scenarios**:

1. **Given** a badge that has just paired, **When** `BadgeStorage::savePaired(role)` is called and the badge reboots, **Then** `BadgeStorage::loadState()` returns `BADGE_DEMO` (previously paired, offline boot) and the correct role.
2. **Given** a cached QR in NVS, **When** `BadgeStorage::loadQR(buf, &len)` is called, **Then** it fills the buffer and returns the correct byte count without touching IR or display logic.

---

### Edge Cases

- Core 0 (`irTask`) and Core 1 (`loop`) share `irStatus`, `screenDirty`, `boopListening`, `irPairingRequested`, and `pairingCancelRequested`. The inter-core communication contract must be preserved exactly during the split (volatiles, mutex scope, `modalActive` gate).
- `showModal()` is called from Core 0 but renders to the display — it must remain accessible to the IR module without creating a circular dependency on the display module.
- `submitPairing()` runs HTTP on Core 0 (inside `irTask` stack, 8KB). The extracted `BadgeAPI` functions must fit within the same stack budget.
- The `tiltFadeTransition()` calls `renderScreen()` directly during the transition — the display module must be callable from the tilt poll path on Core 1 without deadlock.
- QR bitmap is heap-allocated (`malloc`) and cached to NVS; the API and storage modules must agree on ownership (who frees, who allocates).

## Requirements *(mandatory)*

### Functional Requirements

**Modularization:**

- **FR-001**: The modularized firmware MUST compile and run with identical behavior to `Firmware-0308.ino`, including all dual-core interactions, NVS persistence, badge state machine transitions, and modal consent flow.
- **FR-002**: All deployment constants (WiFi SSID/password, server URL, endpoint path fragments, `BYPASS`, timeouts, pin definitions, `THIS_BADGE_ROLE`, `TILT_SHOWS_BADGE`) MUST be in a single config file with no duplicate definitions elsewhere.
- **FR-003**: The IR module MUST contain: `IrPhase` enum, `IrStatus` struct, `irStatus` global, `irSetPhase`/`irSetPeer`/`irSetDebug` helpers, `irTask` (Core 0 FreeRTOS task), `submitPairing`, and the IR protocol constants (`ROLE_*`, `PKT_*`, `IR_ADDR`/`IR_GET_ROLE`/`IR_GET_PKT` macros). It MUST NOT contain display draws or NVS calls.
- **FR-004**: The display module MUST contain: U8G2 instance and initialization, display mutex (`displayMutex`, `DISPLAY_TAKE`/`DISPLAY_GIVE`), `renderBoot`/`renderQR`/`renderMain`/`renderScreen`, `renderModal`/`showModal`, `bootPrint`, `drawXBM`, `drawStringCharWrap`, `setDisplayFlip`, and XBM graphics assets (from `graphics.h`). It MUST NOT contain IR logic or NVS calls.
- **FR-005**: The input module MUST contain: `Button` struct and `buttons[]` array, `pollButtons`, `pollJoystick`, joystick position state (`joySquareX/Y`), `pollTilt`, tilt state (`tiltState`, `tiltNametagActive`, `tiltHoldPending`), `tiltFadeTransition`, and `onButtonPressed`. It MUST NOT contain display draws or IR hardware calls.
- **FR-006**: The UID module MUST contain: `uid[UID_SIZE]` array, `uid_hex` string, `read_uid`, and `uid_to_hex`. It MUST halt on eFuse read failure with an error display before any other module runs.
- **FR-007**: The `BadgeStorage` module MUST contain: NVS helpers (`nvsSaveQR`, `nvsLoadQR`, `nvsSavePaired`, `nvsLoadState`) and own the `prefs` (`Preferences`) instance. It MUST expose typed functions that hide NVS key names from callers.
- **FR-008**: The `BadgeAPI` module MUST expose named functions for each backend endpoint (see FR-011–FR-016). The HTTP implementation currently inline in `submitPairing` and `fetchBadgeXBM` MUST be extracted into this module.
- **FR-009**: The main `.ino` file MUST contain only `setup()`, `loop()`, `osRun`, `osConnectWiFi`, `osUnpairedFlow`, `osPairedFlow`, `osDemoFlow`, and `wifiConnect` — high-level orchestration only. Target: under 150 lines.
- **FR-010**: The inter-core communication contract MUST be preserved exactly: `volatile bool` flags (`boopListening`, `irPairingRequested`, `pairingCancelRequested`), `modalActive` gate, and `displayMutex` scope unchanged from the working firmware.

**API SDK Module (`BadgeAPI`):**

- **FR-011**: `BadgeAPI::getBadgeInfo(uid)` MUST call `GET /api/v1/badge/{uid}/info`, parse name, title, company, attendee_type, and bitmap array (used for role assignment), and return a typed result.
- **FR-012**: `BadgeAPI::createBoop(myUID, theirUID)` MUST POST to `/api/v1/boops` with `{"badge_uuids": [...]}`, handle HTTP 200 (confirmed, second badge) and 202 (pending, first badge), and return `{workflowId, status, partnerName}`.
- **FR-013**: `BadgeAPI::getBoopStatus(workflowId, myUID)` MUST call `GET /api/v1/boops/status/{workflowId}?badge_uuid={myUID}` and return the status string.
- **FR-014**: `BadgeAPI::cancelBoop(myUID, theirUID)` MUST send `DELETE /api/v1/boops/pending` with the badge UUID pair and return success/failure.
- **FR-015**: `BadgeAPI::fetchQR(uid, &outBuf, &outLen)` MUST call `GET /api/v1/badge/{uid}/qr.xbm`, parse the XBM hex byte array from the response body, allocate a buffer, and return the byte count.
- **FR-016**: `BadgeAPI::fetchBadgeXBM(uid, &outBuf, &outLen, &outRole)` MUST call `GET /api/v1/badge/{uid}/info`, parse the `bitmap` JSON array and `attendee_type` field, allocate a buffer, and return the assigned role.
- **FR-017**: All `BadgeAPI` functions MUST close the HTTP connection before returning in both success and error paths.
- **FR-018**: All `BadgeAPI` functions MUST return a result struct or enum that distinguishes success from specific failure modes (network error, 404, parse error) without exceptions.
- **FR-019**: A single internal transport helper MUST handle URL construction, headers, timeout, response string extraction, and connection teardown. All public functions delegate to it.
- **FR-020**: Adding HMAC auth headers (`X-Badge-ID`, `X-Timestamp`, `X-Signature`) in the future MUST require changes only to the internal transport — zero changes to public function callers.

### Key Entities

- **Config**: All deployment constants; no functions, no mutable state.
- **UID Module**: eFuse raw bytes + hex string; halts on read failure.
- **IR Module**: `IrPhase` state machine, `IrStatus` struct, Core 0 FreeRTOS task (`irTask`), NEC byte-by-byte TX/RX with role-encoded address, `submitPairing` (delegates HTTP to `BadgeAPI`).
- **Display Module**: U8G2 instance, display mutex, all render functions, modal system, XBM graphics assets.
- **Input Module**: Button debounce array, joystick ADC + deadband + circular clamp, tilt hold gate + fade transition, `onButtonPressed` action dispatch.
- **BadgeStorage Module**: NVS read/write for pairing state, role, and QR cache; owns `Preferences` instance.
- **BadgeAPI Module**: Typed result structs; named functions for all 6 endpoints; private transport helper.
- **Badge State**: `BadgeState` enum (`UNPAIRED`/`PAIRED`/`DEMO`) and `assignedRole` — owned by main sketch, updated by OS boot stages and pairing callbacks.
- **Main Sketch**: `setup()`, `loop()`, boot stage orchestration (`osRun` and its sub-stages), WiFi connect helper.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Modularized firmware compiles without errors and produces a binary that runs correctly on badge hardware — badge state machine, dual-core IR/display, NVS persistence, and API calls all behave identically to Firmware-0308.
- **SC-002**: The main `.ino` file is under 150 lines, containing only `setup()`, `loop()`, boot orchestration, and WiFi connect.
- **SC-003**: Any single functional concern (IR, display, input, UID, storage, API) can be located and understood by reading exactly one file.
- **SC-004**: Changing WiFi credentials, server URL, or pairing timeout requires editing exactly one file.
- **SC-005**: Adding a new API endpoint requires changes only to the `BadgeAPI` module — no edits to IR, display, input, or storage files.
- **SC-006**: Each `BadgeAPI` function can be exercised from a minimal test sketch (WiFi + `BadgeAPI` only) with no IR hardware or display required.

## Assumptions

- Arduino multi-file sketch convention: `.h` + `.cpp` files in the same sketch directory, with forward declarations in headers as needed.
- Firmware-0308.ino (post-rebase, 1626 lines) is the sole authoritative reference — Firmware-0306 is superseded.
- `showModal()` is called from Core 0 (`irTask`) and uses the display mutex internally; it belongs in the display module even though the IR module calls it. The IR module depends on the display module for modal support.
- `submitPairing`'s HTTP logic is extracted into `BadgeAPI::createBoop` + `BadgeAPI::getBoopStatus` + `BadgeAPI::cancelBoop`; `submitPairing` becomes a thin orchestrator in the IR module that calls those functions and updates `irStatus`.
- No new badge features are added as part of this refactor — behavior parity only.
- HMAC auth is out of scope; the `BadgeAPI` transport design accommodates it as a future single-point change.
- QR bitmap buffer ownership: `BadgeAPI::fetchQR` allocates; the caller (main sketch / storage module) is responsible for freeing.
