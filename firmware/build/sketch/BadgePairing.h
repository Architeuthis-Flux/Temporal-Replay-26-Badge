#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgePairing.h"
// BadgePairing.h — WiFi connection, badge association, and re-pair flow
//
// osRun()   — boot sequence: connect WiFi, run paired or unpaired flow, enter menu
// rePair()  — called from loop() when "WiFi / Pair" is selected; reconnects and re-runs flow

#pragma once
#include <Arduino.h>

void osRun();
void rePair();

// Called from loop() each tick while the badge is in non-blocking QR pairing mode.
void pollQRPairing();

// Status string rendered in the corner of the QR screen (e.g. "polling...", "conn err").
extern volatile bool qrPairingActive;
extern char          qrPollStatus[];

// Set true by BTN_DOWN on the QR screen to trigger a single probe.
extern volatile bool qrCheckRequested;

// spec-009 (T006/T007): Fetch badge's own ticket UUID from GET /api/v1/lookup-attendee/<uuid>
// and persist to NVS badge_identity.ticket_uuid. Called at enrollment + boot fallback.
void fetchAndSaveTicketUUID(const char* badge_uuid);
