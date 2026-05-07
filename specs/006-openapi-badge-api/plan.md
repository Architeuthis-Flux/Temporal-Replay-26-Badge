# Implementation Plan: OpenAPI Badge API Contract Library

**Branch**: `006-openapi-badge-api` | **Date**: 2026-03-13 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/006-openapi-badge-api/spec.md`

**Note**: This file was filled in by the `/speckit.plan` command.

## Summary

Refactor `BadgeAPI` from an ad-hoc Arduino module into a spec-driven contract library.
Deliverables: (1) `specs/openapi.json` hand-authored from backend route inspection,
(2) `scripts/gen_badge_api_types.py` that generates `BadgeAPI_types.h` from it,
(3) refactored `BadgeAPI.h/.cpp` with typed result structs (no `String` / raw JSON in
public signatures), (4) inline `HTTPClient` poll in `BadgePairing.cpp::osUnpairedFlow`
replaced by a new `BadgeAPI` function, (5) three new stub functions: `getBoopPartner`,
`sendPing`, `getPings`. Existing firmware behavior is unchanged.

## Technical Context

**Language/Version**: Arduino C++ (ESP32 Arduino core 3.x) for firmware; Python 3.x
(stdlib only) for generator script
**Primary Dependencies**: `ArduinoJson` 6.x, `HTTPClient`, `WiFi` (firmware); no
third-party packages for generator
**Storage**: N/A — generator is stateless; output `BadgeAPI_types.h` is committed to repo
**Testing**: Manual build verification (`./build.sh`); determinism check (`diff` of
generated header on repeated runs)
**Target Platform**: ESP32-S3-MINI-1 XIAO; 8 KB task stacks on Core 0 and Core 1
**Project Type**: Contract library (BadgeAPI module) + off-device codegen script
**Performance Goals**: All result structs stack-allocated; no dynamic allocation in public
result types
**Constraints**: Structs must fit in 8 KB stack; char arrays bounded by constants; no
`std::string`, no `Arduino::String` in public API
**Scale/Scope**: 7 firmware-facing endpoints; 1 codegen script (~150 LoC); refactor ~300
LoC across BadgeAPI.h/.cpp and BadgePairing.cpp

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Arduino C++ First | ✅ PASS | Generator is off-device Python tooling; all firmware remains Arduino C++ |
| II. Firmware-0308 Behavioral Source | ✅ PASS | FR-011: no existing behavior may change; spec explicitly requires identical display/IR behavior |
| III. Credentials-at-Build | ✅ PASS | No new secrets; `openapi.json` contains no credentials; generator reads only the spec file |
| IV. Backend Contract Compliance | ✅ PASS | This feature *implements* the contract layer; `openapi.json` is the versioned binding |
| V. Reproducible Build | ✅ PASS | `BadgeAPI_types.h` output is deterministic (FR-013); committed to repo so build does not need generator at build time |
| VI. Hardware Safety | ✅ PASS | No GPIO, eFuse, or peripheral initialization changes |
| VII. API Contract Library | ✅ PASS | This feature is the direct implementation of Principle VII |

No violations — complexity tracking table not required.

## Project Structure

### Documentation (this feature)

```text
specs/006-openapi-badge-api/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output — C struct definitions
├── quickstart.md        # Phase 1 output — generator workflow
├── contracts/
│   └── firmware-endpoints.md   # Phase 1 output — 7-endpoint subset contract
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
specs/
└── openapi.json                       # NEW — versioned backend API contract artifact

scripts/
└── gen_badge_api_types.py             # NEW — reads openapi.json, writes BadgeAPI_types.h

firmware/Firmware-0308-modular/
├── BadgeAPI_types.h                   # NEW (generated) — C-compatible structs + enums
├── BadgeAPI.h                         # MODIFIED — updated public signatures, no String returns
├── BadgeAPI.cpp                       # MODIFIED — typed structs, 3 new functions, auth stub
└── BadgePairing.cpp                   # MODIFIED — inline HTTPClient poll → BadgeAPI call
```

**Structure Decision**: Single-project (Arduino sketch directory). The generator script
lives in `scripts/` at repo root, consistent with existing tooling (`flash.py`, etc.).
The OpenAPI spec lives in `specs/` alongside feature specs as a versioned artifact.
