# Feature Specification: Firmware UI & API Integration

**Feature Branch**: `003-firmware-ui-api`
**Created**: 2026-03-10
**Status**: Draft
**Input**: User description: "Create spec 002-firmware-ui-and-api for the Temporal Badge DELTA MicroPython firmware — flash tool reliability, hardware UI verification, WiFi boot path, badge API client, live nametag from server."

## User Scenarios & Testing *(mandatory)*

### User Story 1 — Reliable Badge Update Tool (Priority: P1)

A developer needs to update Python files on a running badge without performing a full firmware reflash. Currently the update tool fails silently: file names appear in the terminal before any copy actually happens, and the tool cannot interrupt the badge's startup sequence. The developer cannot trust whether files were actually updated.

**Why this priority**: All subsequent development depends on being able to reliably push code changes to the badge. Without a working update tool, every change requires a full firmware flash cycle, blocking iteration on all other user stories.

**Independent Test**: Developer runs `./flash.sh --vfs-only` on a powered-on badge; every file name appears in the terminal only after that file is confirmed copied; the badge responds normally afterward.

**Acceptance Scenarios**:

1. **Given** the badge is powered on and running its startup sequence, **When** the developer runs `./flash.sh --vfs-only`, **Then** the tool reliably interrupts the badge and enters copy mode within 10 seconds.
2. **Given** the tool has entered copy mode, **When** a file is successfully copied, **Then** the file name is printed to terminal; if copy fails, an error is printed instead (not the file name).
3. **Given** `./flash.sh --vfs-only` completes, **When** the badge restarts, **Then** the VFS filesystem mounts cleanly without format errors.

---

### User Story 2 — Verified Hardware Inputs & Display (Priority: P2)

A hardware engineer needs confidence that all physical inputs on the badge (buttons, joystick, tilt sensor) produce the correct visual feedback on the OLED display. The display uses procedural drawing functions rather than stored images.

**Why this priority**: Hardware verification must happen before adding network features. A badge with broken button or display logic cannot be used at a conference regardless of backend connectivity.

**Independent Test**: Tester exercises each input on a live badge and observes the OLED; no backend connection required.

**Acceptance Scenarios**:

1. **Given** a powered badge in the main UI loop, **When** each of the 4 directional buttons is pressed individually, **Then** a filled dot appears at the correct position in the d-pad area of the display (active-low logic: dot shows when button is physically held).
2. **Given** the main UI is running, **When** the joystick is moved in any direction, **Then** a dot tracks within a 6-pixel radius circle on the display.
3. **Given** the badge is tilted, **When** the tilt sensor holds a new state for 300 ms, **Then** a visual indicator on the display changes position to reflect the new orientation.
4. **Given** the badge is idle, **When** an IR signal is transmitted or received, **Then** directional arrows animate on the display for the duration of the event.
5. **Given** any display state, **When** `draw_base()` renders the chrome layer, **Then** the TEMPORAL header, status bar, joystick circle, and d-pad crosshair all appear at expected positions with no rendering artifacts.

---

### User Story 3 — WiFi Connection at Startup (Priority: P3)

An attendee powers on their badge at the conference venue. The badge automatically connects to the venue WiFi using stored credentials, syncs the clock, and shows the connection status on the OLED before loading the main UI.

**Why this priority**: WiFi connectivity is the prerequisite for all server-connected features. Without it, the badge operates in offline-only mode and cannot display live attendee data.

**Independent Test**: Badge with valid credentials powers on; OLED shows "Connected to \<SSID\>" and the system clock is confirmed synced in the REPL.

**Acceptance Scenarios**:

1. **Given** credentials are stored on the badge, **When** the badge powers on, **Then** it attempts WiFi connection within the configured timeout and displays progress on the OLED.
2. **Given** valid credentials and a reachable access point, **When** connection succeeds, **Then** the OLED shows "Connected to \<SSID\>" and the system clock is synchronized.
3. **Given** invalid credentials or an unreachable access point, **When** the connection attempt exceeds the configured timeout, **Then** the OLED shows "WiFi failed" and the badge halts without proceeding to server-dependent features.

---

### User Story 4 — Badge-to-Server Communication Client (Priority: P4)

A developer needs a self-contained module on the badge that handles all communication with the conference backend: fetching attendee info, initiating badge pairings, and checking pairing status. The module must be memory-safe and resilient to network errors. QR codes are generated on-device and do not require a server call.

**Why this priority**: This is the foundational library for all server-connected features. It must exist and work correctly before any feature that calls backend endpoints can be built or tested.

**Independent Test**: `badge_api.py` is deployed to the badge; a developer manually calls each function from the REPL and verifies correct return values and graceful error handling across success, network error, and not-found scenarios.

**Acceptance Scenarios**:

1. **Given** the badge is connected to WiFi, **When** `get_badge_info(badge_uuid)` is called, **Then** a dictionary with `name`, `title`, `company`, and `attendee_type` is returned, or `None` if the badge is not registered (404).
2. **Given** two badge UUIDs are known, **When** `create_boop(uuid1, uuid2)` is called, **Then** a response with pairing status (`pending` or `confirmed`) is returned.
3. **Given** a workflow ID from a pending boop, **When** `get_boop_status(workflow_id, badge_uuid)` is called, **Then** the current pairing status is returned.
4. **Given** any API call encounters a network error or non-200/202 response, **When** the call completes, **Then** the function returns `None` or raises a descriptive error; no raw HTTP response object is exposed to the caller.
5. **Given** any API call completes (success or failure), **When** the function returns, **Then** the HTTP connection has been closed to free device memory.
6. **Given** authentication must be added to all requests, **When** the transport layer is updated inside the API client module, **Then** all endpoint functions gain authentication behavior without any changes to their callers (boot, main loop, SDK).

**Key Endpoints**:

| Endpoint | Purpose |
|----------|---------|
| `GET /api/v1/badge/{uuid}/info` | Fetch attendee name, title, company |
| `POST /api/v1/boops` | Initiate badge pairing workflow |
| `GET /api/v1/boops/status/{workflow_id}` | Poll pairing consent status |
| `GET /api/v1/boops` | List all pairings for a badge |
| `DELETE /api/v1/boops/pending` | Cancel a pending pairing |

---

### User Story 5 — Live Attendee Nametag at Startup (Priority: P5)

An attendee powers on their badge. After connecting to WiFi, the badge fetches their registration data from the conference server and displays their name, title, and company on the OLED. If the badge is not yet registered, a brief message appears before the badge proceeds to the main UI.

**Why this priority**: This is the primary attendee-facing value of the network connection — the badge personalizes itself with live registration data. It depends on US3 (WiFi) and US4 (API client).

**Independent Test**: Registered badge powers on with valid WiFi → real name shown on OLED. Unregistered badge → "Not registered" shown for 2 seconds, then main UI loads.

**Acceptance Scenarios**:

1. **Given** a registered badge with WiFi connected, **When** startup fetches attendee info, **Then** the OLED displays name on line 1, title on line 2, and company on line 3.
2. **Given** any text field is longer than 16 characters, **When** rendered on the OLED, **Then** the text is truncated at 16 characters with no overflow or display corruption.
3. **Given** an unregistered badge (server returns 404), **When** startup attempts to fetch info, **Then** the OLED shows "Not registered" for 2 seconds and then proceeds to the main UI without error.
4. **Given** a network error during info fetch, **When** startup completes, **Then** the badge proceeds to the main UI without hanging or crashing.

---

### Edge Cases

- What happens when the badge VFS is unformatted on first boot? The filesystem must be formatted automatically rather than crashing.
- What happens when `./flash.sh --vfs-only` is run but the badge is in a reset loop? The tool must timeout and report an error rather than hanging indefinitely.
- What happens when the credentials file is missing or malformed? The badge must fail gracefully with a clear error display rather than an unhandled exception.
- What happens when the NTP server is unreachable but WiFi connects? The badge must continue with an unsynchronized clock rather than halting.
- What happens when attendee name, title, or company fields are null in the API response? Each field must render as blank rather than crashing.
- What happens when a boop POST returns HTTP 202 (first badge to boop) vs 200 (second badge confirming)? Both response shapes must be handled correctly.

## Requirements *(mandatory)*

### Functional Requirements

**Flash Tool Reliability**

- **FR-001**: The VFS update tool MUST be able to interrupt a running badge and enter file-copy mode even when the badge startup sequence is executing mid-sleep.
- **FR-002**: The VFS update tool MUST print each file name to the terminal only after that file has been confirmed successfully copied; errors must print an error message instead.
- **FR-003**: The frozen filesystem mount module MUST attempt to mount the VFS regardless of prior state; an already-mounted filesystem MUST be treated as success; an unformatted filesystem MUST trigger automatic formatting.
- **FR-004**: The production firmware binary MUST incorporate the fixed filesystem mount behavior from FR-003.

**Hardware UI**

- **FR-005**: The display MUST render buttons as active-low: a dot appears when the button GPIO reads low (physically pressed).
- **FR-006**: The joystick position MUST be represented as a dot constrained within a 6-pixel radius circle on the display.
- **FR-007**: The tilt indicator MUST debounce: the display indicator MUST only change position after the tilt sensor holds a new state for 300 ms.
- **FR-008**: IR transmit and receive events MUST each trigger a corresponding arrow animation on the display for the duration of the event.
- **FR-009**: The base display chrome MUST include: TEMPORAL header, status bar area, joystick circle, and d-pad crosshair, rendered on every frame.

**WiFi Boot Path**

- **FR-010**: At startup, the badge MUST attempt WiFi connection using credentials from the stored credentials file, up to the configured connection timeout.
- **FR-011**: On successful WiFi connection, the badge MUST synchronize the system clock and display the connected network name on the OLED.
- **FR-012**: On WiFi connection failure, the badge MUST display a failure message on the OLED and halt; it MUST NOT proceed to server-dependent features.

**Badge API Client**

- **FR-013**: The badge API client MUST implement named functions for each badge-facing endpoint: fetch attendee info, create boop, get boop status, list boops, cancel pending boop. QR codes MUST be generated on-device; no QR endpoint MUST be called.
- **FR-014**: Every API function MUST close the HTTP response before returning, regardless of success or failure.
- **FR-015**: Every API function MUST return `None` or raise a descriptive error on HTTP errors or network failures; raw HTTP response objects MUST NOT be exposed to callers.
- **FR-016**: The server base URL MUST be sourced from the credentials configuration file; it MUST NOT be hardcoded in the API client module.
- **FR-017**: The badge API client module MUST be included in the VFS deploy list so it is copied to the badge alongside other runtime files.
- **FR-018**: All named endpoint functions MUST delegate HTTP execution to a single internal transport function. Authentication, headers, and signing logic MUST be added exclusively in the transport function without modifying any endpoint function.
- **FR-019**: All callers outside the badge API client module (boot, main loop, SDK) MUST interact only through the named endpoint functions and MUST NOT make raw HTTP calls directly. Swapping in a new version of the badge API client module MUST be sufficient to change all network and authentication behavior.

**Live Nametag**

- **FR-020**: After a successful WiFi connection, the badge MUST fetch attendee info and display name (line 1), title (line 2), and company (line 3) on the OLED using native drawing functions.
- **FR-021**: Each text field MUST be truncated to 16 characters if longer; the display MUST NOT overflow or corrupt adjacent lines.
- **FR-022**: If the server returns a 404 for the badge UUID, the badge MUST display "Not registered" for 2 seconds and then proceed to the main UI without error.

### Key Entities

- **Badge**: A physical conference device identified by a unique UUID. Paired to an attendee ticket on the server.
- **Attendee Info**: Registration data associated with a badge — name, title, company, and attendee type. May be null for unregistered badges.
- **Boop / Pairing**: A mutual consent connection between two badges, managed as a long-running workflow on the server. Identified by a workflow ID that can be polled for status.
- **QR Code**: A matrix barcode encoding the badge's registration link, generated entirely on-device without a server call.
- **Credentials File**: A configuration file stored on the badge containing WiFi SSID, password, and server base URL. Not committed to source control.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A developer can deploy updated Python files to a running badge in under 60 seconds using the VFS-only update path, with zero ambiguity about whether each file was successfully copied.
- **SC-002**: All 4 buttons, the joystick, and the tilt sensor produce correct and immediate visual feedback on the OLED when exercised manually; zero unhandled exceptions during a 5-minute interactive session on real hardware.
- **SC-003**: A badge with valid credentials connects to WiFi and displays its attendee name within 15 seconds of power-on under normal network conditions.
- **SC-004**: A badge with invalid or missing credentials halts with a clear error display and makes zero network calls; recovery requires only correcting the credentials file and rebooting.
- **SC-005**: All badge-facing API functions return correct parsed data or fail gracefully — no unhandled exceptions — across at minimum: successful call, network timeout, and HTTP 404 scenarios.
- **SC-006**: An unregistered badge reaches the main UI within 5 seconds of the "Not registered" message appearing; no error crashes occur.

## Assumptions
M
- The conference backend at the staging URL is the authoritative source for all API contracts. Endpoint paths, request/response shapes, and status codes match the live OpenAPI spec fetched 2026-03-10.
- `creds.py` (WiFi credentials + server URL) is managed out-of-band and is already present on the badge before network-dependent features are exercised.
- The badge UUID is available as a string to all modules that need it (stored in NVS or derived at runtime); this spec does not define how it is obtained.
- `uQR.py` is already on the badge VFS for local QR generation and is not modified by this spec.
- The full boop polling loop (repeated status checks during IR pairing) is deferred to a future spec covering the end-to-end IR pairing UX flow; this spec only requires that the API client can make individual boop calls.
- `WIFI_TIMEOUT_MS` is already defined in `config.py`; this spec does not change that value.
- Firmware rebuild (to bake in the fixed `_boot.py`) is in scope for US1 and is required before US3–US5 can be verified.
- HMAC request signing (badge authentication) is deferred to a future spec. The transport layer in the API client is designed as the single insertion point for auth headers so that adding it later requires no changes to endpoint functions or their callers.
