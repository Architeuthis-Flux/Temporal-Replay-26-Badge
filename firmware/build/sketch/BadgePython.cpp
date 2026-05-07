#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgePython.cpp"
// BadgePython.cpp — MicroPython VM lifecycle, escape chord timer, error handling
//
// Manages the embedded MicroPython interpreter:
//   - One-time init during boot (heap allocation, LittleFS mount, mp_embed_init)
//   - Per-app execution with escape chord (BTN_UP + BTN_DOWN simultaneously)
//   - Soft-reset between app launches (mp_embed_deinit + mp_embed_init)
//   - Error display for crashes, silent return for badge.exit() and KeyboardInterrupt
//
// CORE 1 ONLY — all mp_* and gc_* calls must happen on Core 1 (main Arduino loop()).

#include "BadgePython.h"
#include "BadgeDisplay.h"
#include "BadgeConfig.h"
#include "badge_mp/BadgePython_bridges.h"

#include <LittleFS.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

// MicroPython embed port API — must be wrapped in extern "C"
// #include <micropython_embed.h> triggers arduino-cli to load the micropython_embed
// library, adding micropython_embed/src/ to the include path for all sketch files.
extern "C" {
    #include <micropython_embed.h>
    // py/runtime.h declares mp_sched_keyboard_interrupt() used by the escape timer
    #include "py/runtime.h"
    // badge_exit_requested: set by badge.exit() before raising SystemExit
    extern volatile int badge_exit_requested;
}

// ─── Module state ─────────────────────────────────────────────────────────────

static PythonRuntimeState _state    = PYTHON_UNINIT;
// MicroPython GC heap — placed in PSRAM to free 128 KB of SRAM for WiFi/TLS/stack.
// ps_malloc() allocates from external PSRAM (8 MB on XIAO ESP32-S3).
// The pointer is set once in init() and lives for the process lifetime.
static uint8_t* _heap = nullptr;

// Python execution task stack size.
// The loop task default is 8 KB (CONFIG_ARDUINO_LOOP_STACK_SIZE=8192), which is
// too small for mp_embed_exec_str + the MicroPython VM frame. Use a dedicated
// task with 32 KB stack, pinned to Core 1 (same as main loop).
#define PYTHON_TASK_STACK_BYTES  32768

// Context passed from execApp() to the pyExecTask FreeRTOS task.
struct PyExecCtx {
    const char*       src;        // null-terminated Python source (heap-allocated)
    SemaphoreHandle_t done;       // binary semaphore: give when execution finishes
};

// File-scope TCB for statically-allocated pyExecTask.
// execApp() and execStr() cannot overlap (_state guard), so one TCB suffices.
static StaticTask_t _pyTaskTCB;

static void pyExecTask(void* arg);  // defined below

// Spawn pyExecTask with a PSRAM stack if available, falling back to internal RAM.
// Returns the allocated PSRAM stack (caller must heap_caps_free after task completes),
// or nullptr if internal RAM was used (FreeRTOS owns that stack automatically).
// Sets *createdOut to pdPASS on success, error code on failure.
static StackType_t* spawnPyTask(PyExecCtx* ctx, BaseType_t* createdOut) {
    StackType_t* psStack = (StackType_t*)heap_caps_malloc(
        PYTHON_TASK_STACK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (psStack) {
        TaskHandle_t th = xTaskCreateStaticPinnedToCore(
            pyExecTask, "badge_py",
            PYTHON_TASK_STACK_BYTES, ctx, 1, psStack, &_pyTaskTCB, 1);
        *createdOut = (th != NULL) ? pdPASS : errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
        if (*createdOut != pdPASS) { heap_caps_free(psStack); psStack = nullptr; }
    }
    if (!psStack) {
        *createdOut = xTaskCreatePinnedToCore(
            pyExecTask, "badge_py",
            PYTHON_TASK_STACK_BYTES, ctx, 1, NULL, 1);
    }
    return psStack;
}

// Exception detection: mp_embed_exec_str returns void.
// mp_hal_stdout_tx_strn in BadgePython_bridges.cpp sets this when it sees
// "Traceback" output — the prefix MicroPython uses for exception tracebacks.
// Regular print() output does NOT start with "Traceback", so this is safe.
volatile bool _pyHadException = false;

// ─── Escape chord flag ───────────────────────────────────────────────────────
// Set by FreeRTOS timer callback (50ms period) when both BTN_UP and BTN_DOWN
// are held simultaneously. BadgePython::execApp reads this to distinguish
// KeyboardInterrupt (escape) from other exceptions.
static volatile bool _escapeTriggered = false;

// ─── FreeRTOS escape chord timer ─────────────────────────────────────────────

static TimerHandle_t _escapeTimer = nullptr;

static void escapeTimerCb(TimerHandle_t) {
    // Called from FreeRTOS timer task — safe to call mp_sched_keyboard_interrupt
    // because it uses an atomic section internally (cross-core safe).
    if (digitalRead(BTN_UP) == LOW && digitalRead(BTN_DOWN) == LOW) {
        _escapeTriggered = true;
        mp_sched_keyboard_interrupt();
    }
}

// ─── BadgePython::init ────────────────────────────────────────────────────────

void BadgePython::init() {
    if (_state != PYTHON_UNINIT) return;

    // Mount LittleFS partition (label "spiffs") at VFS path "/spiffs".
    // Base path must NOT be "/" — ESP-IDF rejects root as a VFS mount point.
    // After this, fopen("/spiffs/hello.py") works via ESP32 POSIX VFS layer.
    if (!LittleFS.begin(false, "/spiffs", 5, "spiffs")) {
        Serial.println("[BadgePython] LittleFS.begin() failed — Python DISABLED");
        _state = PYTHON_DISABLED;
        return;
    }
    Serial.println("[BadgePython] LittleFS mounted");

    // Allocate MicroPython GC heap from PSRAM to free SRAM for WiFi/TLS.
    // Falls back to SRAM malloc if PSRAM is not available on this board variant.
    _heap = (uint8_t*)ps_malloc(MICROPY_HEAP_SIZE);
    bool usedPSRAM = (_heap != nullptr);
    if (!_heap) _heap = (uint8_t*)malloc(MICROPY_HEAP_SIZE);
    if (!_heap) {
        Serial.println("[BadgePython] heap alloc failed — Python DISABLED");
        _state = PYTHON_DISABLED;
        return;
    }
    Serial.printf("[BadgePython] heap %u B from %s\n",
                  MICROPY_HEAP_SIZE, usedPSRAM ? "PSRAM" : "SRAM");

    unsigned long t0 = millis();

    // Initialize the MicroPython heap and VM.
    // __builtin_frame_address(0) gives the current stack pointer for stack-check.
    mp_embed_init(_heap, MICROPY_HEAP_SIZE, __builtin_frame_address(0));

    unsigned long elapsed = millis() - t0;
    Serial.printf("[BadgePython] init took %lums\n", elapsed);
    _state = PYTHON_IDLE;
}

// ─── Python execution task ────────────────────────────────────────────────────
// Runs mp_embed_exec_str on a task with a large stack (PYTHON_TASK_STACK_BYTES),
// pinned to Core 1. Reinits the VM with this task's stack top so that
// MICROPY_STACK_CHECK uses the correct stack base address.

static void pyExecTask(void* arg) {
    PyExecCtx* ctx = static_cast<PyExecCtx*>(arg);
    Serial.println("[pyExecTask] started");

    // Reinit VM with THIS task's stack pointer as stack_top.
    // BadgePython::init() called mp_embed_init from the loop task (8KB stack at
    // a different address). If we skip this reinit, the GC root scan uses the
    // loop task's stack_top and scans from pyExecTask's SP all the way to the
    // loop task's frame — a ~640KB range that covers other tasks' stacks and
    // corrupts IDLE0 under memory pressure. Always reinit here so stack_top
    // is always correct regardless of which task called the initial init.
    mp_embed_deinit();
    mp_embed_init(_heap, MICROPY_HEAP_SIZE, __builtin_frame_address(0));

    _pyHadException = false;
    mp_embed_exec_str(ctx->src);
    Serial.println("[pyExecTask] mp_embed_exec_str returned");

    // Soft reset: clear Python state for next launch
    mp_embed_deinit();
    mp_embed_init(_heap, MICROPY_HEAP_SIZE, __builtin_frame_address(0));

    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

// ─── BadgePython::execApp ────────────────────────────────────────────────────

void BadgePython::execApp(const char* appPath) {
    if (_state == PYTHON_DISABLED) return;
    if (_state == PYTHON_RUNNING)  return;  // already running — should not happen

    // ── 1. Load source file ──────────────────────────────────────────────────

    Serial.printf("[BadgePython] execApp: opening %s\n", appPath);
    FILE* f = fopen(appPath, "r");
    Serial.printf("[BadgePython] fopen: %s\n", f ? "ok" : "FAIL");
    if (!f) {
        setScreenText("App not found", appPath);
        renderScreen();
        delay(2000);
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize < 0 || fsize > MP_SCRIPT_MAX_BYTES) {
        fclose(f);
        char msg[32];
        snprintf(msg, sizeof(msg), "%ld > %d bytes", fsize, MP_SCRIPT_MAX_BYTES);
        setScreenText("App too large", msg);
        renderScreen();
        delay(2000);
        return;
    }

    char* src = (char*)malloc((size_t)fsize + 1);
    if (!src) {
        fclose(f);
        setScreenText("App error", "OOM loading");
        renderScreen();
        delay(2000);
        return;
    }

    size_t nread = fread(src, 1, (size_t)fsize, f);
    fclose(f);
    src[nread] = '\0';
    Serial.printf("[BadgePython] read %u bytes\n", (unsigned)nread);

    // ── 2. Start escape chord timer ──────────────────────────────────────────

    _escapeTriggered = false;
    badge_exit_requested = 0;

    if (!_escapeTimer) {
        _escapeTimer = xTimerCreate("badge_py_esc",
                                    pdMS_TO_TICKS(50),
                                    pdTRUE,  // auto-reload
                                    nullptr,
                                    escapeTimerCb);
    }
    if (_escapeTimer) xTimerStart(_escapeTimer, 0);

    // ── 3. Execute in a dedicated FreeRTOS task with large stack ────────────
    // The Arduino loop task stack is 8 KB (CONFIG_ARDUINO_LOOP_STACK_SIZE=8192),
    // which is too small for mp_embed_exec_str + MicroPython VM frame.
    // pyExecTask runs on Core 1 with PYTHON_TASK_STACK_BYTES stack.
    // execApp() blocks here until the task signals completion.

    _state = PYTHON_RUNNING;

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    PyExecCtx ctx = { src, done };

    BaseType_t created;
    StackType_t* psStack = spawnPyTask(&ctx, &created);

    if (created != pdPASS) {
        Serial.println("[BadgePython] failed to create exec task — OOM?");
        free(src);
        vSemaphoreDelete(done);
        _state = PYTHON_IDLE;
        setScreenText("App error", "Task OOM");
        renderScreen();
        delay(2000);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);  // wait for Python to finish
    vSemaphoreDelete(done);
    if (psStack) heap_caps_free(psStack);

    free(src);
    _state = PYTHON_IDLE;  // pyExecTask already did soft-reset

    // Auto-disable Python IR receive on any app exit (clean, exception, or badge.exit())
    badge_bridge_ir_stop();

    // ── 4. Stop escape chord timer ───────────────────────────────────────────

    if (_escapeTimer) xTimerStop(_escapeTimer, 0);

    // ── 6. Determine exit type and display message ───────────────────────────

    // badge.exit() — clean voluntary exit
    if (badge_exit_requested) {
        badge_exit_requested = 0;
        return;
    }

    if (_escapeTriggered) {
        // BTN_UP + BTN_DOWN escape chord
        setScreenText("Interrupted", "UP+DOWN");
        renderScreen();
        delay(1500);
        return;
    }

    if (_pyHadException) {
        // Unhandled exception (crash, OOM, syntax error, etc.)
        // mp_embed_exec_str already printed the traceback to Serial.
        setScreenText("App crashed", "See Serial");
        renderScreen();
        delay(2000);
        return;
    }

    // Natural end-of-script — silent return to menu.
}

// ─── BadgePython::execStr ─────────────────────────────────────────────────────
// Lightweight inline execution — no escape timer, no error display.
// Used by the serial test framework.

bool BadgePython::execStr(const char* code) {
    if (_state == PYTHON_DISABLED) return false;
    if (_state == PYTHON_RUNNING)  return false;

    _state = PYTHON_RUNNING;

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    PyExecCtx ctx = { code, done };

    BaseType_t created;
    StackType_t* psStack = spawnPyTask(&ctx, &created);

    if (created != pdPASS) {
        vSemaphoreDelete(done);
        _state = PYTHON_IDLE;
        return false;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    if (psStack) heap_caps_free(psStack);
    _state = PYTHON_IDLE;

    bool ok = !_pyHadException && !badge_exit_requested;
    badge_exit_requested = 0;
    return ok;
}

// ─── BadgePython::listApps ────────────────────────────────────────────────────

void BadgePython::listApps(char names[][32], int* count, int max) {
    *count = 0;
    DIR* d = opendir(MP_APPS_DIR);
    if (!d) return;

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr && *count < max) {
        // Only list .py files
        const char* name = entry->d_name;
        size_t len = strlen(name);
        if (len > 3 && strcmp(name + len - 3, ".py") == 0) {
            size_t baselen = len - 3;
            if (baselen > 31) baselen = 31;
            strncpy(names[*count], name, baselen);
            names[*count][baselen] = '\0';
            (*count)++;
        }
    }
    closedir(d);
}

// ─── BadgePython::isDisabled ─────────────────────────────────────────────────

bool BadgePython::isDisabled() {
    return _state == PYTHON_DISABLED;
}
