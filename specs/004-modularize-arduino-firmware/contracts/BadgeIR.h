// BadgeIR.h — Public interface for the BadgeIR module
// See FR-003 in spec.md
//
// Contains: IrPhase enum, IrStatus struct, irStatus global,
//           irSetPhase/irSetPeer/irSetDebug helpers,
//           irTask (Core 0 FreeRTOS task), submitPairing.
//           IR protocol constants and macros.
//
// Inter-core contract (must not change):
//   boopListening, irPairingRequested, pairingCancelRequested are volatile bool.
//   Written by Core 1 (BadgeInput / main loop), read/cleared by Core 0 (irTask).
//   No mutex needed — each is a single-byte atomic write on ESP32.

#pragma once
#include <Arduino.h>

// ─── IR Protocol constants ────────────────────────────────────────────────────

#define ROLE_ATTENDEE   0x10
#define ROLE_STAFF      0x20
#define ROLE_VENDOR     0x30
#define ROLE_SPEAKER    0x40

#define PKT_UID         0x01
#define PKT_PING        0x02
#define PKT_MSG         0x03
#define PKT_ACK         0x04

#define IR_ADDR(role, pkt)  ((role) | (pkt))
#define IR_GET_ROLE(addr)   ((addr) & 0xF0)
#define IR_GET_PKT(addr)    ((addr) & 0x0F)

// Role number constants (for assignedRole global in main sketch)
#define ROLE_NUM_ATTENDEE  1
#define ROLE_NUM_STAFF     2
#define ROLE_NUM_VENDOR    3
#define ROLE_NUM_SPEAKER   4

// ─── IrPhase ─────────────────────────────────────────────────────────────────

enum IrPhase {
    IR_IDLE,
    IR_LISTENING,
    IR_SENDING,
    IR_WAITING,
    IR_INCOMING,
    IR_NO_REPLY,
    IR_PAIR_CONSENT,
    IR_PAIRED_OK,
    IR_PAIR_IGNORED,
    IR_PAIR_FAILED,
    IR_PAIR_CANCELLED,
    IR_UNAVAIL
};

// ─── IrStatus ────────────────────────────────────────────────────────────────

struct IrStatus {
    volatile IrPhase phase;
    char             peerUID[13];
    char             peerRole[12];
    char             peerName[32];
    char             debugMsg[24];
    unsigned long    phaseUntil;
};

extern IrStatus irStatus;

// ─── Inter-core communication flags (volatile — Core 1 writes, Core 0 reads) ─

extern volatile bool boopListening;
extern volatile bool irPairingRequested;
extern volatile bool pairingCancelRequested;

// ─── Helpers (Core 0 calls these) ────────────────────────────────────────────

void irSetPhase(IrPhase p, unsigned long holdMs = 0);
void irSetPeer(const char* uidHex, uint8_t addr, const char* name = nullptr);
void irSetDebug(const char* msg);

// ─── Role helpers ─────────────────────────────────────────────────────────────

const char* getRoleName(uint8_t addr);
uint8_t     roleNumToIR(int roleNum);

// ─── FreeRTOS task (Core 0) ───────────────────────────────────────────────────

// Launch via: xTaskCreatePinnedToCore(irTask, "IR", 8192, NULL, 1, NULL, 0);
void irTask(void* pvParameters);

// ─── Pairing orchestrator (called from irTask — thin wrapper around BadgeAPI) ─

// submitPairing calls BadgeAPI::createBoop, getBoopStatus, cancelBoop.
// Runs on Core 0; all HTTP work happens inside BadgeAPI functions.
void submitPairing(const char* theirUID, uint8_t theirAddr);
