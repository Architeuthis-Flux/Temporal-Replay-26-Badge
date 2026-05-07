# Research: OpenAPI Badge API Contract Library

## Source of Truth

The backend is a Quart/Python application in `registrationScanner/`. There is no
pre-existing `openapi.json`. The spec must be hand-authored from route inspection.
All routes were read directly from:
- `routes/registration.py` — badge info, QR, registration
- `routes/boops/__init__.py` — boop record CRUD
- `routes/boops/pair.py` — consent lifecycle, partner info
- `routes/pings.py` — directed async interactions

---

## Decision 1 — How to obtain openapi.json

**Decision**: Hand-author `specs/openapi.json` from backend route + Pydantic model inspection.

**Rationale**: The staging backend may not be accessible during offline firmware development.
The backend already uses Quart-Schema with Pydantic models, so all field names, types, and
optionality are readable directly from source. A hand-authored spec from source is more
reliable than a fetch that may return a schema with generated descriptions or unstable URLs.

**Alternatives considered**:
- Fetch from staging via `GET /docs/openapi.json` — rejected: requires live network access;
  spec would differ between environments; introduces coupling between firmware CI and backend availability.
- Use quart-schema's auto-generated spec — rejected: same network dependency issue; also
  includes admin/registration endpoints that are not firmware-facing.

---

## Decision 2 — char array sizes for string fields

**Decision**: Use the following project-default sizes where `maxLength` is absent from the spec.

| Field category | Default char array size | Rationale |
|----------------|------------------------|-----------|
| Person name (name, title, company) | 64 bytes | Business cards; typical conferee field length |
| attendee_type string | 16 bytes | Enum-like; longest value is "Attendee" (8) |
| UUID string | 37 bytes | UUID v4 canonical form: 36 chars + null terminator |
| workflow_id string | 96 bytes | `badge-pairing-<uuid1>-<uuid2>` = ~82 chars + margin |
| partner_name, partner_title, partner_company | 64 bytes | Same as name/title/company |
| partner_attendee_type | 16 bytes | Same as attendee_type |
| Ping UUID (id) | 37 bytes | UUID v4 |
| Ping activity_type | 32 bytes | App-defined; "room_checkin" is 12 chars; 32 gives headroom |
| Ping data (raw JSON char buffer) | 256 bytes | Arbitrary JSON object; truncation is documented |
| Ping cursor (next_cursor) | 128 bytes | Base64-encoded JSON cursor; bounded by before_ts + before_id |
| Ping created_at / updated_at | 32 bytes | ISO 8601 timestamp: "2026-03-13T00:00:00.000Z" = 24 chars |

**Why not dynamic strings**: ESP32-S3 tasks run on 8 KB stacks. Arduino `String` heap
allocation in interrupt-adjacent code causes fragmentation. Stack-allocated char arrays
are safe for Core 0 (IR task) and Core 1 (display task) callers alike.

---

## Decision 3 — Status fields as enums vs integer constants

**Decision**: Represent `status` fields as enums defined in `BadgeAPI_types.h`.

```c
typedef enum {
    BOOP_STATUS_UNKNOWN       = 0,
    BOOP_STATUS_PENDING       = 1,
    BOOP_STATUS_CONFIRMED     = 2,
    BOOP_STATUS_NOT_REQUESTED = 3
} BadgeBoopStatus;
```

**Rationale**: The spec (FR-005) requires status values to be enums or integer constants.
Enums provide named constants that cause compile errors on typos, satisfy FR-005, and
occupy 4 bytes (same as `int`) on ARM Cortex-M. Existing callers compare `result.status`
against string literals — those comparisons become enum comparisons after refactor.

**Alternatives considered**:
- `#define` integer constants — rejected: no type safety; enums preferred for readability
  and debugger support.
- Keep `String status` — rejected: violates FR-004 and FR-005 directly.

---

## Decision 4 — Naming convention for new vs existing functions

**Decision**: Keep `BadgeAPI::` namespace. Rename only the functions that are being
redesigned for the new spec. The spec (constitution Principle VII) uses `BadgeAPI_` prefix
for the stable C ABI but the `.h` may expose them as a C++ namespace or extern "C" at
the firmware developer's discretion.

**Functions being renamed / type-changed**:
| Old name | New name | Change |
|----------|----------|--------|
| `getBadgeInfo` | `getBadgeInfo` | Return type: `BadgeInfoResult` with char arrays instead of `String` |
| `fetchQR` | `getQRBitmap` | Renamed to match spec; return type unchanged (`FetchQRResult` is a buffer) |
| `createBoop` | `createBoop` | Return type: `BoopResult` with enum status and char arrays |
| `getBoopStatus` | `getBoopStatus` | Return type: `BoopStatusResult` with enum status, pairing_id int |
| `cancelBoop` | `cancelBoop` | No change needed — `APIResult` is already typed |
| `fetchBadgeXBM` | `fetchBadgeXBM` | No public API change — internal to pairing flow |
| NEW | `getBoopPartner` | New function |
| NEW | `sendPing` | New function |
| NEW | `getPings` | New function |
| NEW | `probeBadgeExistence` | Replaces inline HTTPClient in BadgePairing.cpp |

**Rationale**: The spec (FR-009) requires `getBoopPartner`, `sendPing`, and `getPings` as
public functions. Renaming `fetchQR` → `getQRBitmap` aligns with the spec's naming table.
Constitution Principle VII uses `BadgeAPI_` prefix for the future C ABI — that binding
layer is out of scope for this feature; the C++ namespace form is sufficient for now.

---

## Decision 5 — getPings result struct capacity

**Decision**: `GetPingsResult` holds a fixed-capacity array of `PingRecord`:
```c
#define BADGE_PINGS_MAX_RECORDS 8

typedef struct {
    bool          ok;
    int           httpCode;
    PingRecord    records[BADGE_PINGS_MAX_RECORDS];
    int           count;          // actual records returned (≤ BADGE_PINGS_MAX_RECORDS)
    char          next_cursor[128]; // empty string if no further pages
} GetPingsResult;
```

**Rationale**: Dynamic arrays are unsafe on ESP32 stacks. 8 records × ~512 bytes per record
= ~4 KB, which fits in the 8 KB stack. Excess records from the backend are discarded; the
caller uses `next_cursor` to fetch more. The spec (edge case §3) explicitly documents this.

**Alternatives considered**:
- Heap-allocated array — rejected: caller must free; fragmentation risk on Core 0.
- Capacity of 4 — rejected: too small for typical use; 8 records is a reasonable page.

---

## Decision 6 — Auth stub placement

**Decision**: The auth stub lives in `_request()` in `BadgeAPI.cpp` as a clearly-marked
`// AUTH STUB: inject X-Badge-ID, X-Timestamp, X-Signature headers here` comment block.
The stub does NOT send any headers. The `_request()` signature accepts a `bool needsAuth`
parameter that callers set; when `false` (default) nothing happens; when `true` the stub
fires (currently a no-op).

**Rationale**: FR-010 requires a stub for future auth injection. Making `needsAuth` a
parameter to the transport function is the minimal surface that (a) causes no behavior
change and (b) gives the auth implementation a clear, single place to add code later.

---

## Decision 7 — openapi.json scope

**Decision**: `specs/openapi.json` covers only the 7 firmware-facing endpoints. Non-firmware
endpoints (`/api/v1/lookup-attendee`, `/api/v1/link-user-to-badge`, `/api/v1/unpair`,
`/api/v1/search-attendees`, `/api/v1/boops/{id}` GET/DELETE, `/api/v1/pings/{id}` PATCH)
are excluded.

**Rationale**: FR-012 states the generator MUST NOT generate structs for non-firmware
endpoints. Limiting `openapi.json` to the firmware subset keeps the file minimal and
prevents accidental generation of unintended types. The generator enforces this independently
via its `FIRMWARE_ENDPOINTS` allowlist, but the spec file being scoped correctly is a
belt-and-suspenders measure.

**Alternatives considered**:
- Include full backend spec, filter at generator — rejected: full spec would expose auth
  tokens, admin routes, and registration flows that firmware developers should not see.

---

## Resolved: Backend endpoint summary for firmware

| Function | Method | Path | Auth | Response schemas |
|----------|--------|------|------|-----------------|
| `getQRBitmap` | GET | `/api/v1/badge/{id}/qr.xbm` | No | `image/x-xbitmap` binary |
| `getBadgeInfo` + `fetchBadgeXBM` | GET | `/api/v1/badge/{id}/info` | No | `BadgeInfoResponse` |
| `createBoop` | POST | `/api/v1/boops` | No (HMAC stub) | `PairingPendingResponse` (202), `PairingConfirmedResponse` (200) |
| `getBoopStatus` | GET | `/api/v1/boops/status/{workflow_id}` | No | `PairingStatusResponse` |
| `cancelBoop` | DELETE | `/api/v1/boops/pending` | No | `OkResponse` |
| `getBoopPartner` | GET | `/api/v1/boops/{id}/partner?badge_uuid=` | No | `PartnerInfoResponse` |
| `sendPing` | POST | `/api/v1/pings` | No (HMAC stub) | `PingRecord` |
| `getPings` | GET | `/api/v1/pings` | No (HMAC stub) | `PingListResponse` |
| `probeBadgeExistence` | GET | `/api/v1/badge/{id}/info` | No | 200/404 only; struct fields ignored |

Note: `cancelBoop` and `fetchBadgeXBM` are existing functions not in the 7 named in spec
but retained as internal/existing API surface. `probeBadgeExistence` is a new thin wrapper.
