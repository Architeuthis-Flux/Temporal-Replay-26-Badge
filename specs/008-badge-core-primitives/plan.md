# Implementation Plan: Badge Core Primitives — TCP-IR Boop Protocol

**Branch**: `008-badge-core-primitives` | **Date**: 2026-03-19 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/008-badge-core-primitives/spec.md`

## Summary

Replace the broken simultaneous-TX IR boop protocol with a TCP-inspired half-duplex
exchange over NEC IR. The new protocol uses a three-way handshake (SYN/SYN-ACK/ACK)
to establish roles, then exchanges 6-byte eFuse UIDs sequentially with per-byte ACKs
and retransmission. IR hardware is enabled only on the Boop screen to save battery.
One badge presses UP (momentary edge trigger) to initiate; the other listens passively.
Live protocol status is shown on both screens throughout the exchange.

---

## Technical Context

**Language/Version**: Arduino C++ (C++17, ESP32 Arduino core 3.x)
**Primary Dependencies**: IRremote, U8G2, ArduinoJson 6.x, Preferences, HTTPClient, WiFi, MicroPython v1.27.0 embed
**Storage**: ESP32 NVS via `Preferences`; LittleFS VFS for Python app `.py` files
**Testing**: Manual hardware verification (two badges); serial log analysis
**Target Platform**: ESP32-S3-MINI-1 (XIAO form factor)
**Project Type**: Embedded firmware
**Performance Goals**: Full SYN→UID-exchange→result within 15 seconds; each NEC frame ~64ms
**Constraints**: Single-core IR task (Core 0); no heap allocation in irTask; 8KB FreeRTOS task stack
**Scale/Scope**: 2,000 units; single canonical source in `firmware/Firmware-0308-modular/`

---

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Arduino C++ First | ✅ PASS | All protocol logic in C++; no firmware behavior moved to scripting |
| II. Firmware-0308 as Behavioral Source | ✅ PASS (documented deviation) | TCP-IR protocol replaces old simultaneous-TX approach. Deviation documented in spec clarifications and commit message. Old Firmware-0308 IR behavior is superseded by this spec. |
| III. Credentials-at-Build | ✅ PASS | No new credentials; `BadgeConfig.h` unchanged |
| IV. Backend Contract Compliance | ✅ PASS | `POST /api/v1/boops` payload unchanged; backend receives same `{badge_uuids: [...]}` |
| V. Reproducible Build | ✅ PASS | No new dependencies; build.sh unchanged |
| VI. Hardware Safety | ✅ PASS | IR_TX=GPIO3, IR_RX=GPIO4 unchanged; IrSender.begin/IrReceiver.begin called on screen entry; IrReceiver.end on screen exit |
| VII. API Contract Library | ✅ PASS | BadgeAPI::createBoop unchanged; HTTP layer not modified |

**Complexity Tracking** (Principle II deviation):

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|--------------------------------------|
| TCP-IR replaces old PING-burst protocol | Hardware testing proved simultaneous TX is architecturally unsolvable with the old protocol; both badges stop their receivers during 800ms UID burst, guaranteed collision | Jitter tuning (T002/T003) was implemented and tested; logs confirmed window expires before peer sends UID — timing gap is structural, not a tuning problem |

---

## Project Structure

### Documentation (this feature)

```text
specs/008-badge-core-primitives/
├── plan.md              # This file
├── research.md          # Updated with TCP-IR protocol decisions (8–11)
├── data-model.md        # IrPhase states, IrStatus struct, packet type encoding
├── quickstart.md        # IR boop quickstart for developers
├── contracts/
│   ├── badge-ir-protocol.md   # TCP-IR NEC packet encoding contract (new)
│   ├── badge-c-abi.md         # C ABI for BadgeIR (updated: new phases, IR enable/disable)
│   └── badge-python-api.md    # Python badge module (unchanged)
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (affected files only)

```text
firmware/Firmware-0308-modular/
├── BadgeIR.h            # New IrPhase enum, packet constants, updated IrStatus struct
├── BadgeIR.cpp          # Full irTask() rewrite; IR enable/disable helpers
├── BadgeDisplay.cpp     # renderMain() updated for new phases + live status display
├── BadgeInput.cpp       # boopEngaged: continuous → edge trigger; IR enable on MODE_MAIN
└── Firmware-0308-modular.ino  # boopEngaged edge trigger handling; IR lifecycle on screen switch
```

**Structure Decision**: Single-module embedded firmware. All changes are contained within
`firmware/Firmware-0308-modular/`. No new files; no new dependencies.

---

## Phase 0: Research

See [research.md](research.md) — Decisions 8–11 added for the TCP-IR protocol redesign.

---

## Phase 1: Design

### IR Protocol State Machine

```
irTask() on Core 0 — FreeRTOS task, always running

IR hardware lifecycle:
  Screen enters MODE_MAIN  → IrSender.begin() + IrReceiver.begin()  [Core 1 signal]
  Screen leaves MODE_MAIN  → IrReceiver.end() + IrSender power-off  [Core 1 signal]

States:
  IR_IDLE          → on MODE_MAIN entry; passive SYN listener active
  IR_SYN_SENT      → UP pressed (edge); sending SYN frames with backoff
  IR_SYN_RECEIVED  → SYN received; sent SYN_ACK; waiting for ACK
  IR_ESTABLISHED   → handshake complete; role determined
  IR_TX_UID        → sending our 6 UID bytes (INITIATOR first, RESPONDER second)
  IR_RX_UID        → receiving peer's 6 UID bytes
  IR_PAIR_CONSENT  → UIDs exchanged; HTTP POST in progress
  IR_PAIRED_OK     → POST succeeded; result screen
  IR_PAIR_FAILED   → timeout / retries exhausted / RST received; result screen
  IR_PAIR_CANCELLED → BTN_RIGHT in SYN_SENT; result screen
```

### Packet Encoding

```
NEC frame: addr (8-bit) + cmd (8-bit)

Control packets (addr = type, cmd = payload):
  SYN     0xC0  cmd = my_cookie (random 1–255)
  SYN_ACK 0xC1  cmd = my_cookie
  ACK     0xC2  cmd = seq being acknowledged (0–5)
  NACK    0xC4  cmd = seq expected
  FIN     0xC5  cmd = 0x00 (reserved, not used in v1)
  RST     0xC6  cmd = 0x00

Data packets (addr encodes type + seq):
  DATA    addr = 0xD0 | seq (0xD0–0xD5), cmd = UID byte value
```

### Role Determination

- Normal case (one UP press, one listener): UP presser = INITIATOR, listener = RESPONDER
- Simultaneous SYN: both get each other's cookies via SYN_ACK; lower cookie = INITIATOR; tie → lower uid_hex[0]

### UID Exchange Order

```
INITIATOR: TX UID bytes 0–5 (DATA 0xD0..0xD5), each ACK'd by RESPONDER
RESPONDER: TX UID bytes 0–5 (DATA 0xD0..0xD5), each ACK'd by INITIATOR
→ Both have peer UID → trigger boopPairingRequested → HTTP POST
```

### Timing Parameters

```cpp
#define IR_SYN_RETRY_MS       300   // SYN retransmit interval
#define IR_SYN_MAX_RETRIES    10    // give up after 10 SYNs (~3s)
#define IR_SYNACK_TIMEOUT_MS  400   // wait for ACK after sending SYN_ACK
#define IR_SYNACK_MAX_RETRIES 5
#define IR_ACK_TIMEOUT_MS     250   // wait for DATA ACK
#define IR_ACK_MAX_RETRIES    3     // 3 retries per byte before RST
#define IR_DATA_TIMEOUT_MS   1500   // wait for next DATA frame in RX_UID
```

### Display During Exchange

```
IR_IDLE:          "Ready to boop"   (passive listener active)
IR_SYN_SENT:      "Seeking..."      + statusMsg ("TX 0xC0:0xNN retry N")
IR_SYN_RECEIVED:  "Connecting..."   + statusMsg ("SYN_ACK 0xC1:0xNN")
IR_ESTABLISHED:   "Connected"       + role ("INIT" / "RESP")
IR_TX_UID:        "Sending UID..."  + statusMsg ("TX[N]=0xNN")
IR_RX_UID:        "Receiving..."    + partial peer UID hex filling in
IR_PAIR_CONSENT:  "Booping..."      + peer UID
IR_PAIRED_OK:     result screen     (existing renderBoopResult)
IR_PAIR_FAILED:   result screen
IR_PAIR_CANCELLED: result screen
```

### IR Hardware Lifecycle (new)

Core 1 (`loop()`) owns screen transitions. When `renderMode` changes:
- `→ MODE_MAIN`: set `irHardwareEnabled = true` → irTask enables IrReceiver on next iteration
- `← MODE_MAIN`: set `irHardwareEnabled = false` → irTask stops IrReceiver and resets to IR_IDLE

Implemented via a `volatile bool irHardwareEnabled` flag (Core 1 writes, Core 0 reads).

### boopEngaged Edge Trigger (BadgeInput.cpp change)

```cpp
// Old: continuous (held state)
boopEngaged = (buttons[0].state == LOW) && !boopTaskInFlight;

// New: edge trigger — fires once on UP press when on Boop screen
// in onButtonPressed(case 0), when renderMode == MODE_MAIN:
if (!boopTaskInFlight && !irExchangeActive) {
    boopEngaged = true;   // consumed and cleared by irTask
}
```

`irExchangeActive` is a `volatile bool` set by irTask during an exchange to prevent re-triggering.
