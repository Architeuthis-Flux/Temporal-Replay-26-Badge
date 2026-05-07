# Contract: Badge TCP-IR NEC Packet Encoding

**Feature**: `008-badge-core-primitives`
**Date**: 2026-03-19
**Protocol version**: v1

---

## Overview

The badge-to-badge IR boop protocol is a TCP-inspired half-duplex exchange over NEC IR.
It uses a three-way handshake to establish roles, then exchanges 6-byte eFuse UIDs
sequentially with per-byte ACKs and retransmission.

**Physical layer**: NEC IR protocol via IRremote library.
Each NEC frame carries: `addr` (8-bit) + `cmd` (8-bit).

---

## Packet Encoding

### Control Packets — `addr = packet type, cmd = payload`

| Packet | addr | cmd | Description |
|--------|------|-----|-------------|
| SYN | `0xC0` | `my_cookie` (random 1–255) | Initiate handshake |
| SYN_ACK | `0xC1` | `my_cookie` (random 1–255) | Acknowledge SYN; include own cookie |
| ACK | `0xC2` | `seq` (0–5) | Acknowledge DATA byte at seq |
| NACK | `0xC4` | `seq_expected` (0–5) | Request retransmission of seq |
| FIN | `0xC5` | `0x00` | Reserved; not used in v1 |
| RST | `0xC6` | `0x00` | Abort exchange; peer must return to IR_IDLE |

### Data Packets — `addr = 0xD0 | seq, cmd = UID byte`

| Packet | addr | cmd | Description |
|--------|------|-----|-------------|
| DATA[0] | `0xD0` | UID byte 0 | First UID byte |
| DATA[1] | `0xD1` | UID byte 1 | |
| DATA[2] | `0xD2` | UID byte 2 | |
| DATA[3] | `0xD3` | UID byte 3 | |
| DATA[4] | `0xD4` | UID byte 4 | |
| DATA[5] | `0xD5` | UID byte 5 | Last UID byte |

**Duplicate detection**: A retransmitted DATA frame has identical `addr` + `cmd`. The
receiver ACKs it immediately without re-writing the byte (idempotent receive).

---

## Handshake: Three-Way (SYN / SYN-ACK / ACK)

```
INITIATOR                         RESPONDER
    |                                 |
    |-- SYN (0xC0, cookie=X) -------->|  [INITIATOR sends first; enters IR_SYN_SENT]
    |                                 |  [RESPONDER was IR_IDLE, enters IR_SYN_RECEIVED]
    |<- SYN_ACK (0xC1, cookie=Y) -----|  [RESPONDER sends SYN_ACK with own cookie]
    |                                 |
    |-- ACK (0xC2, seq=0) ----------->|  [INITIATOR ACKs, both enter IR_ESTABLISHED]
    |                                 |
```

### Simultaneous SYN (both badges press UP)

Both badges send SYN; both receive each other's cookies via SYN_ACK.

**Role determination**: `lower cookie → INITIATOR`. Tie-break: `lower uid_hex[0]`.

```
BADGE-A                           BADGE-B
    |-- SYN (0xC0, cookie=A) -------->|
    |<- SYN (0xC0, cookie=B) ---------|

    (both send SYN_ACK with own cookie, receive peer cookie)

    If cookie_A < cookie_B:
      A = INITIATOR, B = RESPONDER
    Else if cookie_B < cookie_A:
      B = INITIATOR, A = RESPONDER
    Else (tie):
      lower uid_hex[0] = INITIATOR
```

---

## UID Exchange

After `IR_ESTABLISHED`, roles determine TX order:

```
INITIATOR                         RESPONDER
    |-- DATA[0] (0xD0, uid[0]) ------>|
    |<- ACK (0xC2, seq=0) ------------|
    |-- DATA[1] (0xD1, uid[1]) ------>|
    |<- ACK (0xC2, seq=1) ------------|
    | ... (bytes 2–5) ...             |
    |-- DATA[5] (0xD5, uid[5]) ------>|
    |<- ACK (0xC2, seq=5) ------------|
    |                                 |
    | [INITIATOR → IR_RX_UID]         | [RESPONDER → IR_TX_UID]
    |                                 |
    |<- DATA[0] (0xD0, uid[0]) -------|
    |-- ACK (0xC2, seq=0) ----------->|
    | ... (bytes 1–5) ...             |
    |<- DATA[5] (0xD5, uid[5]) -------|
    |-- ACK (0xC2, seq=5) ----------->|
    |                                 |
    | [both → IR_PAIR_CONSENT]        |
```

---

## Timing Parameters

| Constant | Value | Meaning |
|----------|-------|---------|
| `IR_SYN_RETRY_MS` | 300 ms | SYN retransmit interval |
| `IR_SYN_MAX_RETRIES` | 10 | Give up after 10 SYNs (~3s) |
| `IR_SYNACK_TIMEOUT_MS` | 400 ms | Wait for ACK after sending SYN_ACK |
| `IR_SYNACK_MAX_RETRIES` | 5 | SYN_ACK retransmit limit |
| `IR_ACK_TIMEOUT_MS` | 250 ms | Wait for DATA ACK |
| `IR_ACK_MAX_RETRIES` | 3 | Retries per byte before RST |
| `IR_DATA_TIMEOUT_MS` | 1500 ms | Wait for next DATA frame in IR_RX_UID |

**NEC frame duration**: ~64ms per frame (38 kHz carrier, NEC timing).

---

## Error Handling

| Condition | Sender Action | Receiver Action |
|-----------|--------------|-----------------|
| ACK timeout (TX_UID) | Retransmit DATA, increment retry | — |
| ACK retries exhausted | Send RST; enter IR_PAIR_FAILED | — |
| DATA timeout (RX_UID) | — | Send RST; enter IR_PAIR_FAILED |
| RST received (any state) | Enter IR_PAIR_FAILED | Enter IR_PAIR_FAILED |
| NACK received | Retransmit seq in NACK.cmd | — |
| BTN_RIGHT (pre-handshake) | Enter IR_PAIR_CANCELLED | — |
| Screen exit (any state) | IrReceiver.end(); reset to IR_IDLE | — |

---

## Implementation Constants (BadgeIR.h)

```cpp
#define IR_SYN       0xC0
#define IR_SYN_ACK   0xC1
#define IR_ACK       0xC2
#define IR_NACK      0xC4
#define IR_FIN       0xC5
#define IR_RST       0xC6
#define IR_DATA_BASE 0xD0   // addr = IR_DATA_BASE | seq (0–5)

#define IR_SYN_RETRY_MS       300
#define IR_SYN_MAX_RETRIES    10
#define IR_SYNACK_TIMEOUT_MS  400
#define IR_SYNACK_MAX_RETRIES 5
#define IR_ACK_TIMEOUT_MS     250
#define IR_ACK_MAX_RETRIES    3
#define IR_DATA_TIMEOUT_MS   1500
```

---

## Invariants

- Only one badge transmits DATA at a time (half-duplex enforced by role + state machine)
- UID is exactly 6 bytes; DATA seq values are always 0–5
- Cookies must be non-zero (random in range 1–255); zero is reserved/invalid
- RST from either side always results in IR_PAIR_FAILED on both sides
- IR hardware (IrReceiver + IrSender) is active only during `MODE_MAIN` (Boop screen)
