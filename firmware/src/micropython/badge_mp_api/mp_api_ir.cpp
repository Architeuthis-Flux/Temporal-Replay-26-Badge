#include <Arduino.h>

#ifdef BADGE_ENABLE_BLE_PROXIMITY
#include "../../ble/BadgeBeaconAdv.h"
#include "../../ble/BleBeaconScanner.h"
#endif
#include "../../ir/BadgeIR.h"

#include "temporalbadge_runtime.h"

// ── IR ──────────────────────────────────────────────────────────────────────

extern "C" int temporalbadge_runtime_ir_send(int addr, int cmd)
{
    return irSendRaw((uint8_t)addr, (uint8_t)cmd);
}

extern "C" void temporalbadge_runtime_ir_start(void)
{
#ifdef BADGE_ENABLE_BLE_PROXIMITY
    BadgeBeaconAdv::setPausedForIr(true);
    BleBeaconScanner::stopScan();
    BleBeaconScanner::clearScanCache();
    for (int i = 0; i < 25 && BadgeBeaconAdv::isBroadcasting(); ++i)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
#endif
    pythonIrListening = true;
    // Wait up to ~500 ms for irTask on Core 0 to bring the RMT hardware up
    // (it polls every 50 ms). Without this, the first ir_send* call after
    // ir_start() would return EPERM if the caller doesn't sleep first.
    for (int i = 0; i < 100; ++i)
    {
        if (irHwIsUp())
            break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    // Drop any frames that lingered from a prior session or the Boop screen
    // so the first ir_read*() after ir_start() never returns stale data.
    irDrainPythonRx();
}

extern "C" void temporalbadge_runtime_ir_stop(void)
{
    pythonIrListening = false;
    irDrainPythonRx();
#ifdef BADGE_ENABLE_BLE_PROXIMITY
    BadgeBeaconAdv::setPausedForIr(false);
#endif
}

extern "C" int temporalbadge_runtime_ir_available(void)
{
    portENTER_CRITICAL(&irPythonQueueMux);
    int avail = (irPythonQueueHead != irPythonQueueTail) ? 1 : 0;
    portEXIT_CRITICAL(&irPythonQueueMux);
    return avail;
}

extern "C" int temporalbadge_runtime_ir_read(int *addr_out, int *cmd_out)
{
    portENTER_CRITICAL(&irPythonQueueMux);
    if (irPythonQueueHead == irPythonQueueTail)
    {
        portEXIT_CRITICAL(&irPythonQueueMux);
        return -1;
    }
    IrPythonFrame f = irPythonQueue[irPythonQueueHead];
    irPythonQueueHead = (irPythonQueueHead + 1) % IR_PYTHON_QUEUE_SIZE;
    portEXIT_CRITICAL(&irPythonQueueMux);
    if (addr_out)
        *addr_out = f.addr;
    if (cmd_out)
        *cmd_out = f.cmd;
    return 0;
}

// ── IR multi-word ───────────────────────────────────────────────────────────

extern "C" int temporalbadge_runtime_ir_send_words(const uint32_t *words,
                                                   size_t count)
{
    return irSendWords(words, count);
}

extern "C" int temporalbadge_runtime_ir_read_words(uint32_t *out,
                                                   size_t max_words,
                                                   size_t *count_out)
{
    return irReadWords(out, max_words, count_out);
}

extern "C" void temporalbadge_runtime_ir_flush(void)
{
    irDrainPythonRx();
}

extern "C" int temporalbadge_runtime_ir_tx_power(int percent)
{
    if (percent < 0)
    {
        return irGetTxPower();
    }
    return (irSetTxPower(percent) == 0) ? irGetTxPower() : -1;
}
