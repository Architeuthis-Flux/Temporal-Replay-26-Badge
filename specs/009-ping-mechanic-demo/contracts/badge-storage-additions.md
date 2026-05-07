# Contract: BadgeStorage Additions (spec-009)

## Overview

NVS contact storage is **not needed** — the server is authoritative via `GET /api/v1/boops`. The only addition to `BadgeStorage` is persisting the badge's own `ticket_uuid` at enrollment.

---

## New NVS Key

| Namespace | Key | Type | Description |
|-----------|-----|------|-------------|
| `badge_identity` | `ticket_uuid` | string (37) | Badge's own ticket UUID |

**When written**: At enrollment, when `POST /api/v1/link-user-to-badge` returns `ticket_uuid` in the response. Should be written alongside `hmac_secret` and `enrolled` in the existing enrollment flow.

**Fallback**: If absent (badge enrolled before spec-009), call `GET /api/v1/lookup-attendee/<my_badge_uuid>` once at boot and cache the result in NVS.

---

## New Functions (namespace `BadgeStorage`)

### `saveMyTicketUUID(const char* ticket_uuid)`
Writes `ticket_uuid` to NVS namespace `badge_identity`.

### `loadMyTicketUUID(char* out, size_t maxLen)` → bool
Reads `ticket_uuid` from NVS. Returns `false` if not yet stored.

---

## Eliminated

~~`badge_contacts` NVS namespace~~ — not needed.
~~`ContactEntry` struct~~ — not needed.
~~`saveContact()`, `loadContacts()`, `contactCount()`~~ — not needed.
~~`boop_cnt` key~~ — not needed; army size from server.
~~`BadgePairing.cpp` contact write call~~ — not needed.
