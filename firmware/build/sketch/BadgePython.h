#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgePython.h"
// BadgePython.h — MicroPython scripting runtime for the Temporal Badge
//
// BadgePython owns the Python VM lifecycle:
//   init()       — called once during boot; initializes heap, mounts LittleFS, starts VM
//   execApp()    — called from the Apps menu; runs one .py file to completion
//   execStr()    — execute a Python source string directly (no file needed)
//   listApps()   — enumerates .py files from MP_APPS_DIR on the badge VFS
//   isDisabled() — returns true if init() failed (LittleFS or mp_embed_init failed)
//
// Threading: ALL mp_* and gc_* calls must run on Core 1 only.
// init() and execApp() are called from Core 1 (setup() and loop()).

#pragma once
#include <Arduino.h>

// ─── Python runtime state ─────────────────────────────────────────────────────

enum PythonRuntimeState {
    PYTHON_UNINIT,    // not yet initialized
    PYTHON_IDLE,      // VM initialized, no app running
    PYTHON_RUNNING,   // app currently executing
    PYTHON_DISABLED,  // init failed; all Python features unavailable
};

// ─── BadgePython namespace ────────────────────────────────────────────────────

namespace BadgePython {

    // Initialize the MicroPython VM and mount the badge VFS (LittleFS).
    // Sets state to PYTHON_IDLE on success, PYTHON_DISABLED on failure.
    // MUST be called from Core 1 before execApp().
    // Safe to call multiple times; subsequent calls are no-ops if state != PYTHON_UNINIT.
    void init();

    // Execute the Python app at appPath (e.g. "/apps/hello.py").
    // Blocks until the app exits (badge.exit(), natural end, exception, or escape chord).
    // Handles error display and soft-reset between launches.
    // Returns when the app has finished and control should return to the menu.
    // MUST be called from Core 1.
    void execApp(const char* appPath);

    // Execute a Python source string directly (no file needed).
    // Blocks until execution completes. No escape timer or error display.
    // Returns true if the script completed without exception, false on error.
    // MUST be called from Core 1.
    bool execStr(const char* code);

    // Enumerate .py files in MP_APPS_DIR on the badge VFS.
    // names:  caller-allocated array of name strings (without .py extension)
    // count:  set to number of apps found (≤ max)
    // max:    maximum names to fill
    void listApps(char names[][32], int* count, int max);

    // Returns true if init() failed and Python features are unavailable.
    bool isDisabled();

}  // namespace BadgePython
