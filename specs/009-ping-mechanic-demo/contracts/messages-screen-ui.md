# Contract: Messages Screen UI (spec-009)

## Overview

A new C++ menu item "Messages" is added between "Boop" and "QR / Pair". Its UI is managed by a new module `BadgeMessages` (or inline in `BadgeMenu`/main `.ino` loop — decision for implementer based on complexity).

---

## Menu Change

**Before (4 items):**
```
0: Boop
1: QR / Pair
2: Input Test
3: Apps
```

**After (5 items):**
```
0: Boop
1: Messages       ← NEW
2: QR / Pair
3: Input Test
4: Apps
```

`MENU_COUNT` changes from `4` to `5` in `BadgeMenu.h`.

---

## Screen States

```
CONTACT_LIST → EMOJI_PALETTE → SENDING → SENT/ERROR
     ↑                                       ↓
     └───────────────────────────────────────┘
                 (BTN_RIGHT at any point)
```

Also handles a passive **INCOMING** sub-state that interrupts CONTACT_LIST to display an inbound ping.

---

## Screen: CONTACT_LIST

**Renders:**
- Title: `"Messages"`
- Each line: `"<name>  <company>"` (truncated to 21 chars)
- Cursor highlight on selected contact (inverse render)
- If no contacts: single line `"No contacts yet"` (FR-007)

**Input:**
- `BTN_UP` / `BTN_DOWN` or joystick Y: scroll contact list
- `BTN_DOWN` (select) or joystick press: enter EMOJI_PALETTE for selected contact
- `BTN_RIGHT`: exit to main menu

**Poll behavior:**
- Every 5 seconds, calls `BadgeAPI::getPings(requester=my_uuid, target=my_ticket_uuid, type=PING_TYPE_EMOJI, ...)`
- `target=my_ticket_uuid` is required — without `source` or `target` the endpoint returns 422
- On results: transitions to INCOMING sub-state if new pings arrive
- Uses `lastPingId` + `lastPingTs` cursor fields to avoid re-showing seen pings

---

## Screen: EMOJI_PALETTE

**Renders:**
- Line 1: `"To: <contact name>"`
- Lines 2–5: emoji entries; cursor highlight on selected
- Emoji set (8 total): `♥  ★  ✓  ⚡  ☺  !  ?  …`
  *(fallback ASCII if font lacks symbols: `<3  *  ok  zap  hi  !!  ??  ...`)*

**Input:**
- `BTN_UP` / `BTN_DOWN`: scroll emoji list; wraps at both ends
- `BTN_DOWN` (select): confirm → SENDING
- `BTN_RIGHT`: back to CONTACT_LIST

---

## Screen: SENDING

**Renders:**
- `"Sending..."`
- Blocks for duration of HTTP POST (typically <3s)

**Transition:**
- On `ok=true`: → SENT
- On `ok=false`: → ERROR

---

## Screen: SENT

**Renders:**
- `"Sent! ♥"` (or chosen emoji)
- Auto-returns to CONTACT_LIST after 2 seconds

---

## Screen: ERROR

**Renders:**
- `"Send failed"` on line 1
- HTTP code if available: `"HTTP 403"` on line 2
- Auto-returns to CONTACT_LIST after 3 seconds (FR-005)

---

## Screen: INCOMING (sub-state overlaid on CONTACT_LIST)

**Renders:**
- Line 1: `"<sender name>"`
- Line 2: `"sent you:"` or `"says:"`
- Line 3: emoji or fallback label
- Auto-clears after 4 seconds, returns to CONTACT_LIST

**Trigger:** New `PingRecord` with `activity_type == PING_TYPE_EMOJI` where `target_ticket_uuid` matches this badge's ticket UUID.

---

## Module Boundary

The Messages screen logic is self-contained. Entry point called from the main `loop()` when `menuIndex == MENU_MESSAGES`. Exit returns control to the menu loop. No new FreeRTOS tasks — ping polling is called from within the Messages render/input loop.

`BadgeConfig.h` addition: `#define MSG_POLL_INTERVAL_MS 5000`
