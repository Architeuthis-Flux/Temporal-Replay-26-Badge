# Feature Specification: OpenAPI Badge API Contract Library

**Feature Branch**: `006-openapi-badge-api`
**Created**: 2026-03-13
**Status**: Draft
**Input**: User description: "Refactor BadgeAPI to be an OpenAPI-driven contract library. Commit specs/openapi.json (fetched from the staging backend) as a versioned artifact. Write scripts/gen_badge_api_types.py — a Python script that reads openapi.json and generates BadgeAPI_types.h containing only C-compatible structs for the firmware-facing endpoints (getBadgeInfo, getQRBitmap, createBoop, getBoopStatus, getBoopPartner, sendPing, getPings). Refactor the existing BadgeAPI.h/.cpp in firmware/Firmware-0308-modular/ so all public functions return typed result structs (never raw Arduino String or JSON), and move the inline HTTPClient poll call in the main .ino into BadgeAPI. Auth headers are not yet implemented — leave stubs. The existing firmware behavior must not change."

## User Scenarios & Testing *(mandatory)*

### User Story 1 — Developer regenerates API types after a backend change (Priority: P1)

A firmware developer learns the staging backend was updated with a new field or endpoint. They run `scripts/gen_badge_api_types.py` and receive an updated `BadgeAPI_types.h`. The header is self-consistent with `specs/openapi.json` — no manual struct editing required.

**Why this priority**: This is the foundational contract mechanism. All other stories depend on the type definitions this script produces.

**Independent Test**: Run `gen_badge_api_types.py` against a known `specs/openapi.json` and verify `BadgeAPI_types.h` contains a struct definition for each of the seven firmware-facing endpoints. The firmware must compile with the generated header.

**Acceptance Scenarios**:

1. **Given** `specs/openapi.json` exists in the repo, **When** the developer runs `gen_badge_api_types.py`, **Then** `BadgeAPI_types.h` is written with C-compatible structs for all seven firmware-facing endpoints (getBadgeInfo, getQRBitmap, createBoop, getBoopStatus, getBoopPartner, sendPing, getPings).
2. **Given** a backend response field is renamed, **When** the developer updates `openapi.json` and re-runs the script, **Then** `BadgeAPI_types.h` reflects the rename and the firmware fails to compile until call sites are updated — the breaking change is caught at compile time rather than runtime.
3. **Given** the script is run with a missing or malformed `openapi.json`, **When** the script encounters the error, **Then** it exits with a non-zero code and a human-readable error message.

---

### User Story 2 — Firmware caller receives typed data with no post-parsing (Priority: P2)

A firmware module calls any BadgeAPI function and uses the returned struct fields directly — no JSON parsing, no manual string-to-enum conversion in the calling code. All returned string values are char arrays bounded to a known maximum length; all status values are enums or integer constants.

**Why this priority**: Eliminates a class of bugs where callers silently ignore parse errors or mishandle dynamic string types in interrupt-context code.

**Independent Test**: Read `BadgeAPI.h` and verify no public function signature returns a dynamic string type or a raw JSON value. Inspect all result struct fields: each is a primitive, a fixed-size char array, a uint8_t buffer with explicit length, or an enum.

**Acceptance Scenarios**:

1. **Given** a successful `getBadgeInfo` call, **When** the caller accesses the name field, **Then** it is a null-terminated char array of bounded length — not a dynamic string or JSON value.
2. **Given** a `createBoop` call that returns a pending status, **When** the caller checks the status, **Then** it compares against a typed constant or enum, not a string literal — typos in status strings are a compile error, not a silent bug.
3. **Given** a successful `getBoopStatus` returning a confirmed status, **When** the caller reads the pairing ID field, **Then** it receives a numeric value matching the backend's integer pairing ID, not a string that requires further parsing.
4. **Given** any BadgeAPI function returns `ok == false`, **When** the caller inspects the struct, **Then** all string/buffer fields are empty or zero and all numeric fields are zero — no undefined values.

---

### User Story 3 — Badge activation polling is encapsulated in BadgeAPI (Priority: P3)

The badge activation flow (waiting for the backend to register the badge after QR scan) calls a BadgeAPI function to probe badge existence rather than constructing an HTTP client inline. The calling code does not import or use any HTTP library directly.

**Why this priority**: Removes the only raw HTTP call outside of BadgeAPI, completing the encapsulation. Lower priority than types because it is a pure refactor — no new user-visible capability is added.

**Independent Test**: Search `BadgePairing.cpp` and `Firmware-0308-modular.ino` for direct HTTP client usage — none should remain. The badge activation sequence compiles and behaves identically on hardware.

**Acceptance Scenarios**:

1. **Given** the badge is in the unpaired state with WiFi connected, **When** the activation polling loop runs, **Then** it calls a BadgeAPI function to probe badge existence rather than directly constructing an HTTP client.
2. **Given** the probe returns a success status, **When** the activation flow proceeds, **Then** it calls `BadgeAPI::fetchBadgeXBM` as before — the trigger condition for the full info fetch is unchanged.
3. **Given** the probe returns a network error or non-success status, **When** the caller receives the result, **Then** the error is reported to the display identically to the prior behavior (e.g. "conn err" or "HTTP N").

---

### User Story 4 — Firmware can call new endpoints: getBoopPartner, sendPing, getPings (Priority: P4)

Three endpoints present in the backend but not yet in the firmware API surface are added as callable BadgeAPI functions. Each has a typed result struct derived from the OpenAPI spec. The firmware compiles with these symbols available. No higher-level UI or workflow code is required to use them in this feature.

**Why this priority**: These are new capability stubs needed for future badge interaction features. They must exist and compile but need not be wired to any existing firmware flow.

**Independent Test**: Build the firmware after adding the new function signatures and confirm the build succeeds. The function signatures in `BadgeAPI.h` match the typed structs in `BadgeAPI_types.h`.

**Acceptance Scenarios**:

1. **Given** a confirmed pairing with a numeric pairing ID, **When** firmware calls `getBoopPartner`, **Then** a typed result struct with partner name fields is returned (or `ok == false` on error).
2. **Given** a confirmed pairing exists, **When** firmware calls `sendPing` with a source badge UUID, target ticket UUID, activity type, and payload, **Then** a typed ping result struct is returned with `ok == true` on success.
3. **Given** an active pairing, **When** firmware calls `getPings` with the required query parameters, **Then** a typed list result struct is returned with a fixed-capacity array of ping records and a next-page cursor field.
4. **Given** auth headers are not yet implemented, **When** any of the seven BadgeAPI functions makes an HTTP request, **Then** the request is sent without auth headers — a stub hook for future auth injection is present but inactive.

---

### Edge Cases

- What happens when `gen_badge_api_types.py` encounters a backend endpoint outside the seven firmware-facing ones? The script must skip it with a warning to stdout — it must not generate unintended types.
- What happens when a char array field in the generated struct is shorter than the value the backend returns? The BadgeAPI implementation must truncate safely with null-termination within bounds — no buffer overrun.
- What happens when `getPings` returns more records than the firmware's fixed-capacity array can hold? The result struct must report the capped count; excess records are discarded and the cursor allows the caller to fetch more.
- What if the firmware is built before `BadgeAPI_types.h` is regenerated after an `openapi.json` update? The build must fail with a clear compile error — no silent use of stale types.
- What happens when the pings `data` field (an arbitrary JSON object) is larger than the fixed buffer in the result struct? The buffer must be filled up to capacity and null-terminated; the caller is responsible for detecting truncation via the provided byte count.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: `specs/openapi.json` MUST be committed to the repository as a versioned artifact representing the staging backend's API contract.
- **FR-002**: `scripts/gen_badge_api_types.py` MUST read `specs/openapi.json` and produce `firmware/Firmware-0308-modular/BadgeAPI_types.h` containing C-compatible struct definitions for each of the seven firmware-facing endpoints: getBadgeInfo, getQRBitmap, createBoop, getBoopStatus, getBoopPartner, sendPing, getPings.
- **FR-003**: Generated structs MUST use only C-compatible types: fixed-size char arrays, `uint8_t`, `int`, `bool`, enums, and nested structs. No dynamic string types or template types are permitted.
- **FR-004**: All public functions in the BadgeAPI module MUST return typed result structs. No public function MAY return a dynamic string type, raw JSON text, or an untyped pointer.
- **FR-005**: Status fields that currently use dynamic string values (e.g. "pending", "confirmed", "not_requested") MUST be represented as enums or integer constants in the result structs.
- **FR-006**: String fields in result structs (e.g. name, title, workflow ID) MUST be fixed-size char arrays. Sizes MUST be derived from the OpenAPI spec's `maxLength` constraints where present, or from a documented project default where `maxLength` is absent.
- **FR-007**: The inline HTTP client block inside the badge activation polling loop in `BadgePairing.cpp` MUST be replaced with a call to a new BadgeAPI function that probes badge existence.
- **FR-008**: The new badge-existence probe function MUST return a typed result struct with at minimum an `ok` flag and an `httpCode` field, consistent with the base result pattern used by all other BadgeAPI functions.
- **FR-009**: `getBoopPartner`, `sendPing`, and `getPings` MUST be added as public BadgeAPI functions with typed result structs matching the backend's OpenAPI schemas for those endpoints.
- **FR-010**: A clearly marked stub for future auth header injection MUST be present in the HTTP transport layer. The stub MUST NOT send any auth headers in this feature.
- **FR-011**: The firmware MUST compile successfully and produce a valid binary after all changes. No existing behavior visible to the badge user MAY change.
- **FR-012**: `gen_badge_api_types.py` MUST NOT generate structs for backend endpoints outside the seven firmware-facing ones. Unrecognized endpoints must be skipped with a warning to stdout.
- **FR-013**: `gen_badge_api_types.py` output MUST be deterministic: given the same `openapi.json`, repeated runs produce byte-identical output.

### Key Entities

- **OpenAPI spec** (`specs/openapi.json`): Versioned snapshot of the backend's API contract. Source of truth for field names, types, and constraints that flow into `BadgeAPI_types.h`.
- **BadgeAPI_types.h**: A generated C header containing only the firmware-facing struct definitions. Consumed by `BadgeAPI.h` and all firmware modules that include it. Must never be edited by hand.
- **BadgeAPI module** (`BadgeAPI.h` / `BadgeAPI.cpp`): The firmware's sole HTTP abstraction layer. All network calls from firmware modules go through this module. Owns the HTTP transport and the auth stub.
- **Result struct**: A C struct returned by each BadgeAPI function. Contains at minimum `ok` (bool) and `httpCode` (int), plus endpoint-specific typed fields. On failure, all fields beyond `ok` and `httpCode` are zero or empty.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The firmware builds without errors after all changes, verified by a successful `./build.sh` run producing a valid `.bin`.
- **SC-002**: Zero occurrences of direct HTTP client usage in `BadgePairing.cpp` or `Firmware-0308-modular.ino` after the refactor, verified by a source search.
- **SC-003**: Zero occurrences of dynamic string types in any public function signature in `BadgeAPI.h`, verified by a source search.
- **SC-004**: Running `gen_badge_api_types.py` with the committed `openapi.json` produces a `BadgeAPI_types.h` that matches the committed file byte-for-byte (deterministic output).
- **SC-005**: All seven firmware-facing endpoint names have a corresponding public function in the BadgeAPI module, verified by reading `BadgeAPI.h`.
- **SC-006**: Badge startup, QR display, boop pairing, and status-confirmed flows behave identically to pre-refactor behavior on hardware — no display or IR regression.

## Assumptions

- The staging backend is accessible during development to fetch `openapi.json`. If not, the spec can be hand-authored from the existing route definitions already read.
- `maxLength` constraints may not be present for all string fields in the backend's OpenAPI spec. Where absent, a project-default maximum (e.g. 64 or 128 bytes) will be used and documented in a comment in the generated header.
- The pings `data` payload is an arbitrary JSON object and cannot be fully typed in a C struct. The generated struct for `sendPing` and `getPings` will represent `data` as a fixed-size char array of raw JSON text, bounded by a project constant.
- `getBoopPartner`, `sendPing`, and `getPings` are new additions to the firmware API surface. No firmware feature code calls them yet in this feature — they exist as callable stubs.
- The "inline HTTPClient poll" referred to in the feature description is the raw HTTP client block in `BadgePairing.cpp::osUnpairedFlow()`, not a call in `Firmware-0308-modular.ino` itself.
