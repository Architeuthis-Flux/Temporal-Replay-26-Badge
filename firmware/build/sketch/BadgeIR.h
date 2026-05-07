#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeIR.h"
// BadgeIR.h — Public interface for the BadgeIR module
// See FR-003 in spec.md
//
// Contains: IrPhase/IrRole enums, TCP-IR packet & timing constants,
//           IrStatus struct, irStatus global, irSetPhase helper,
//           irTask (Core 0 FreeRTOS task),
//           Python IR receive queue globals and IrPythonFrame struct,
//           BADGE_IR_ADDR and ROLE_NUM_* constants.
//
// Inter-core contract (must not change):
//   irHardwareEnabled, irExchangeActive, boopEngaged, pairingCancelRequested
//   are volatile bool. Written by Core 1, read/cleared by Core 0 (irTask).
//   No mutex needed — each is a single-byte atomic write on ESP32.
//   irPythonQueue head/tail are protected by irPythonQueueMux (portMUX spinlock).

#pragma once
#include <Arduino.h>

// ─── IR Protocol constants (legacy NEC addresses — kept for reference) ────────

// Role number constants — attendee type stored in NVS and used by BadgeAPI/BadgeStorage/BadgePairing.
// These are NOT IR protocol values.
#define ROLE_NUM_ATTENDEE  1
#define ROLE_NUM_STAFF     2
#define ROLE_NUM_VENDOR    3
#define ROLE_NUM_SPEAKER   4

// ─── TCP-IR packet type constants ─────────────────────────────────────────────
// NEC frame: addr (8-bit) = packet type, cmd (8-bit) = payload

#define IR_SYN       0xC0   // cmd = my_cookie (random 1–255)
#define IR_SYN_ACK   0xC1   // cmd = my_cookie
#define IR_ACK       0xC2   // cmd = seq being acknowledged (0–5)
#define IR_NACK      0xC4   // cmd = seq expected
#define IR_FIN       0xC5   // cmd = 0x00 (reserved, not used in v1)
#define IR_RST       0xC6   // cmd = 0x00
#define IR_DATA_BASE 0xD0   // DATA: addr = 0xD0|seq (0xD0–0xD5), cmd = UID byte

// ─── TCP-IR timing constants ──────────────────────────────────────────────────

#define IR_SYN_RETRY_MS       300   // SYN retransmit interval (ms)
#define IR_SYN_MAX_RETRIES    10    // give up after 10 SYNs (~3 s)
#define IR_SYNACK_TIMEOUT_MS  400   // wait for ACK after sending SYN_ACK
#define IR_SYNACK_MAX_RETRIES 5
#define IR_ACK_TIMEOUT_MS     250   // wait for DATA ACK
#define IR_ACK_MAX_RETRIES    3     // 3 retries per byte before RST
#define IR_DATA_TIMEOUT_MS   1500   // wait for next DATA frame in IR_RX_UID

// ─── IrRole ───────────────────────────────────────────────────────────────────

enum IrRole {
    IR_ROLE_NONE,
    IR_INITIATOR,   // lower cookie wins; sends UID first
    IR_RESPONDER,   // higher cookie; receives UID first
};

// ─── IrPhase ─────────────────────────────────────────────────────────────────

enum IrPhase {
    IR_IDLE,
    IR_SYN_SENT,
    IR_SYN_RECEIVED,
    IR_ESTABLISHED,
    IR_TX_UID,
    IR_RX_UID,
    IR_PAIR_CONSENT,
    IR_PAIRED_OK,
    IR_PAIR_FAILED,
    IR_PAIR_CANCELLED,
};

// ─── IrStatus ────────────────────────────────────────────────────────────────

struct IrStatus {
    volatile IrPhase phase;
    IrRole           role;
    char             peerUID[13];       // final peer UID (set on completion)
    char             peerUidHex[13];    // working buffer filled in during RX_UID
    char             peerName[32];
    char             statusMsg[48];     // live protocol status for display
    uint8_t          my_cookie;
    uint8_t          peer_cookie;
    unsigned long    phaseUntil;
};

extern IrStatus irStatus;

// ─── IR hardware lifecycle flags (Core 1 writes, Core 0 reads) ───────────────

extern volatile bool irHardwareEnabled;   // true while Boop screen (MODE_BOOP) is active
extern volatile bool irExchangeActive;    // true from IR_ESTABLISHED to terminal state

// ─── Python IR receive queue ──────────────────────────────────────────────────
// Core 0 (irTask) writes; Core 1 (MicroPython bridge) reads.

#define IR_PYTHON_QUEUE_SIZE 8

struct IrPythonFrame {
    uint8_t addr;
    uint8_t cmd;
};

extern volatile bool      pythonIrListening;
extern IrPythonFrame      irPythonQueue[IR_PYTHON_QUEUE_SIZE];
extern volatile int       irPythonQueueHead;
extern volatile int       irPythonQueueTail;
extern portMUX_TYPE       irPythonQueueMux;

// ─── Inter-core communication flags (volatile — Core 1 writes, Core 0 reads) ─

extern volatile bool boopEngaged;          // edge trigger — consumed by irTask on SYN fire
extern volatile bool pairingCancelRequested;

// ─── Helpers ─────────────────────────────────────────────────────────────────

void irSetPhase(IrPhase p, unsigned long holdMs = 0);

// ─── FreeRTOS task (Core 0) ───────────────────────────────────────────────────

// Launch via: xTaskCreatePinnedToCore(irTask, "IR", 8192, NULL, 1, NULL, 0);
void irTask(void* pvParameters);
