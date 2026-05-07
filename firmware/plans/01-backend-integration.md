# 01 — Backend integration & local test server

Goal: bring up the `replayBadgeBackend` locally, verify the
firmware's current API contract end-to-end, and leave a
repeatable test recipe behind so future work (`02`–`04`) can
validate against a real server instead of guessing.

## Context — what exists

### Repos

- Firmware: `/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/`
- Backend: `/Users/kevinsanto/Documents/GitHub/replayBadgeBackend/`
  - `app.py`, `routes/pings.py`, `routes/boops/`, `db.py`,
    `temporal_client.py`, `workflows/`, `specs/`, `tests/`.
- Shared specs:
  `/Users/kevinsanto/Documents/GitHub/Temporal-Badge/specs/openapi.json`
  (authoritative source for `BADGE_*_MAX` constants generated into
  [`../src/BadgeAPI_types.h`](../src/BadgeAPI_types.h)).

### Firmware API surface (what we call today)

All in [`../src/BadgeAPI.cpp`](../src/BadgeAPI.cpp) /
[`../src/BadgeAPI.h`](../src/BadgeAPI.h):

| Endpoint                       | Caller                                     |
|--------------------------------|--------------------------------------------|
| `GET  /api/v1/badge/{uid}/info`| `BadgeAPI::fetchBadgeXBM`, `probeBadgeExistence` |
| `GET  /api/v1/lookup-attendee/{uuid}` | `BadgeAPI::lookupAttendee`          |
| `POST /api/v1/boops`           | `BadgeAPI::createBoop` (HMAC)              |
| `GET  /api/v1/boops?badge_uuid=` | `BadgeAPI::getBoops`                     |
| `GET  /api/v1/boops/{id}/partner?badge_uuid=` | `BadgeAPI::getBoopPartner` |
| `POST /api/v1/pings`           | `BadgeAPI::sendPing` (HMAC)                |
| `GET  /api/v1/pings`           | `BadgeAPI::getPings` (HMAC)                |

HMAC details: [`../src/BadgeAPI.cpp`](../src/BadgeAPI.cpp) L17–L57
(`attachHMACHeaders`). Reads `hmac_secret` from NVS namespace
`badge_identity`; skips header attachment when absent ("not
enrolled"). Computes HMAC-SHA256 over
`<badge_uuid><unix_timestamp>` via eFuse `HMAC_KEY4`.

### Firmware send/receive shape for notifications

- **Sender** — [`../src/GUI.cpp`](../src/GUI.cpp) `SendBoopBackground::sendViaApi`:
  POSTs `/api/v1/pings` with
  `{"title": "...", "body": "...", "msg_id": "<8-hex-token>"}`,
  `activity_type = "notification"`, `target_ticket_uuid = peerTicket`.
- **Receiver** — [`../src/WiFiService.cpp`](../src/WiFiService.cpp)
  `WiFiService::pollNotifications`: GETs `/api/v1/pings` every 30 s
  by default, filter `activity_type=notification`. Resolves the
  `source_ticket_uuid` to a peer badge UID via `/boops.json`
  (`resolveSenderFromBoops`). Reads `data.msg_id` as the cross-
  transport dedup key.

### Server endpoints relevant to the above

From `replayBadgeBackend/routes/pings.py`: confirm presence of:

- `POST /api/v1/pings` — HMAC-authenticated, accepts
  `{source_badge_uuid, target_ticket_uuid, activity_type, data}`,
  returns `{id}`.
- `GET  /api/v1/pings` with query params
  `requester_badge_uuid`, `target`, `type`, `limit`, `before_ts`,
  `before_id`. Returns `{events: [...], ...}`.

Verify these match firmware's expectations exactly — if the
backend has already drifted, the plan is to change firmware,
NOT the server (server is the authoritative contract).

## Goals

1. Run `replayBadgeBackend` locally, pointing both badges at it.
2. Confirm the `/api/v1/pings` payload shape matches firmware's
   current serializer / parser.
3. Exercise the full sender→receiver loop for a single notification
   end-to-end with the server in the path (no IR), proving the
   parity-send scheme (`02-ir-multiframe-parity.md`) will actually
   have a working API rail to lean on.
4. Leave a 5-minute test recipe in this file or in
   `replayBadgeBackend/curl-test.md` that future agents can follow
   to smoke-test API changes without having to rediscover the
   enrollment + HMAC flow.

## Research to do first

- `replayBadgeBackend/README.md` — project overview (already
  roughly skimmed; confirms boop + ping primitives, 2000 badges
  architecture).
- `replayBadgeBackend/pyproject.toml` — Python version, deps.
- `replayBadgeBackend/curl-test.md` — existing smoke-test recipes,
  if any.
- `replayBadgeBackend/routes/pings.py` — the actual HTTP shape
  vs. what firmware sends.
- `replayBadgeBackend/temporal_client.py` — whether a Temporal
  worker is required to be running for pings (likely yes — boops
  need it; pings MAY or may not depending on the implementation).
- `replayBadgeBackend/scripts/` — local dev helpers.
- `../specs/openapi.json` — authoritative shape contract. Diff
  against `routes/pings.py` if they disagree.

Skim order: README → pyproject → routes/pings.py → curl-test.md →
scripts/.

## Design sketch

### A. Get the server running

Prefer `uv` (lockfile is present: `uv.lock`). Likely sequence:

```bash
cd /Users/kevinsanto/Documents/GitHub/replayBadgeBackend
uv sync                      # install deps
# dev config — likely an .env file; check config.py for ENV vars
uv run python app.py         # or whatever entrypoint README specifies
```

If Temporal is required for boops (it is per the README's
boop-workflow description): check for a `docker-compose.yml` or
local dev recipe — there's a `railway.worker.toml` and
`railway.web.toml` suggesting a web process + a worker process.

### B. Point the badge at it

Badge WiFi credentials and the backend server URL are now build-time
settings. Copy `../wifi.local.env.example` to `../wifi.local.env` and set:

```
BADGE_WIFI_SSID=<network>
BADGE_WIFI_PASS=<password>
BADGE_SERVER_URL=http://<local-dev-ip>:<port>
```

Then rebuild and reflash the badge. Runtime `/settings.txt` still owns the
non-secret endpoint paths, but changing the network or backend host requires a
new firmware build. Both badges need to be on the same WiFi the backend is
reachable from.

### C. End-to-end verification

1. Enroll the badge via the normal flow (HMAC secret writes to
   NVS `badge_identity/hmac_secret`).
2. From badge A (HMAC-authenticated):
   `POST /api/v1/pings` with
   `{source_badge_uuid:A, target_ticket_uuid:B_ticket,
     activity_type:"notification", data:{title, body, msg_id}}`.
3. From badge B's WiFiService poll: `GET /api/v1/pings?...` returns
   the record. Verify `data.msg_id` round-trips and is usable as
   the store's dedup key.

### D. Deliverables

- A `replayBadgeBackend` README blurb or shell script that brings
  the server up with the minimum services required (web + Temporal
  worker if needed).
- A tested curl recipe: enroll → sendPing → getPings pair.
- A note in `../AGENTS.md` pointing future agents at that recipe
  so tests don't have to hunt again.

## Implementation checklist

- [ ] Read `replayBadgeBackend/README.md`, `pyproject.toml`,
      `routes/pings.py`, `curl-test.md` (if present), and
      `../specs/openapi.json`.
- [ ] Diff `POST /api/v1/pings` request shape (backend) vs what
      `BadgeAPI::sendPing` in [`../src/BadgeAPI.cpp`](../src/BadgeAPI.cpp)
      L395–L444 actually sends. Flag discrepancies.
- [ ] Diff `GET /api/v1/pings` response shape vs what
      `BadgeAPI::getPings` in [`../src/BadgeAPI.cpp`](../src/BadgeAPI.cpp)
      L446–L527 parses. Flag discrepancies.
- [ ] Stand up local backend. Document the exact commands (Python
      version, uv commands, env setup) inside this file as a
      "Tested setup" section.
- [ ] Configure one badge to point at local backend. Send a ping
      from its `SendBoopBackground`. Watch `/tmp/serial_<badge>.log`
      for `[SendBoop] API sent http=200` and server logs for the
      POST hitting `routes/pings.py`.
- [ ] Watch the OTHER badge's `WiFiService::pollNotifications` pick
      the ping up on its next 30 s tick. Verify it lands in a
      thread keyed on the sender's resolved badge UID
      (`resolveSenderFromBoops` in
      [`../src/WiFiService.cpp`](../src/WiFiService.cpp)).
- [ ] If `source_ticket_uuid` → badge UID resolution fails (peer
      not in local `/boops.json` because no prior boop), log the
      gap and decide: is the "thr:server" fallback good enough, or
      does the peer lookup path need a `GET /api/v1/lookup-attendee`
      fallback?

## Open questions for the user at start

- **API-side dedup**: should the server also dedup on `data.msg_id`
  so a re-delivered API ping produces only one database row?
  Currently firmware dedups at receive — server-side dedup is a
  nice-to-have but not required.
- **Enrollment for test badges**: do the current badges already
  have HMAC secrets provisioned for the local backend, or does the
  test recipe need to include enrollment?

## Testing approach

- Two badges on the same WiFi as dev laptop.
- `tail -f /tmp/serial_charlie.log /tmp/serial_delta.log`.
- Server: `tail -f` its log; filter for POST /pings.
- Expect within ~30 s: `[SendBoop] API sent` on sender AND
  `[BadgeNotifs] new incoming from <uid> title=...` on receiver.

## Conventions to preserve

- Do NOT change HMAC key (HMAC_KEY4 is baked into eFuse on shipping
  hardware).
- Do NOT change ArduinoJson capacities without checking the json
  size guard inside `BadgeAPI.cpp`'s `StaticJsonDocument<N>` usage.
- Server URL changes go through `/settings.txt` + `ConfigWatcher`
  hot-reload; no firmware reflash needed to switch prod↔dev.
