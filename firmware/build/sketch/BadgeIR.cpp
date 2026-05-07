#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeIR.cpp"
// BadgeIR.cpp — IR task (Core 0), TCP-IR protocol, pairing orchestration

#include "BadgeIR.h"
#include "BadgeConfig.h"
#include "BadgeDisplay.h"
#include "BadgeAPI.h"

#include <IRremote.hpp>

// ─── Globals (declared extern in BadgeIR.h) ───────────────────────────────────
IrStatus irStatus;
volatile bool irHardwareEnabled    = false;
volatile bool irExchangeActive     = false;
volatile bool boopEngaged          = false;
volatile bool pairingCancelRequested = false;

// Python IR receive queue (Core 0 writes, Core 1 reads)
volatile bool   pythonIrListening                    = false;
IrPythonFrame   irPythonQueue[IR_PYTHON_QUEUE_SIZE]  = {};
volatile int    irPythonQueueHead                    = 0;
volatile int    irPythonQueueTail                    = 0;
portMUX_TYPE    irPythonQueueMux                     = portMUX_INITIALIZER_UNLOCKED;

// Externs from BadgeDisplay
extern bool          screenDirty;
extern RenderMode    renderMode;
extern unsigned long boopResultShownAt;

// boopTaskDone is owned by the .ino and signalled here so loop() shows result
extern volatile bool boopTaskDone;

// ─── Helpers ──────────────────────────────────────────────────────────────────

void irSetPhase(IrPhase p, unsigned long holdMs) {
  irStatus.phase      = p;
  irStatus.phaseUntil = holdMs ? millis() + holdMs : 0;
  screenDirty = true;
}

// Push one frame into the Python IR queue (called from Core 0 irTask).
// Drops the oldest frame on overflow — never blocks.
static void irPythonQueuePush(uint8_t addr, uint8_t cmd) {
  taskENTER_CRITICAL(&irPythonQueueMux);
  int next = (irPythonQueueTail + 1) % IR_PYTHON_QUEUE_SIZE;
  if (next == irPythonQueueHead) {
    irPythonQueueHead = (irPythonQueueHead + 1) % IR_PYTHON_QUEUE_SIZE;
  }
  irPythonQueue[irPythonQueueTail].addr = addr;
  irPythonQueue[irPythonQueueTail].cmd  = cmd;
  irPythonQueueTail = next;
  taskEXIT_CRITICAL(&irPythonQueueMux);
}

// ─── Externs needed from BadgeUID ────────────────────────────────────────────
extern uint8_t uid[];
extern char    uid_hex[];

// ─── irTask — Core 0, TCP-IR protocol state machine ──────────────────────────

void irTask(void* pvParameters) {
  Serial.println("IR task started on Core 0");

  // ── Local state ────────────────────────────────────────────────────────────
  bool          hwEnabled       = false;
  IrRole        role            = IR_ROLE_NONE;
  uint8_t       myCookie        = 0;
  uint8_t       peerCookie      = 0;
  int           synRetries      = 0;
  int           synAckRetries   = 0;
  int           ackRetries      = 0;
  unsigned long retryAt         = 0;
  unsigned long dataTimer       = 0;
  int           txSeq           = 0;
  int           rxSeq           = 0;
  uint8_t       rxUidBytes[6]   = {};

  // ── sendFrame: stop RX, transmit one NEC frame, restart RX ─────────────────
  auto sendFrame = [](uint8_t addr, uint8_t cmd) {
    IrReceiver.stop();
    IrSender.sendNEC(addr, cmd, 0);
    delay(70);   // NEC frame ~67 ms + margin
    IrReceiver.begin(IR_RX_PIN, DISABLE_LED_FEEDBACK);
    Serial.printf("IR TX: addr=0x%02x cmd=0x%02x\n", addr, cmd);
  };

  // ── setStatus: update live status string on display ─────────────────────────
  auto setStatus = [](const char* msg) {
    strncpy(irStatus.statusMsg, msg, 47);
    irStatus.statusMsg[47] = '\0';
    screenDirty = true;
  };

  // ── goFailed / goCancelled: transition to terminal failed/cancelled state ───
  auto goFailed = [&]() {
    irExchangeActive     = false;
    irStatus.phase       = IR_PAIR_FAILED;
    irStatus.statusMsg[0]= '\0';
    boopTaskDone         = true;
    screenDirty          = true;
    Serial.println("IR: PAIR_FAILED");
  };

  auto goCancelled = [&]() {
    irExchangeActive     = false;
    irStatus.phase       = IR_PAIR_CANCELLED;
    irStatus.statusMsg[0]= '\0';
    boopTaskDone         = true;
    screenDirty          = true;
    Serial.println("IR: PAIR_CANCELLED");
  };

  // ── determineRole: lower cookie = INITIATOR; equal → both become INITIATOR ──
  // On equal cookies both will TX first → collision → timeout → PAIR_FAILED
  // → user retries with new random cookies (P=1/255 of recurrence).
  auto determineRole = [&]() -> IrRole {
    if (myCookie < peerCookie) return IR_INITIATOR;
    if (myCookie > peerCookie) return IR_RESPONDER;
    return IR_INITIATOR;  // tie — both become INITIATOR; retry handles it
  };

  for (;;) {
    vTaskDelay(1);

    unsigned long now = millis();

    // ── Hardware lifecycle ───────────────────────────────────────────────────
    bool wantEnabled = irHardwareEnabled;

    if (wantEnabled && !hwEnabled) {
      // Rising edge — enable hardware, full state reset
      IrSender.begin(IR_TX_PIN);
      IrReceiver.begin(IR_RX_PIN, DISABLE_LED_FEEDBACK);
      hwEnabled = true;
      // Reset all state
      role           = IR_ROLE_NONE;
      myCookie       = peerCookie = 0;
      synRetries     = synAckRetries = ackRetries = 0;
      retryAt        = dataTimer = 0;
      txSeq          = rxSeq = 0;
      memset(rxUidBytes, 0, sizeof(rxUidBytes));
      irExchangeActive              = false;
      boopEngaged                   = false;
      irStatus.phase                = IR_IDLE;
      irStatus.role                 = IR_ROLE_NONE;
      irStatus.my_cookie            = 0;
      irStatus.peer_cookie          = 0;
      irStatus.peerUID[0]           = '\0';
      irStatus.peerUidHex[0]        = '\0';
      irStatus.peerName[0]          = '\0';
      irStatus.statusMsg[0]         = '\0';
      screenDirty                   = true;
      Serial.println("IR: hardware enabled");

    } else if (!wantEnabled && hwEnabled) {
      // Falling edge — disable hardware; preserve irStatus.phase for result display
      IrReceiver.end();
      hwEnabled      = false;
      irExchangeActive = false;
      boopEngaged    = false;
      role           = IR_ROLE_NONE;
      myCookie       = peerCookie = 0;
      synRetries     = synAckRetries = ackRetries = 0;
      retryAt        = dataTimer = 0;
      txSeq          = rxSeq = 0;
      memset(rxUidBytes, 0, sizeof(rxUidBytes));
      Serial.println("IR: hardware disabled");
    }

    if (!hwEnabled) {
      continue;
    }

    // ── Read one incoming NEC frame ──────────────────────────────────────────
    bool    gotFrame = false;
    uint8_t rxAddr   = 0;
    uint8_t rxCmd    = 0;
    if (IrReceiver.decode()) {
      rxAddr   = (uint8_t)IrReceiver.decodedIRData.address;
      rxCmd    = (uint8_t)IrReceiver.decodedIRData.command;
      gotFrame = true;
      Serial.printf("IR RX: addr=0x%02x cmd=0x%02x phase=%d\n",
                    rxAddr, rxCmd, (int)irStatus.phase);
      IrReceiver.resume();
    }

    // RST in any active (non-idle, non-terminal) state → PAIR_FAILED
    if (gotFrame && rxAddr == IR_RST) {
      IrPhase p = irStatus.phase;
      if (p != IR_IDLE && p != IR_PAIRED_OK &&
          p != IR_PAIR_FAILED && p != IR_PAIR_CANCELLED) {
        Serial.println("IR: RST received → PAIR_FAILED");
        goFailed();
        continue;
      }
    }

    // ── State machine ────────────────────────────────────────────────────────
    switch (irStatus.phase) {

    // ── IR_IDLE ──────────────────────────────────────────────────────────────
    case IR_IDLE:
      // Forward frames to Python queue when a Python app has IR listening armed
      if (gotFrame && pythonIrListening) {
        irPythonQueuePush(rxAddr, rxCmd);
        gotFrame = false;
      }

      // Incoming SYN → passive responder path
      if (gotFrame && rxAddr == IR_SYN && rxCmd != 0) {
        peerCookie           = rxCmd;
        myCookie             = (uint8_t)random(1, 256);
        irStatus.my_cookie   = myCookie;
        irStatus.peer_cookie = peerCookie;
        char msg[48];
        snprintf(msg, sizeof(msg), "SYN rx cookie=0x%02x", peerCookie);
        setStatus(msg);
        delay(150);  // wait for SYN sender's receiver to restart after its SYN TX (~137ms off-time)
        sendFrame(IR_SYN_ACK, myCookie);
        irStatus.phase   = IR_SYN_RECEIVED;
        synAckRetries    = 0;
        retryAt          = millis() + IR_SYNACK_TIMEOUT_MS;  // millis() post-TX: full 400ms window from when receiver is up
        screenDirty      = true;
        Serial.printf("IR: SYN rx cookie=0x%02x → SYN_RECEIVED\n", peerCookie);
        break;
      }

      // boopEngaged edge → initiator path (consumed here; not held)
      if (boopEngaged) {
        boopEngaged          = false;
        myCookie             = (uint8_t)random(1, 256);
        irStatus.my_cookie   = myCookie;
        char msg[48];
        snprintf(msg, sizeof(msg), "SYN tx cookie=0x%02x", myCookie);
        setStatus(msg);
        sendFrame(IR_SYN, myCookie);
        irStatus.phase = IR_SYN_SENT;
        synRetries     = 0;
        retryAt        = now + IR_SYN_RETRY_MS;
        screenDirty    = true;
        Serial.printf("IR: boopEngaged → SYN_SENT cookie=0x%02x\n", myCookie);
      }
      break;

    // ── IR_SYN_SENT ──────────────────────────────────────────────────────────
    case IR_SYN_SENT:
      // Cancel — BTN_RIGHT pressed pre-handshake
      if (pairingCancelRequested) {
        pairingCancelRequested = false;
        sendFrame(IR_RST, 0x00);
        goCancelled();
        break;
      }

      if (gotFrame) {
        // Simultaneous SYN (both pressed UP): send SYN_ACK, stay in SYN_SENT
        if (rxAddr == IR_SYN && rxCmd != 0) {
          peerCookie           = rxCmd;
          irStatus.peer_cookie = peerCookie;
          delay(150);  // wait for peer's receiver to restart after its SYN TX
          sendFrame(IR_SYN_ACK, myCookie);
          char msg[48];
          snprintf(msg, sizeof(msg), "simul SYN, peer=0x%02x", peerCookie);
          setStatus(msg);
          Serial.printf("IR: simultaneous SYN, peer=0x%02x — sent SYN_ACK\n", peerCookie);
          break;
        }

        // SYN_ACK received — complete handshake
        if (rxAddr == IR_SYN_ACK) {
          peerCookie           = rxCmd;
          irStatus.peer_cookie = peerCookie;
          // Wait for responder's receiver to restart after its SYN_ACK TX.
          // sendFrame() stops the receiver for ~137ms (67ms NEC + 70ms delay);
          // if we send ACK immediately the responder misses it.
          delay(150);
          sendFrame(IR_ACK, 0);
          role              = determineRole();
          irStatus.role     = role;
          irExchangeActive  = true;
          irStatus.phase    = IR_ESTABLISHED;
          screenDirty       = true;
          Serial.printf("IR: SYN_ACK rx → ESTABLISHED role=%s\n",
                        role == IR_INITIATOR ? "INIT" : "RESP");
          break;
        }
      }

      // SYN retry
      if (now >= retryAt) {
        synRetries++;
        if (synRetries > IR_SYN_MAX_RETRIES) {
          Serial.println("IR: SYN retries exhausted → PAIR_FAILED");
          goFailed();
          break;
        }
        char msg[48];
        snprintf(msg, sizeof(msg), "SYN retry %d/%d", synRetries, IR_SYN_MAX_RETRIES);
        setStatus(msg);
        sendFrame(IR_SYN, myCookie);
        retryAt = now + IR_SYN_RETRY_MS;
      }
      break;

    // ── IR_SYN_RECEIVED ──────────────────────────────────────────────────────
    case IR_SYN_RECEIVED:
      // ACK from initiator — complete handshake
      if (gotFrame && rxAddr == IR_ACK) {
        role             = determineRole();
        irStatus.role    = role;
        irExchangeActive = true;
        irStatus.phase   = IR_ESTABLISHED;
        screenDirty      = true;
        Serial.printf("IR: ACK rx → ESTABLISHED role=%s\n",
                      role == IR_INITIATOR ? "INIT" : "RESP");
        break;
      }

      // SYN_ACK retry
      if (now >= retryAt) {
        synAckRetries++;
        if (synAckRetries > IR_SYNACK_MAX_RETRIES) {
          Serial.println("IR: SYN_ACK retries exhausted → PAIR_FAILED");
          goFailed();
          break;
        }
        char msg[48];
        snprintf(msg, sizeof(msg), "SYN_ACK retry %d/%d", synAckRetries, IR_SYNACK_MAX_RETRIES);
        setStatus(msg);
        sendFrame(IR_SYN_ACK, myCookie);
        retryAt = millis() + IR_SYNACK_TIMEOUT_MS;
      }
      break;

    // ── IR_ESTABLISHED ────────────────────────────────────────────────────────
    case IR_ESTABLISHED:
      if (role == IR_INITIATOR) {
        // INITIATOR sends UID first
        txSeq = 0;
        char msg[48];
        snprintf(msg, sizeof(msg), "TX[0]=0x%02x", uid[0]);
        setStatus(msg);
        sendFrame(IR_DATA_BASE | txSeq, uid[txSeq]);
        ackRetries     = 0;
        retryAt        = millis() + IR_ACK_TIMEOUT_MS;  // millis() post-TX so window starts when receiver is up
        irStatus.phase = IR_TX_UID;
        screenDirty    = true;
      } else {
        // RESPONDER waits for INITIATOR's UID first
        rxSeq          = 0;
        dataTimer      = now;
        irStatus.peerUidHex[0] = '\0';
        irStatus.phase = IR_RX_UID;
        setStatus("waiting for DATA");
        screenDirty    = true;
      }
      break;

    // ── IR_TX_UID ─────────────────────────────────────────────────────────────
    case IR_TX_UID:
      if (gotFrame) {
        if (rxAddr == IR_ACK && rxCmd == (uint8_t)txSeq) {
          // Byte acknowledged — advance sequence
          txSeq++;
          if (txSeq == 6) {
            // All 6 UID bytes sent
            if (role == IR_INITIATOR) {
              // INITIATOR: now receive RESPONDER's UID
              rxSeq          = 0;
              dataTimer      = now;
              irStatus.peerUidHex[0] = '\0';
              irStatus.phase = IR_RX_UID;
              setStatus("UID sent, receiving...");
              screenDirty    = true;
            } else {
              // RESPONDER: both UIDs exchanged — consent
              irStatus.phase = IR_PAIR_CONSENT;
              setStatus(irStatus.peerUID);
              screenDirty    = true;
            }
            break;
          }
          // Send next byte
          char msg[48];
          snprintf(msg, sizeof(msg), "TX[%d]=0x%02x", txSeq, uid[txSeq]);
          setStatus(msg);
          sendFrame(IR_DATA_BASE | txSeq, uid[txSeq]);
          ackRetries = 0;
          retryAt    = millis() + IR_ACK_TIMEOUT_MS;
        } else if (rxAddr == IR_NACK) {
          // NACK — retransmit current byte immediately
          char msg[48];
          snprintf(msg, sizeof(msg), "NACK, re-TX[%d]", txSeq);
          setStatus(msg);
          sendFrame(IR_DATA_BASE | txSeq, uid[txSeq]);
          ackRetries = 0;
          retryAt    = millis() + IR_ACK_TIMEOUT_MS;
        }
        break;
      }

      // ACK timeout
      if (now >= retryAt) {
        ackRetries++;
        if (ackRetries > IR_ACK_MAX_RETRIES) {
          sendFrame(IR_RST, 0x00);
          Serial.println("IR: TX ACK retries exhausted → PAIR_FAILED");
          goFailed();
          break;
        }
        char msg[48];
        snprintf(msg, sizeof(msg), "ACK timeout retry %d", ackRetries);
        setStatus(msg);
        sendFrame(IR_DATA_BASE | txSeq, uid[txSeq]);
        retryAt = millis() + IR_ACK_TIMEOUT_MS;
      }
      break;

    // ── IR_RX_UID ─────────────────────────────────────────────────────────────
    case IR_RX_UID:
      if (gotFrame) {
        uint8_t expectedAddr = IR_DATA_BASE | (uint8_t)rxSeq;

        if (rxAddr == expectedAddr) {
          // Expected data byte — store and ACK
          rxUidBytes[rxSeq] = rxCmd;
          // Fill in peerUidHex live so display can show partial UID
          snprintf(irStatus.peerUidHex + rxSeq * 2, 3, "%02x", rxCmd);
          irStatus.peerUidHex[rxSeq * 2 + 2] = '\0';
          delay(150);  // wait for sender's receiver to restart after its DATA TX (~137ms off-time)
          sendFrame(IR_ACK, (uint8_t)rxSeq);
          dataTimer = now;
          char msg[48];
          snprintf(msg, sizeof(msg), "RX[%d]=0x%02x", rxSeq, rxCmd);
          setStatus(msg);
          rxSeq++;
          screenDirty = true;

          if (rxSeq == 6) {
            // All 6 bytes received — build final peer UID hex string
            char hexBuf[13];
            for (int i = 0; i < 6; i++) snprintf(hexBuf + i * 2, 3, "%02x", rxUidBytes[i]);
            hexBuf[12] = '\0';
            strncpy(irStatus.peerUidHex, hexBuf, 13);
            strncpy(irStatus.peerUID,    hexBuf, 13);
            Serial.printf("IR: full peer UID = %s\n", hexBuf);

            if (role == IR_RESPONDER) {
              // RESPONDER received INITIATOR's UID — now TX our UID
              txSeq          = 0;
              char msg2[48];
              snprintf(msg2, sizeof(msg2), "TX[0]=0x%02x", uid[0]);
              setStatus(msg2);
              sendFrame(IR_DATA_BASE | txSeq, uid[txSeq]);
              ackRetries     = 0;
              retryAt        = millis() + IR_ACK_TIMEOUT_MS;
              irStatus.phase = IR_TX_UID;
              screenDirty    = true;
            } else {
              // INITIATOR received RESPONDER's UID — both done; consent
              irStatus.phase = IR_PAIR_CONSENT;
              setStatus(irStatus.peerUID);
              screenDirty    = true;
            }
          }
          break;
        }

        // Duplicate DATA frame (same seq as last ACK'd) — resend ACK
        if (rxSeq > 0 && rxAddr == (IR_DATA_BASE | (uint8_t)(rxSeq - 1))) {
          delay(150);
          sendFrame(IR_ACK, (uint8_t)(rxSeq - 1));
          break;
        }
      }

      // Data frame timeout
      if (now - dataTimer > IR_DATA_TIMEOUT_MS) {
        sendFrame(IR_RST, 0x00);
        Serial.println("IR: data timeout → PAIR_FAILED");
        goFailed();
      }
      break;

    // ── IR_PAIR_CONSENT ───────────────────────────────────────────────────────
    case IR_PAIR_CONSENT: {
      Serial.printf("IR: createBoop peer=%s\n", irStatus.peerUID);
      BoopResult r = BadgeAPI::createBoop(uid_hex, irStatus.peerUID);
      Serial.printf("IR: createBoop -> ok=%d code=%d\n", r.ok, r.httpCode);

      if (r.ok) {
        if (r.partnerName[0] != '\0') {
          strncpy(irStatus.peerName, r.partnerName, 31);
          irStatus.peerName[31] = '\0';
        }
        irExchangeActive = false;
        irStatus.phase   = IR_PAIRED_OK;
        boopTaskDone     = true;
        screenDirty      = true;
        Serial.println("IR: PAIRED_OK");
      } else {
        goFailed();
      }
      break;
    }

    // ── Terminal states — wait for hardware disable (irHardwareEnabled → false)
    case IR_PAIRED_OK:
    case IR_PAIR_FAILED:
    case IR_PAIR_CANCELLED:
      // loop() detects boopTaskDone → sets renderMode = MODE_BOOP_RESULT
      // which clears irHardwareEnabled → hardware lifecycle resets state
      break;

    default:
      break;
    }
  }
}

// ─── Python IR bridge ─────────────────────────────────────────────────────────
// Called from BadgePython_bridges.cpp (badge_bridge_ir_send) via C linkage.
// Defined here to avoid including <IRremote.hpp> in BadgePython_bridges.cpp,
// which would cause multiple-definition linker errors.

extern "C" int badge_bridge_ir_send(int addr, int cmd) {
    IrSender.begin(IR_TX_PIN);
    IrSender.sendNEC((uint16_t)addr, (uint8_t)cmd, 0);
    return 0;
}
