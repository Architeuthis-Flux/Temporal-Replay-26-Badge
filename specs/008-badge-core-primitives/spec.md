# Feature Specification: Badge Core Primitives Validation & Integration

**Feature Branch**: `008-badge-core-primitives`
**Created**: 2026-03-18
**Status**: Draft
**Input**: User description: "On the C and then python level, let's ensure all of our primatives work correctly. I want to coordinate with the backend to make sure pairing from the badge QR code works, that the badges displays the nametag when in the correct tilt state, that our badges can pair over IR, and that we have a function that leverages ping in someway. We'll also need to make sure our documentation for both the quickstart and the app is upto date, and that our code base is lean, efficient and high quality."

## Clarifications

### Session 2026-03-19

- Q: How does each badge learn the peer's hardware ID over IR? → A: Existing 6-byte eFuse UID transmitted as 6 sequential NEC frames (already implemented); backend receives these short machine IDs, not UUID strings.
- Q: When should the badge POST the boop on button release? → A: Only POST if "boop acquired" was shown (peer UID received). No POST if button released without receiving.
- Q: Should the new hold-to-boop mode drop the PING handshake entirely? → A: No — retain PING handshake but apply aggressive TX jitter (500–2000 ms randomized backoff) to reduce simultaneous-transmit collisions; current 150–350 ms jitter is insufficient.
- Q: What does the display show while UP is held before "boop acquired"? → A: "Booping…" with cycling dots — communicates active TX/RX state.
- Q: How does the badge return to normal after the boop POST result is shown? → A: Stay on result screen until user presses any button to dismiss, then return to menu.

### Session 2026-03-19 (TCP-IR protocol redesign)

- Q: When should a badge passively listen for incoming SYN frames? → A: Boop screen only (MODE_MAIN). IR hardware MUST be disabled on all other screens to conserve battery.
- Q: Is UP a momentary press or held to seek? → A: Momentary press — one edge trigger fires SYN; UP release after that is irrelevant. Protocol runs to completion independently.
- Q: Can BTN_RIGHT abort an in-progress exchange? → A: BTN_RIGHT aborts only pre-handshake (SYN_SENT); once SYN-ACK is received the exchange is committed and BTN_RIGHT is ignored until result screen.
- Q: What is the target window from UP press to both badges reaching the result screen? → A: 15 seconds.
- Q: What does the passive badge show while waiting, and does it auto-update when a SYN arrives? → A: Shows "Ready to boop" while idle on Boop screen; display auto-updates to live exchange status the moment a SYN is received — no user action required on the passive badge.

### Session 2026-03-19 (scope refinement)

- Q: Is tilt debounce at 250ms a bug to fix? → A: No — 250ms is intentional; smoother UX. SC-002 (200ms) is removed from scope.
- Q: Is QR screen not appearing on boot a remaining bug? → A: No — proven to work in hardware testing; not a bug for this spec.
- Q: What is the remaining scope of spec-008? → A: Narrowed to IR collision fix and boop flow only. All work unrelated to IR working and boops working is out of scope.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 — QR Pairing Completes End-to-End (Priority: P1)

An attendee picks up a new badge. The badge shows a QR code on screen. The attendee scans it with their phone, completes registration on the backend, and the badge detects the successful enrollment — transitioning from the unpaired QR screen to the paired nametag view with their name, role, and bitmap displayed.

**Why this priority**: QR pairing is the primary onboarding path. Without it, no attendee can get a personalized badge. All other features (IR boop, nametag display) depend on a paired badge identity.

**Independent Test**: Flash a factory-reset badge, power on, observe QR code on screen, hit the enrollment endpoint via `curl`, observe badge transition to paired state displaying the enrolled name. Backend enrollment endpoint must be implemented and reachable.

**Acceptance Scenarios**:

1. **Given** a badge with no stored identity, **When** the badge boots, **Then** a QR code for the badge's hardware UUID is displayed on screen within 5 seconds of WiFi connect.
2. **Given** a QR code is displayed, **When** the backend receives an enrollment request for that badge UUID, **Then** the badge detects enrollment within the polling window and transitions to showing the nametag screen.
3. **Given** the backend enrollment call fails (network error or 4xx), **When** the badge polls, **Then** it continues showing the QR screen and displaying a human-readable status string (e.g. "polling…" or "conn err") — it does not crash or freeze.
4. **Given** a previously paired badge reboots, **When** boot completes, **Then** the badge skips the QR screen and goes directly to the nametag/menu flow.

---

### User Story 2 — Nametag Shows on Correct Tilt (Priority: P2)

A paired attendee is on the menu screen (not actively using the badge). When they hold the badge up — rotating it so the tilt sensor trips — the nametag bitmap and name are displayed. When they lower/tilt it back, the menu resumes. The nametag tilt is intentionally scoped to the menu state so it does not interrupt Python apps or other active interactions.

**Why this priority**: The tilt-triggered nametag is the badge's primary passive display mode — it's what other attendees see when someone holds up their badge. Scoping it to menu state ensures it activates in the passive "show me your badge" gesture without hijacking an active session.

**Independent Test**: With a paired badge on the menu screen, hold it in each orientation and verify the nametag activates and deactivates correctly. Also verify that while a Python app is running, tilting the badge does not trigger the nametag.

**Acceptance Scenarios**:

1. **Given** a paired badge is on the menu screen in the nametag-active tilt orientation, **When** tilt is read, **Then** the nametag bitmap and name/title are rendered on screen.
2. **Given** a paired badge on the menu screen is tilted away from the nametag orientation, **When** tilt is read, **Then** the display reverts to the menu.
3. **Given** a badge transitions from non-nametag to nametag orientation while on the menu, **When** the transition occurs, **Then** the display updates within 200 ms without visible screen corruption.
4. **Given** an unpaired badge is tilted to nametag orientation, **When** tilt is read, **Then** the badge does not attempt to render an empty nametag — it ignores the tilt.
5. **Given** the badge is in any non-menu state (Python app running, input test, QR screen), **When** the tilt sensor trips, **Then** the nametag is NOT shown — the current screen is unaffected.

---

### User Story 3 — IR Badge-to-Badge Boop (Priority: P2)

Two attendees face their badges toward each other and both hold the UP button. Each badge continuously broadcasts its 6-byte hardware UID via IR while listening for the peer's UID. When a badge successfully receives the peer's full UID, the display shows "boop acquired" beneath the active UI. When both users release UP, each badge independently POSTs the boop to the backend (only if it received the peer's UID). Both badges display a success confirmation.

**Why this priority**: IR pairing is the core social mechanic. It depends on a working IR transport and boop API — all of which must be validated together.

**Interaction model (spec-008 TCP-IR revision)**:
- Both badges navigate to the Boop screen. IR hardware is enabled on entry and disabled on exit.
- The passive badge shows "Ready to boop" and silently listens for a SYN frame — no button press needed.
- The initiating badge presses UP once (momentary edge trigger) to fire a SYN. The display shows "Seeking..." with retransmit status while awaiting a SYN-ACK.
- Once the three-way handshake completes (SYN → SYN-ACK → ACK), both badges display live exchange status (current phase and packet values) throughout the UID transfer.
- The exchange runs to completion regardless of any button state after the handshake. BTN_RIGHT cancels only in the pre-handshake SYN_SENT phase.
- On completion, both badges POST the boop and show the result screen. Any button dismisses the result screen and returns to menu.

**Protocol (spec-008 TCP-IR revision)**:
- A TCP-inspired half-duplex protocol over NEC IR, using two bytes per frame: `addr` = packet type, `cmd` = payload.
- Packet types: `SYN (0xC0)`, `SYN_ACK (0xC1)`, `ACK (0xC2)`, `DATA (0xD0|seq)`, `NACK (0xC4)`, `FIN (0xC5)`, `RST (0xC6)`.
- Three-way handshake establishes roles (initiator/responder) and resolves simultaneous-SYN via cookie comparison (lower cookie = initiator).
- UID exchange is strictly sequential and ACK'd: initiator sends all 6 UID bytes first (each ACK'd), then responder sends its 6 bytes. Each DATA frame carries a sequence number in the addr lower nibble (`0xD0`–`0xD5`), allowing duplicate detection.
- Each byte has a 250 ms ACK timeout with 3 retransmit attempts; exhausting retries sends RST and transitions to `IR_PAIR_FAILED`.
- SYN retransmits up to 10 times at 300 ms intervals before giving up.
- IR hardware is disabled outside MODE_MAIN (battery conservation).
- Backend POST requires `{badge_uuids: [myUID, theirUID]}` — the IR exchange provides `theirUID`; `myUID` comes from NVS.

**Independent Test**: Use two flashed badges. Both hold UP. Verify "boop acquired" appears on both. Both release UP. Verify the Boop workflow completes on the backend.

**Acceptance Scenarios**:

1. **Given** two badges on the Boop screen, **When** neither has pressed UP, **Then** both badges show "Ready to boop" and IR is actively listening for SYN frames.
2. **Given** one badge presses UP (momentary), **When** the SYN is transmitted, **Then** the initiating badge shows "Seeking..." with retransmit count; the passive badge auto-updates to show exchange status as soon as it receives the SYN.
3. **Given** the three-way handshake completes, **When** SYN-ACK and ACK are exchanged, **Then** both badges display live phase and packet status throughout the UID exchange — no further user input required.
4. **Given** the UID exchange completes successfully, **When** both UIDs are received and ACK'd, **Then** both badges POST the boop to the backend and show the result screen within 15 seconds of the initial UP press.
5. **Given** a boop POST succeeds, **Then** the badge displays the boop partner's name on the result screen.
6. **Given** a boop POST fails (network error or 4xx/5xx), **Then** the badge displays an error string and returns to menu without hanging.
7. **Given** the initiating badge presses BTN_RIGHT before SYN-ACK is received, **When** the cancel occurs, **Then** the badge sends RST, aborts the exchange, and returns to the Boop screen idle state.
8. **Given** BTN_RIGHT is pressed after the handshake is established, **Then** it is ignored until the result screen is shown.

---

### User Story 4 — Python `badge` Module Primitives Work (Priority: P3)

A developer writes a Python app that uses the `badge` module to read sensor state, display text, call `badge.ping()` to confirm connectivity, and send/receive raw IR frames. All primitive operations execute correctly without crashing the firmware.

**Why this priority**: The embedded Python runtime is a new platform feature. Ensuring the C-level bridge (`badge` module) exposes correct, stable primitives is a prerequisite for any Python apps running reliably at the event.

**Independent Test**: Write a minimal Python script that calls each `badge` primitive (display, tilt read, ping, ir_send, ir_read), upload it to the badge VFS, launch it from the Apps menu, and observe correct behavior for each call.

**Acceptance Scenarios**:

1. **Given** a Python script calls `badge.ping()`, **When** the badge has WiFi, **Then** the function returns a truthy result confirming backend reachability; when WiFi is absent or backend is unreachable, it returns a falsy result or error without crashing.
2. **Given** a Python script calls a display primitive, **When** the call executes, **Then** text or graphics appear on screen as specified.
3. **Given** a Python script reads the tilt state, **When** the badge is in nametag-active orientation, **Then** the returned value reflects the active tilt state correctly.
4. **Given** a Python app raises an unhandled exception, **When** the exception propagates, **Then** the firmware displays the error and returns to the Apps menu without requiring a hardware reset.
5. **Given** a Python script calls `badge.ir_send(addr, cmd)`, **When** another badge is within IR range, **Then** the frame is transmitted without crashing the firmware.
6. **Given** a Python script calls `badge.ir_start()` then `badge.ir_available()`, **When** IR frames have been received since the last `ir_read()`, **Then** `ir_available()` returns a truthy value; when no frames are queued, it returns a falsy value.
7. **Given** a Python script calls `badge.ir_start()` then `badge.ir_read()`, **When** at least one frame is queued, **Then** the function returns a `(addr, cmd)` tuple and removes that frame from the queue; when no frames are queued, it returns `None`.
8. **Given** a Python app exits (clean, exception, or `badge.exit()`), **When** exit occurs, **Then** IR receive is automatically disabled — the receiver is not left armed after the app ends.

---

### User Story 5 — Documentation Is Accurate and Current (Priority: P3)

A new developer clones the repo, follows the quickstart, and is able to flash a badge and have it connect to the backend within the documented steps — without consulting anyone. A developer writing a Python app can use the app documentation to understand available `badge` primitives and write a working script.

**Why this priority**: Documentation gaps cause onboarding failures and wasted time at events. Accurate docs are a correctness requirement, not a nice-to-have.

**Independent Test**: Follow the quickstart README on a clean machine. Follow the Python app guide to write and deploy a minimal app. Both paths should succeed as documented.

**Acceptance Scenarios**:

1. **Given** the quickstart README, **When** followed step by step on a clean machine with hardware in hand, **Then** the badge is successfully flashed and connects to WiFi without undocumented steps.
2. **Given** the Python app guide, **When** a developer writes a script using only documented `badge` primitives, **Then** the script runs correctly on the badge without needing to inspect C source.
3. **Given** the backend API documentation, **When** a developer calls the QR enrollment endpoint as documented, **Then** the call succeeds and the badge responds as described.

---

### Edge Cases

- What happens when the tilt sensor returns an intermediate/ambiguous reading during orientation change?
- What happens when both badges press UP simultaneously (simultaneous SYN)? → Both send SYN with random cookies; both respond with SYN_ACK carrying their own cookie; lower cookie value wins initiator role.
- What happens if ACK retries are exhausted mid-UID-exchange? → Sender transmits RST, both badges transition to IR_PAIR_FAILED and show the failure result screen.
- What happens when the QR polling task runs while the display is being written from Core 1?
- What happens when `badge.ping()` is called from Python before WiFi is connected?
- What happens when a badge VFS has no Python apps — does the Apps menu show a useful empty state?
- What happens when the backend enrollment endpoint is unreachable during boot QR prefetch?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The badge MUST display a QR code representing its hardware UUID on the unpaired boot screen within 5 seconds of achieving WiFi connectivity.
- **FR-002**: The badge MUST poll the backend for enrollment status at a regular interval while the QR screen is active, and transition to the paired flow automatically when enrollment is confirmed.
- **FR-003**: The badge MUST expose a backend reachability probe (ping) accessible from both the C firmware layer and the Python scripting environment.
- **FR-004**: The badge MUST render the paired nametag (name, title/role, bitmap) when the tilt sensor indicates the nametag-active orientation AND the badge is on the menu screen. Tilt-to-nametag MUST NOT activate while a Python app or any other non-menu screen is active.
- **FR-005**: The tilt-to-nametag transition MUST be debounced so that brief or ambiguous tilt changes do not cause rapid display flickering.
- **FR-006**: The badge MUST support IR badge-to-badge booping via a TCP-inspired NEC protocol. The IR hardware (sender and receiver) MUST be enabled only when the badge is on the Boop screen (MODE_MAIN) and MUST be disabled on all other screens to conserve battery. On the Boop screen, the badge passively listens for SYN frames; pressing UP initiates a SYN and begins the handshake. Once the three-way handshake (SYN/SYN-ACK/ACK) completes, the UID exchange runs to completion regardless of button state. The display MUST show live protocol status (current phase and packet type/value) throughout the exchange.
- **FR-007**: Pressing UP on the Boop screen fires a single SYN (momentary edge trigger). The badge does NOT need to hold UP — the exchange runs to completion once a handshake is established. BTN_RIGHT cancels only in the pre-handshake SYN_SENT phase; once SYN-ACK is received BTN_RIGHT is ignored until the result screen is shown. On exchange completion, the badge POSTs the boop to the backend. The badge MUST display the peer's name on success and an error string on failure. The result screen MUST remain displayed until the user presses any button to dismiss, then the badge returns to the menu.
- **FR-008**: The Python `badge` module MUST expose primitives for: display output, tilt state read, ping/connectivity check, IR send, and explicit IR receive. IR receive MUST be off by default (power conservation). `badge.ir_start()` / `badge.ir_stop()` MUST arm and disarm the receiver. `badge.ir_available()` and `badge.ir_read()` MUST be functional when receive is active. The firmware MUST automatically call the IR stop bridge when any Python app exits.
- **FR-009**: Unhandled Python exceptions MUST be caught at the firmware boundary, displayed on screen, and return control to the Apps menu without requiring a hardware reset.
- **FR-010**: The quickstart documentation MUST reflect the current flash, configure, and boot procedure accurately — including `BadgeConfig.h` setup, `build.sh`, and `flash.py` usage.
- **FR-011**: The Python app guide MUST document all available `badge` module primitives with examples sufficient for a developer to write a working app without reading C source.
- **FR-012**: Dead code, unused modules, and stale documentation artifacts MUST be removed from the active firmware and repo during this pass.

### Key Entities

- **Badge**: Physical device with hardware UUID, paired/unpaired state, enrolled name/role/bitmap stored in NVS.
- **Enrollment**: Backend-side action that associates a UUID with attendee identity; transitions the badge from unpaired to paired.
- **Boop**: A mutual IR pairing event between two badges, recorded on the backend as a workflow.
- **Tilt State**: Binary orientation reading from the hardware tilt sensor indicating whether the nametag orientation is active.
- **Ping**: A lightweight probe confirming that the badge can reach the backend API; returns success/failure with no side effects.
- **Python App**: A `.py` script stored on badge VFS and executed by the embedded MicroPython runtime via the Apps menu.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A factory-reset badge completes the full QR → enrollment → nametag flow end-to-end without manual intervention beyond scanning the QR code.
- **SC-002**: The nametag activates within 200 ms of the badge reaching the correct tilt orientation and deactivates within 200 ms of leaving it.
- **SC-003**: From the moment UP is pressed on the initiating badge (both badges on Boop screen), both badges MUST reach the result screen (success or failure) within 15 seconds. Neither badge should hang or require a reset.
- **SC-004**: `badge.ping()` returns a result (success or failure) in under 3 seconds from a Python app, with no firmware crash on any network state.
- **SC-005**: A developer with the repo and hardware can follow the quickstart and have a flashed, WiFi-connected badge within 15 minutes, with zero undocumented steps.
- **SC-006**: A developer using only the Python app guide can write and deploy a working app that uses at least 3 `badge` primitives without consulting C source or asking for help.
- **SC-007**: The firmware builds cleanly (`./build.sh -n` exits 0) with no warnings introduced by changes in this feature.

## Assumptions

- The backend enrollment endpoint (`POST /api/v1/badges/enroll`) will be implemented as part of this spec's scope, or is already in progress; if not, QR pairing scenarios requiring backend coordination will be blocked.
- "Ping" is defined as a non-mutating HTTP probe to the backend that confirms network reachability — not an ICMP ping or IR broadcast.
- The tilt sensor hardware (tilt ball / GPIO43) is already functional in firmware; this spec validates correct software behavior on top of it.
- Python `badge` module already exposes display primitives from spec-007; this spec adds ping and tilt-read primitives.
- The "quickstart" refers to the top-level `README.md` and any firmware-level `README` already in the repo; no new documentation files are created, only existing ones updated.
- Code quality work (removing dead code, stale artifacts) is scoped to the active firmware directory (`firmware/Firmware-0308-modular/`) and the Python app layer.
1