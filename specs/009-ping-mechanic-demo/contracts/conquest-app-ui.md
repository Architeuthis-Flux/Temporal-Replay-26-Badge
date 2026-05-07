# Contract: Conquest App UI (spec-009)

## Overview

A MicroPython app (`conquest.py`) installed in `firmware/Firmware-0308-modular/apps/`. Launched from the Apps menu. Showcases the Python-layer ping mechanic.

---

## Screen States

```
EMPTY (no contacts) → exits to Apps menu

LIST → SENDING → WAITING → RESULT
  ↑      ↓ (BTN_RIGHT)      ↓
  └──────────────────────────┘ (BTN_RIGHT or auto-return)

Passive: INCOMING (overlay on LIST — auto-respond to challenge)
```

---

## Screen: LIST

**Renders:**
```
Conquest
Army: 7
> Alice (Acme)
  Bob (Beta)
  Carol (Corp)
```
- Title `"Conquest"` + army size line
- Scrollable contact list; `>` prefix on selected entry
- Name + company per entry

**Input:**
- `BTN_UP` / `BTN_DOWN`: move cursor; wraps
- `BTN_DOWN` (confirm selected): → SENDING
- `BTN_RIGHT`: exit app (`badge.exit()`)

**Passive poll (every 3s while on LIST screen):**
- Calls `GET /api/v1/pings?type=conquest_challenge&...`
- If incoming challenge found: auto-respond with `conquest_response` ping, show brief `"Challenge received!"` flash, return to LIST
- Ensures badge can act as defender even when not actively challenging

---

## Screen: EMPTY

**Renders:**
```
Conquest
No contacts.
Boop someone
first!
```

**Input:**
- `BTN_RIGHT`: exit app

---

## Screen: SENDING

**Renders:**
```
Conquest
Challenging
Alice...
```
- Sends `POST /api/v1/pings` with `activity_type: "conquest_challenge"` + `data: {"army_size": N}`
- Transitions: → WAITING on success, → ERROR on failure (brief display + back to LIST)

---

## Screen: WAITING

**Renders:**
```
Conquest
Waiting for
Alice...
[20s timeout]
```
- Polls `GET /api/v1/pings?type=conquest_response` every 3 seconds
- Timeout: 20 seconds from challenge send time
- Shows elapsed seconds or a progress indicator (e.g., dots cycling)

**Input:**
- `BTN_RIGHT`: cancel wait → back to LIST

**On response received:** → RESULT
**On timeout:** → TIMEOUT screen (brief "No response" + back to LIST)

---

## Screen: RESULT

**Renders (victory):**
```
Conquest
VICTORY!
You: 7  They: 4
```

**Renders (defeat):**
```
Conquest
Defeated.
You: 3  They: 9
```

**Renders (draw):**
```
Conquest
DRAW!
Both: 5
```

- Displays for 4 seconds, then auto-returns to LIST
- `BTN_RIGHT`: immediate exit to LIST

---

## Battle Resolution Logic

Challenger holds `my_army` (from `len(boops)` — count of active pairings already loaded at app start).
Response ping `data` contains `{"army_size": N}`.

```python
if my_army > their_army:
    result = "VICTORY"
elif my_army < their_army:
    result = "DEFEAT"
else:
    result = "DRAW"
```

---

## Auto-Response Logic (defender side)

When a `conquest_challenge` ping arrives:

```python
# Parse challenger's army size from ping data
# Read my own army size: len(boops) — already loaded at app start
# POST response ping:
#   activity_type: "conquest_response"
#   target: ping.source_ticket_uuid  # reply to challenger
#   data: {"army_size": my_army}
```

No user interaction needed. Fires automatically in the LIST poll loop.

---

## API URLs Used

```python
SERVER    = badge.server_url()
MY_UUID   = badge.my_uuid()         # hardware UUID
MY_TICKET = badge.my_ticket_uuid()  # ticket UUID — required for source/target filter

SEND_URL = SERVER + "/api/v1/pings"

# Poll for incoming challenges directed AT me
# GET /api/v1/pings requires at least one of source= or target=
POLL_CHALLENGES = (SERVER + "/api/v1/pings"
    + "?requester_badge_uuid=" + MY_UUID
    + "&target=" + MY_TICKET
    + "&type=conquest_challenge&limit=5")

# Poll for responses directed AT me (challenger waiting for result)
POLL_RESPONSES = (SERVER + "/api/v1/pings"
    + "?requester_badge_uuid=" + MY_UUID
    + "&target=" + MY_TICKET
    + "&type=conquest_response&limit=5")
```

---

## Timing Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `POLL_INTERVAL_MS` | 3000 | ms between GET polls |
| `WAIT_TIMEOUT_MS` | 20000 | ms to wait for response before "No response" |
| `RESULT_DISPLAY_MS` | 4000 | ms to display battle result |
| `FLASH_DISPLAY_MS` | 1500 | ms for "Challenge received!" flash |

---

## GC Management

`badge.gc_collect()` called at the top of every main loop iteration to prevent heap exhaustion during polling.
