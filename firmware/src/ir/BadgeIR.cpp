// BadgeIR.cpp — Pure RMT NEC multi-word transport layer.
//
// Scope of this module:
//   - RMT hardware lifecycle (nec_tx_init / nec_rx_init / teardown)
//   - Blocking frame send/recv with self-echo filtering
//     (BadgeIR::sendFrame / BadgeIR::recvFrame)
//   - MicroPython-facing raw queues + irSendRaw / irSendWords / irReadWords
//   - TX carrier duty (effective LED drive strength)
//   - irTask — the FreeRTOS loop on Core 0 that pumps the transport and
//     dispatches BadgeBoops::smTick() when the Boop screen is up
//
// Everything else lives in BadgeBoops: the phase state machine, field
// codec, beacon framing, journal, and feedback. This file must NOT
// reference BoopPhase / BoopStatus / boopEngaged — those are state.
//
// Runs as irTask on Core 0 (FreeRTOS, 8 KB stack, priority 1).
// IR hardware is only active while BoopScreen sets irHardwareEnabled.

#include "BadgeIR.h"
#include "../boops/BadgeBoops.h"
#include "../identity/BadgeUID.h"
#include "../infra/DebugLog.h"
#include "../hardware/HardwareConfig.h"
#include "hw/ir/nec_tx.h"
#include "hw/ir/nec_rx.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include <cstring>

static const char* TAG = "BadgeIR";

#define APP_RMT_RESOLUTION 1000000U // 1 MHz (1 µs/tick)

// ─── Extern globals from BadgeIR.h ──────────────────────────────────────────

volatile bool    irHardwareEnabled   = false;
volatile bool    pythonIrListening   = false;
IrPythonFrame    irPythonQueue[IR_PYTHON_QUEUE_SIZE] = {};
volatile int     irPythonQueueHead     = 0;
volatile int     irPythonQueueTail     = 0;
portMUX_TYPE     irPythonQueueMux      = portMUX_INITIALIZER_UNLOCKED;

// ─── Module-level state ─────────────────────────────────────────────────────

static nec_tx_context_t* s_tx_ctx = nullptr;
static nec_rx_context_t* s_rx_ctx = nullptr;
static QueueHandle_t    s_frame_queue             = nullptr;
static QueueHandle_t    s_python_tx_queue         = nullptr;
static QueueHandle_t    s_python_rx_words_queue   = nullptr;
static bool             s_hw_up                   = false;
static int              s_tx_power_percent        = 10;

static void irLogHeap(const char* phase) {
    Serial.printf("[%s] heap %s largest=%u free=%u\n",
                  TAG,
                  phase,
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

static void irReleaseContexts() {
    if (s_rx_ctx) {
        heap_caps_free(s_rx_ctx);
        s_rx_ctx = nullptr;
    }
    if (s_tx_ctx) {
        heap_caps_free(s_tx_ctx);
        s_tx_ctx = nullptr;
    }
}

static bool irAllocContexts() {
    if (s_tx_ctx && s_rx_ctx) return true;

    if (!s_tx_ctx) {
        s_tx_ctx = static_cast<nec_tx_context_t*>(
            heap_caps_calloc(1, sizeof(nec_tx_context_t),
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    }
    if (!s_rx_ctx) {
        s_rx_ctx = static_cast<nec_rx_context_t*>(
            heap_caps_calloc(1, sizeof(nec_rx_context_t),
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    }

    if (!s_tx_ctx || !s_rx_ctx) {
        ESP_LOGE(TAG, "IR context allocation failed (tx=%p rx=%p)",
                 (void*)s_tx_ctx, (void*)s_rx_ctx);
        irReleaseContexts();
        return false;
    }

    return true;
}

// ─── Self-echo filter ───────────────────────────────────────────────────────
// Every v2 protocol frame carries our UID in words[1..2] when we TX it.
// Detect those reflections and silently drop them.

static bool isSelfEcho(const nec_mw_result_t* frame) {
    if (frame == nullptr || frame->count == 0) return false;

    const uint8_t type = (uint8_t)(frame->words[0] & 0xFF);
    // Frames that carry SENDER UID in words[1..2] — drop if it's our
    // own.  IR_NOTIFY_MANIFEST / _DATA / _NEED follow the boop v2
    // convention (sender-in-envelope) instead of single-frame
    // IR_NOTIFY's target-in-envelope, because the streaming protocol
    // has no free words for a target UID alongside the sender UID and
    // per-stream metadata.  Target-side filtering for those frames
    // lives in the dispatch layer (see notifyTick below).
    const bool senderUidCarrying =
        (type == IR_BOOP_BEACON)      ||
        (type == IR_BOOP_DONE)        ||
        (type == IR_BOOP_MANIFEST)    ||
        (type == IR_BOOP_STREAM_REQ)  ||
        (type == IR_BOOP_DATA)        ||
        (type == IR_BOOP_NEED)        ||
        (type == IR_BOOP_FIN)         ||
        (type == IR_BOOP_FINACK)      ||
        (type == IR_NOTIFY_MANIFEST)  ||
        (type == IR_NOTIFY_DATA)      ||
        (type == IR_NOTIFY_NEED);
    if (senderUidCarrying && frame->count >= 3) {
        const uint32_t lo = frame->words[1];
        const uint32_t hi = frame->words[2];
        const uint8_t bytes[6] = {
            (uint8_t)( lo        & 0xFF),
            (uint8_t)((lo >>  8) & 0xFF),
            (uint8_t)((lo >> 16) & 0xFF),
            (uint8_t)((lo >> 24) & 0xFF),
            (uint8_t)( hi        & 0xFF),
            (uint8_t)((hi >>  8) & 0xFF),
        };
        if (memcmp(bytes, uid, 6) == 0) return true;
    }
    // IR_NOTIFY and IR_NOTIFY_ACK both carry the TARGET UID in
    // words[1..2] (opposite of beacon/DATA which carry the SENDER UID
    // there).  "Not addressed to us" is morally the same as a self-
    // echo from the dispatcher's perspective: drop it here so
    // recvFrame() keeps walking, callers get nothing they can't use.
    if ((type == IR_NOTIFY || type == IR_NOTIFY_ACK) && frame->count >= 3) {
        const uint32_t lo = frame->words[1];
        const uint32_t hi = frame->words[2];
        const uint8_t targetBytes[6] = {
            (uint8_t)( lo        & 0xFF),
            (uint8_t)((lo >>  8) & 0xFF),
            (uint8_t)((lo >> 16) & 0xFF),
            (uint8_t)((lo >> 24) & 0xFF),
            (uint8_t)( hi        & 0xFF),
            (uint8_t)((hi >>  8) & 0xFF),
        };
        if (memcmp(targetBytes, uid, 6) != 0) return true;
    }
    return false;
}

// ─── Transport API (called from BadgeBoops state machine on irTask) ─────────

namespace BadgeIR {

bool sendFrame(const uint32_t* words, size_t count) {
    if (!s_hw_up || s_tx_ctx == nullptr || s_tx_ctx->ask_to_notify == nullptr) return false;
    // No pre-TX queue drain — self-echoes are filtered in recvFrame, and
    // draining here would discard peer beacons that arrived just before
    // our TX slot.
    esp_err_t ret = nec_tx_send(s_tx_ctx, words, count, 3000);
    if (ret == ESP_OK) {
        nec_tx_wait(s_tx_ctx, 3000);
    }
    return (ret == ESP_OK);
}

bool sendFrameNoWait(const uint32_t* words, size_t count) {
    // With trans_queue_depth=1, the nowait path can't actually pipeline —
    // a second frame would fail with ESP_ERR_INVALID_STATE.  Fall through
    // to blocking sendFrame for reliability.  Callers already handle the
    // false-return fallback, so the effective behavior is identical.
    return sendFrame(words, count);
}

bool recvFrame(nec_mw_result_t* out, uint32_t timeout_ms) {
    if (!s_hw_up || s_frame_queue == nullptr || out == nullptr) return false;

    // Special case timeout_ms=0 → non-blocking poll.  The old loop-with-
    // deadline path computed `deadline - now <= 0` on the first iteration
    // and returned false without ever calling xQueueReceive, silently
    // turning every "drain one frame" caller into a no-op.  v2's
    // peer_tickPostConfirm relies on this path to drain incoming
    // MANIFEST / DATA / NEED / FIN frames, so the bug wedged the whole
    // exchange phase.  Handle timeout=0 explicitly as a tick-0 poll.
    if (timeout_ms == 0) {
        for (;;) {
            if (xQueueReceive(s_frame_queue, out, 0) != pdPASS) return false;
            if (isSelfEcho(out)) {
                Serial.printf("[%s] self-echo dropped (type=0x%02X w=%u)\n",
                              TAG,
                              (uint8_t)(out->words[0] & 0xFF),
                              (unsigned)out->count);
                continue;
            }
            return true;
        }
    }

    const TickType_t start    = xTaskGetTickCount();
    const TickType_t deadline = start + pdMS_TO_TICKS(timeout_ms);

    for (;;) {
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) return false;
        const TickType_t remaining = deadline - now;

        if (xQueueReceive(s_frame_queue, out, remaining) != pdPASS) return false;

        if (isSelfEcho(out)) {
            LOG_IR("[%s] self-echo dropped (type=0x%02X w=%u)\n",
                   TAG,
                   (uint8_t)(out->words[0] & 0xFF),
                   (unsigned)out->count);
            continue;
        }

        return true;
    }
}

}  // namespace BadgeIR

// ─── RMT RX callback (called from nec_rx decode task) ───────────────────────
//
// Unconditional log — temporarily promoted from LOG_IR while debugging
// the v3 pingpong "no RX in EXCHANGE phase" issue. At v3's 1.5s retx
// cadence the rate stays low enough not to flood the CDC ring.
static void on_frame_rx(const nec_mw_result_t* result, void* /*user_data*/) {
    if (result == nullptr) return;
    Serial.printf("[%s] rxcb: type=0x%02X w=%u crc=%d\n",
                  TAG,
                  (uint8_t)(result->words[0] & 0xFF),
                  (unsigned)result->count,
                  (int)result->crc_ok);
    if (result->crc_ok && s_frame_queue != nullptr) {
        if (xQueueSend(s_frame_queue, result, 0) != pdPASS) {
            Serial.printf("[%s] rxcb: queue full, dropped type=0x%02X\n",
                          TAG, (uint8_t)(result->words[0] & 0xFF));
        }
    }
}

// ─── Hardware init / deinit ─────────────────────────────────────────────────

static void irDeleteQueues() {
    if (s_frame_queue) {
        vQueueDelete(s_frame_queue);
        s_frame_queue = nullptr;
    }
    if (s_python_tx_queue) {
        vQueueDelete(s_python_tx_queue);
        s_python_tx_queue = nullptr;
    }
    if (s_python_rx_words_queue) {
        vQueueDelete(s_python_rx_words_queue);
        s_python_rx_words_queue = nullptr;
    }
}

static bool irHwInit() {
    if (s_hw_up) return true;

    irLogHeap("before init");
    if (!irAllocContexts()) return false;

    s_frame_queue            = xQueueCreate(4, sizeof(nec_mw_result_t));
    s_python_tx_queue        = xQueueCreate(4, sizeof(ir_tx_request_t));
    s_python_rx_words_queue  = xQueueCreate(2, sizeof(nec_mw_result_t));
    if (!s_frame_queue || !s_python_tx_queue || !s_python_rx_words_queue) {
        ESP_LOGE(TAG, "queue create failed");
        irDeleteQueues();
        irReleaseContexts();
        return false;
    }

    esp_err_t ret = nec_tx_init(s_tx_ctx, (gpio_num_t)IR_TX_PIN, APP_RMT_RESOLUTION);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nec_tx_init failed: %s", esp_err_to_name(ret));
        irDeleteQueues();
        irReleaseContexts();
        return false;
    }
    // Drain any stale task-notify counter so the first tx_wait doesn't
    // spuriously return from a pre-existing notification.
    while (ulTaskNotifyTake(pdTRUE, 0) != 0) { /* consume stale */ }
    xTaskNotifyGive(xTaskGetCurrentTaskHandle());

    ret = nec_rx_init(s_rx_ctx, (gpio_num_t)IR_RX_PIN, APP_RMT_RESOLUTION,
                      on_frame_rx, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nec_rx_init failed: %s", esp_err_to_name(ret));
        nec_tx_deinit(s_tx_ctx);
        irDeleteQueues();
        irReleaseContexts();
        return false;
    }

    s_hw_up = true;

    // Always apply the cached TX power.  The old `!= 50` guard was an
    // optimisation that assumed the RMT init left the carrier at 50 %
    // duty and skipped the apply when the cached value matched — this
    // silently broke any non-50 % default (the cached value has to
    // actually reach the hardware for it to take effect).
    {
        float duty = (float)s_tx_power_percent / 100.0f;
        (void)nec_tx_set_carrier_duty(s_tx_ctx, duty);
    }

    Serial.printf("[%s] RMT hardware initialized (TX power=%d%%)\n",
                  TAG, s_tx_power_percent);
    irLogHeap("after init");
    return true;
}

static void irHwDeinit() {
    if (s_hw_up) {
        if (s_rx_ctx) nec_rx_deinit(s_rx_ctx);
        if (s_tx_ctx) nec_tx_deinit(s_tx_ctx);
    }

    irDeleteQueues();
    s_hw_up = false;
    irReleaseContexts();
    Serial.printf("[%s] RMT hardware deinitialized\n", TAG);
    irLogHeap("after deinit");
}

// ─── Python queue feed ──────────────────────────────────────────────────────
// When nothing on-device is actively using IR, drain any arriving frames
// into the MicroPython-facing queue so ir_read() sees them.

static void feedPythonQueue() {
    if (!pythonIrListening ||
        BadgeBoops::boopStatus.phase != BadgeBoops::BOOP_PHASE_IDLE) {
        return;
    }

    nec_mw_result_t frame;
    while (xQueueReceive(s_frame_queue, &frame, 0) == pdPASS) {
        IrPythonFrame pf;
        pf.addr = (uint8_t)(frame.words[0] & 0xFF);
        pf.cmd  = (uint8_t)((frame.words[0] >> 8) & 0xFF);

        portENTER_CRITICAL(&irPythonQueueMux);
        int next = (irPythonQueueTail + 1) % IR_PYTHON_QUEUE_SIZE;
        if (next != irPythonQueueHead) {
            irPythonQueue[irPythonQueueTail] = pf;
            irPythonQueueTail = next;
        }
        portEXIT_CRITICAL(&irPythonQueueMux);

        if (s_python_rx_words_queue) {
            xQueueSend(s_python_rx_words_queue, &frame, 0);
        }
    }
}

// ─── Service queued Python TX requests ──────────────────────────────────────

static void servicePythonTx() {
    if (!s_hw_up || s_python_tx_queue == nullptr) return;

    ir_tx_request_t req;
    while (xQueueReceive(s_python_tx_queue, &req, 0) == pdPASS) {
        BadgeIR::sendFrame(req.words, req.count);
    }
}

// ─── Public API for MicroPython ─────────────────────────────────────────────

bool irHwIsUp() { return s_hw_up; }

int irSendRaw(uint8_t addr, uint8_t cmd) {
    if (s_python_tx_queue == nullptr) return -1;
    ir_tx_request_t req = {};
    req.words[0] = (uint32_t)addr | ((uint32_t)cmd << 8);
    req.count = 1;
    return (xQueueSend(s_python_tx_queue, &req, pdMS_TO_TICKS(100)) == pdPASS) ? 0 : -1;
}

int irSendWords(const uint32_t* words, size_t count) {
    if (s_python_tx_queue == nullptr || !words || count == 0 || count > NEC_MAX_WORDS)
        return -1;
    ir_tx_request_t req = {};
    memcpy(req.words, words, count * sizeof(uint32_t));
    req.count = count;
    return (xQueueSend(s_python_tx_queue, &req, pdMS_TO_TICKS(200)) == pdPASS) ? 0 : -1;
}

int irReadWords(uint32_t* out, size_t max_words, size_t* count_out) {
    if (s_python_rx_words_queue == nullptr || !out || !count_out) return -1;
    nec_mw_result_t frame;
    if (xQueueReceive(s_python_rx_words_queue, &frame, 0) != pdPASS) return -1;
    size_t n = frame.count;
    if (n > max_words) n = max_words;
    memcpy(out, frame.words, n * sizeof(uint32_t));
    *count_out = n;
    return 0;
}

void irDrainPythonRx() {
    if (s_frame_queue)           xQueueReset(s_frame_queue);
    if (s_python_rx_words_queue) xQueueReset(s_python_rx_words_queue);
    portENTER_CRITICAL(&irPythonQueueMux);
    irPythonQueueHead = 0;
    irPythonQueueTail = 0;
    portEXIT_CRITICAL(&irPythonQueueMux);
}

int irSetTxPower(int percent) {
    if (percent < 1 || percent > 50) return -1;
    s_tx_power_percent = percent;
    if (!s_hw_up) {
        Serial.printf("[%s] TX power stored (%d%%) — will apply on next hw init\n",
                      TAG, percent);
        return 0;
    }
    float duty = (float)percent / 100.0f;
    if (s_tx_ctx == nullptr) return -1;
    esp_err_t ret = nec_tx_set_carrier_duty(s_tx_ctx, duty);
    Serial.printf("[%s] TX power -> %d%% (duty=%.2f, rmt_apply_carrier=%s)\n",
                  TAG, percent, duty, esp_err_to_name(ret));
    return (ret == ESP_OK) ? 0 : -1;
}

int irGetTxPower() { return s_tx_power_percent; }

// Post-pairing messaging is WiFi API-only; legacy IR notify code removed.

// ─── irTask — FreeRTOS Core 0 ───────────────────────────────────────────────

static inline bool irShouldBeActive() {
    return irHardwareEnabled || pythonIrListening;
}

void irTask(void* /*pvParameters*/) {
    Serial.printf("[%s] irTask started on Core 0\n", TAG);

    for (;;) {
        while (!irShouldBeActive()) vTaskDelay(pdMS_TO_TICKS(50));

        if (!irHwInit()) {
            BadgeBoops::smReset();
            snprintf(BadgeBoops::boopStatus.statusMsg,
                     sizeof(BadgeBoops::boopStatus.statusMsg),
                     "IR unavailable");
            BadgeBoops::setPhase(BadgeBoops::BOOP_PHASE_FAILED, 1000);
            while (irShouldBeActive() && !s_hw_up) {
                vTaskDelay(pdMS_TO_TICKS(250));
                if (irHwInit()) break;
            }
            if (!s_hw_up) continue;
        }
        BadgeBoops::smReset();

        while (irShouldBeActive()) {
            if (irHardwareEnabled) {
                BadgeBoops::smTick();
            } else {
                if (BadgeBoops::boopStatus.phase != BadgeBoops::BOOP_PHASE_IDLE) {
                    BadgeBoops::setPhase(BadgeBoops::BOOP_PHASE_IDLE);
                }
                BadgeBoops::boopEngaged = false;
                BadgeBoops::pairingCancelRequested = false;
            }

            feedPythonQueue();
            servicePythonTx();
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        Serial.printf("[%s] irTask stack_hwm_bytes=%u\n",
                      TAG,
                      (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        BadgeBoops::smShutdown();
        irHwDeinit();
    }
}
