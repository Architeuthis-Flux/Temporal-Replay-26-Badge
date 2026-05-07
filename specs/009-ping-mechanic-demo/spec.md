# Feature Specification: Badge Ping Demo — Async Ping Mechanic Showcase

**Feature Branch**: `009-ping-mechanic-demo`
**Created**: 2026-03-20
**Status**: In Progress (US1/Messages complete; US2/Conquest not yet started)
**Input**: User description: "I need an implementation of our ping mechanic to confirm it works on both the C and python level. I am open to ideas for what we should do but it should be fun."

## Overview

A two-part feature that showcases the badge's async ping mechanic end-to-end — at both the C and Python layers. All communication is async over WiFi via the `/api/v1/pings` backend endpoint.

**Messages (C layer — new menu item)** — A unified messaging screen. Browse everyone you've booped, select a contact, and send them one of 8 emoji. The emoji is delivered as a directed async ping; incoming emoji from others appear in the same place. Showcases the C-layer ping mechanic.

**Conquest App (Python layer — Apps menu)** — A Warrior's Way-style army battle implemented in MicroPython. Your army size is your lifetime boop count. Select a contact, send a challenge via the async ping backend; the opponent's badge receives it and responds in kind; the higher boop count wins. Showcases the Python-layer ping mechanic.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 — Messages: C-layer async ping (Priority: P1)

A badge holder opens "Messages" from the main menu. They see a list of everyone they've booped — name and company for each. They select a contact, choose one of 8 emoji, and confirm. The emoji is sent as a directed async ping to that person's badge. The recipient's badge, wherever it is, receives the emoji and displays it alongside the sender's name.

Incoming emoji from others appear in the same Messages screen, so the exchange is visible in one place.

**Why this priority**: This is the primary validation of the full async ping mechanic at the C layer. A human deliberately picks a recipient and a message — the delivery is async and the result is visible on the other badge.

**Independent Test**: Two people, two badges, previously booped. One opens Messages, picks the other, sends an emoji. The receiving badge displays the emoji and sender name. The sent record is also verifiable via `GET /api/v1/pings`.

**Acceptance Scenarios**:

1. **Given** a badge with at least one boop pairing, **When** the user opens Messages, **Then** each booped contact appears as a list entry showing their name and company.
2. **Given** a contact is selected, **When** the emoji palette is shown, **Then** the user can scroll through 8 emoji with UP/DOWN and the selection wraps at both ends.
3. **Given** an emoji is selected and confirmed, **When** the ping is sent, **Then** the screen shows a send confirmation.
4. **Given** a badge receives an emoji ping, **When** it arrives, **Then** the badge displays the emoji and the sender's name.
5. **Given** the server is unreachable or returns an error when sending, **When** the send fails, **Then** the badge shows a visible failure message; the failure is not silently dropped.
6. **Given** a badge has no boop history, **When** the user opens Messages, **Then** an empty-state message is shown rather than a blank or error screen.
7. **Given** a user presses BTN_RIGHT from any Messages sub-screen, **Then** the badge steps back (palette → contact list → main menu).

---

### User Story 2 — Conquest App: Python-layer async WiFi battle (Priority: P2)

A badge holder launches the Conquest app from the Apps menu. The screen shows their army size (lifetime boop count) and their contact list. They select one contact to challenge and confirm. A challenge is sent via the async ping backend (`activity_type: "conquest_challenge"` with the challenger's army size). The target badge receives the challenge asynchronously, auto-responds with its own army size via a return ping, and the challenger's badge displays the result when the response arrives.

**Why this priority**: This is the Python-layer showcase of the same async ping mechanic — the `/api/v1/pings` endpoint exercised from MicroPython. The async nature is genuine: sender fires and the result appears when the response arrives. The contact-selection mechanic makes the social graph tangible: your army is your real connections.

**Independent Test**: Two people who have previously booped, both running the Conquest app. One selects the other and sends the challenge. The opponent's badge receives it, auto-responds, and both screens show the battle outcome.

**Acceptance Scenarios**:

1. **Given** the Conquest app is open, **When** the user scrolls with UP/DOWN, **Then** the contact list navigates and the selected contact's name is highlighted.
2. **Given** a contact is selected and confirmed, **When** the challenge ping is sent, **Then** the screen shows "Challenge sent" and polls for the response.
3. **Given** a challenge ping arrives at the target badge, **When** it is received, **Then** the target badge automatically sends a response ping with its army size — no button press required from the defender.
4. **Given** the response ping arrives, **When** the challenger's badge receives it, **Then** it displays the battle outcome (winner / loser / draw) with both army sizes.
5. **Given** no response arrives within a reasonable wait window, **When** the wait expires, **Then** the screen shows "No response" and returns to the contact list.
6. **Given** a badge with an empty contact list, **When** the Conquest app opens, **Then** an empty-state message is shown rather than a crash.
7. **Given** a user presses BTN_RIGHT, **Then** the app exits cleanly to the Apps menu.

---

### User Story 3 — Both features work in a single session (Priority: P3)

A developer completes both features — Messages and Conquest — in a single session without a firmware reload, confirming nothing corrupts shared state between modes.

**Why this priority**: Regression/integration catch. Cannot pass until P1 and P2 each pass independently.

**Acceptance Scenarios**:

1. **Given** a completed Messages session (sent an emoji), **When** the user launches Conquest and challenges a peer, **Then** the battle resolves correctly.
2. **Given** a completed Conquest session, **When** the user opens Messages, **Then** the contact list renders correctly and emoji sending works.

---

### Edge Cases

- What happens if a send returns 403 (no active pairing)? The badge must surface a clear error rather than silently failing.
- What happens if the Contacts list has a partially written entry (storage corruption mid-boop)? Malformed entries must be skipped; valid entries must still render.
- What happens if both Conquest players challenge each other simultaneously? Both badges receive a challenge ping and both auto-respond — the app must handle this without entering a loop.
- What happens if a Conquest challenge is sent but WiFi drops before the response arrives? The wait window must expire cleanly and return to the contact list.

---

## Requirements *(mandatory)*

### Functional Requirements

**Messages (C layer — menu item)**

- **FR-001**: The main firmware menu MUST include a "Messages" item that navigates to the Messages screen.
- **FR-002**: The Messages screen MUST display each previously booped contact as a list entry showing their name and company, navigable with UP and DOWN.
- **FR-003**: Selecting a contact MUST open an emoji palette showing exactly 8 emoji, navigable with UP and DOWN, wrapping at both ends.
- **FR-004**: Confirming an emoji selection MUST POST a ping record to the backend (`activity_type: "emoji"`) containing the chosen emoji, directed at the selected contact's ticket UUID.
- **FR-005**: If the POST fails or returns an error, the badge MUST display a visible failure message.
- **FR-006**: The badge MUST display incoming emoji pings (from any contact) showing the emoji and sender's name.
- **FR-007**: If no contacts exist, the screen MUST show a non-empty placeholder message.
- **FR-008**: BTN_RIGHT MUST step back through sub-screens (palette → contact list → main menu).

**Conquest App (Python layer — Apps menu)**

- **FR-009**: The Conquest app MUST read the badge's boop count from persistent storage and display it as the player's army size.
- **FR-010**: The app MUST display the player's contact list as a scrollable selection showing each contact's name.
- **FR-011**: The player MUST select one contact before a challenge can be sent; browsing does not send anything.
- **FR-012**: Confirming a selection MUST POST a challenge ping (`activity_type: "conquest_challenge"`) containing the challenger's army size, directed at the selected contact's ticket UUID.
- **FR-013**: Receiving a conquest challenge ping MUST trigger an automatic response ping with the defending badge's army size — no user input required.
- **FR-014**: After the response ping arrives, the challenger's badge MUST display the battle result (victory / defeat / draw) with both army sizes.
- **FR-015**: A challenge with no response within a reasonable wait window MUST resolve to "No response" and return to the contact list.
- **FR-016**: Pressing BTN_RIGHT MUST exit the app cleanly.

### Key Entities

- **Contact Entry**: A record of a completed boop stored on the badge; includes the peer's name, company, and ticket UUID as returned by the boop pairing API at consent time.
- **Emoji Ping**: A directed async message from one badge to another containing a chosen emoji; sent and received via the backend ping endpoint; requires an active boop pairing.
- **Army Size**: The badge's lifetime boop count as read from persistent storage; the sole determinant of Conquest battle outcome; never modified by the Conquest app.
- **Challenge Ping**: An async backend ping (`activity_type: "conquest_challenge"`) containing the challenger's army size; sent to a specific contact's ticket UUID; does not block the sender.

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Sending an emoji results in the receiving badge displaying the emoji and sender name; the ping record is verifiable via `GET /api/v1/pings`.
- **SC-002**: A failed send (server unreachable or error response) surfaces a visible error on the sending badge within 10 seconds.
- **SC-003**: The Messages contact list correctly displays name and company for every previously booped badge, verified against a known boop history.
- **SC-004**: A Conquest challenge ping is sent and the challenger's badge shows "Challenge sent" within 2 seconds of confirming; the result is displayed once the response ping arrives.
- **SC-005**: Both features complete successfully in a single session without a firmware reset between them.
- **SC-006**: BTN_RIGHT navigates correctly at every level of both features, 100% of the time.

---

## Assumptions

- The boop pairing response returns `partner_name` and `partner_company` (confirmed via OpenAPI schema `PairingConfirmedResponse`). These fields are stored in NVS at boop time alongside the peer's `ticket_uuid`.
- Boop records store the peer's **ticket UUID** as the persistent identifier, as required by the `/api/v1/pings` endpoint (`target_ticket_uuid`).
- The `/api/v1/pings` POST and GET endpoints are already implemented in the backend. This spec requires no new backend work.
- The Messages screen and the Conquest app read from the same underlying boop history store.
- Test data (fake boop pairings in the DB) will be seeded by the backend team using sample data provided at the end of implementation.
- For the purposes of this spec, the badge is assumed to have at least one existing boop pairing — empty contact list is an edge case, not the primary scenario.
- The 8 emoji are chosen at implementation time; on-screen representation depends on what the display font supports.
- Conquest win/loss records are ephemeral; not persisted when the app exits.
