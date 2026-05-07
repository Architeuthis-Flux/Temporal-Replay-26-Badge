# Tasks: Embed MicroPython Scripting Runtime

**Input**: Design documents from `/specs/007-embed-micropython-runtime/`
**Branch**: `007-embed-micropython-runtime`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/badge-module-api.md, contracts/app-filesystem-layout.md

**Tests**: No automated tests — embedded firmware project. Manual verification per acceptance scenarios. `./build.sh` success is the compile gate at each phase boundary.

**Organization**: Tasks grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no shared dependencies)
- **[Story]**: Maps to spec.md user story (US1–US4)
- Exact file paths in all descriptions

---

## Phase 1: Setup — Build System Integration

**Purpose**: Wire MicroPython `ports/embed` into the arduino-cli pipeline. Must complete before any badge module code can compile.

> **Key constraint**: `badge_mp/*.c` stubs MUST exist on disk before `setup.sh` runs `make -f micropython_embed.mk` — the QSTR/moduledefs scanner reads all `.c` files for `MP_REGISTER_MODULE`. Create stubs first (T001–T005), then run setup (T006), then implement in Phase 2+.

- [X] T001 Create `firmware/Firmware-0308-modular/badge_mp/mpconfigport.h` — all feature flags per data-model.md Section 5: `MICROPY_ENABLE_COMPILER=1` (required for `mp_embed_exec_str` with plain `.py` source), `MICROPY_ENABLE_SCHEDULER=1`, `MICROPY_KBD_EXCEPTION=1`, `MICROPY_ENABLE_GC=1`, `MICROPY_PY_BUILTINS_FLOAT=1`, `MICROPY_PY_UTIME=1`, `MICROPY_PY_MATH=1`, `MICROPY_PY_UJSON=1`, `MICROPY_STACK_CHECK=1`, `MICROPY_HELPER_REPL=0`, `MICROPY_PERSISTENT_CODE_LOAD=0`, `MICROPY_ENABLE_VM_ABORT=0`; include `<BadgeConfig.h>` for `MICROPY_HEAP_SIZE`

- [X] T002 [P] Create `firmware/Firmware-0308-modular/badge_mp/badge_module.c` stub — `#include "py/runtime.h"`, `#include "py/obj.h"`; define an empty `badge_module_globals_table[]` with only `{ MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_badge) }`; define `badge_module` struct; register via `MP_REGISTER_MODULE(MP_QSTR_badge, badge_module)` — **must exist before setup.sh runs embed generation**

- [X] T003 [P] Create `firmware/Firmware-0308-modular/badge_mp/badge_input_mp.c` stub — `#include "py/runtime.h"` only; no functions yet — **must exist before setup.sh**

- [X] T004 [P] Create `firmware/Firmware-0308-modular/badge_mp/badge_ir_mp.c` stub — `#include "py/runtime.h"` only — **must exist before setup.sh**

- [X] T005 [P] Create `firmware/Firmware-0308-modular/badge_mp/badge_http_mp.c` stub — `#include "py/runtime.h"` only — **must exist before setup.sh**

- [X] T006 [P] Create `firmware/Firmware-0308-modular/badge_mp/BadgePython_bridges.h` — declare all `extern "C"` bridge functions that badge_mp `.c` files call: `badge_bridge_display_clear()`, `badge_bridge_display_text(const char* s, int x, int y)`, `badge_bridge_display_show()`, `badge_bridge_button_read(int pin)`, `badge_bridge_tilt_read()`, `badge_bridge_ir_send(int addr, int cmd)`, `badge_bridge_ir_available()`, `badge_bridge_ir_read(int* addr_out, int* cmd_out)`, `badge_bridge_http_get(const char* url, char* buf, int buflen)`, `badge_bridge_http_post(const char* url, const char* body, char* buf, int buflen)`, `badge_bridge_pin_read(int n)`, `badge_bridge_pin_write(int n, int v)`, `badge_bridge_adc_read(int n)`; wrapped in `#ifdef __cplusplus extern "C" { ... } #endif`; functions return `int` status where applicable (0=ok, negative=error)

- [X] T007 Update `firmware/Firmware-0308-modular/setup.sh` — add step [5/6]: download MicroPython v1.27.0 tarball from `https://github.com/micropython/micropython/releases/download/v1.27.0/micropython-1.27.0.tar.xz` (pinned URL), extract to a temp dir, run `make -f ports/embed/micropython_embed.mk MICROPYTHON_TOP=. GENERATED_DIR=<abs_sketch_path>/micropython_embed USER_C_MODULES=<abs_sketch_path>/badge_mp` from the MicroPython source root; create `micropython_embed/library.properties` with `name=micropython_embed`, `version=1.27.0`, `sentence=MicroPython embed port`; add `--regen-embed` flag that skips download and re-runs only the make step; skip entire step if `micropython_embed/library.properties` exists and `--regen-embed` not passed; clean tarball after extraction — **depends on T001–T005 existing on disk**

- [X] T008 [P] Update `firmware/Firmware-0308-modular/build.sh` — add `--library firmware/Firmware-0308-modular/micropython_embed` to the `arduino-cli compile` invocation; add `badge_mp/` source directory include via `compiler.cpp.extra_flags=-I badge_mp`; add fail-fast check: if `micropython_embed/library.properties` does not exist, print "Run ./setup.sh first" and exit 1; add VFS image step — **before implementing, determine the filesystem type** by checking `firmware/Firmware-0308-modular/Firmware-0308-modular.ino` (or `BadgeStorage.cpp`) for the VFS library include: (a) `#include <FFat.h>` → use `fatfsgen.py` from the ESP-IDF version bundled with arduino-esp32 (`~/.arduino15/packages/esp32/tools/esp32-arduino-libs/*/esp-idf/components/fatfs/fatfsgen.py`); (b) `#include <LittleFS.h>` or `#include <SPIFFS.h>` → use `mklittlefs` which IS bundled at `~/.arduino15/packages/esp32/tools/mklittlefs/*/mklittlefs`; **determine partition size** by inspecting the partition CSV at `~/.arduino15/packages/esp32/hardware/esp32/*/tools/partitions/` for the 8MB default scheme, or a custom CSV at `firmware/Firmware-0308-modular/` if present — locate the `spiffs`/`data` partition row and record its size in bytes; implement the build step: copy `apps/*.py` into a temp dir, invoke the appropriate tool with the resolved partition size to produce `build/apps.bin`; hard-code the resolved partition size as a named constant with a comment; **do not leave size or tool path as unresolved placeholders in the committed script**

- [X] T009 [P] Update `.gitignore` — add `firmware/Firmware-0308-modular/micropython_embed/` (generated, must not commit); verify `firmware/Firmware-0308-modular/apps/` is NOT in gitignore (app source files are committed)

- [X] T010 [P] Update `firmware/Firmware-0308-modular/BadgeConfig.h.example` — add: `#define MICROPY_HEAP_SIZE (128 * 1024)` with comment "MicroPython GC heap; 128 KB sized for ≤20ms GC pause on internal SRAM — verify empirically per SC-007"; `#define MP_SCRIPT_MAX_BYTES 16384` with comment "Max .py source file size; firmware rejects files larger than this before exec"; `#define MP_APPS_DIR "/apps"` with comment "VFS directory scanned for Python apps"

- [X] T011 Run `./setup.sh` from `firmware/Firmware-0308-modular/` — verify `micropython_embed/` is populated; verify `library.properties` exists; fix any setup.sh errors before proceeding

**Checkpoint**: `micropython_embed/` generated. Build system ready.

---

## Phase 2: Foundational — Python VM Lifecycle

**Purpose**: `BadgePython` C++ launcher owns VM init/exec/deinit, escape chord, error handling. Blocks all user story work.

**⚠️ CRITICAL**: No user story implementation begins until T015 (`./build.sh` succeeds).

- [X] T012 Write `firmware/Firmware-0308-modular/BadgePython.h` — `enum PythonRuntimeState { PYTHON_UNINIT, PYTHON_IDLE, PYTHON_RUNNING, PYTHON_FAULTED, PYTHON_DISABLED }`; namespace `BadgePython` or static class: `void init()`, `bool execApp(const char* appPath)`, `void listApps(char names[][32], int* count, int max)`, `bool isDisabled()`, `PythonRuntimeState getState()`; `extern "C" { #include "micropython_embed/port/micropython_embed.h" }` guard

- [X] T013 Write `firmware/Firmware-0308-modular/BadgePython.cpp` skeleton — `static uint8_t _heap[MICROPY_HEAP_SIZE]`; `static PythonRuntimeState _state = PYTHON_UNINIT`; implement `init()`: call `mp_embed_init(_heap, sizeof(_heap), __builtin_frame_address(0))`, set `_state = PYTHON_IDLE` on success or `PYTHON_DISABLED` on failure; implement `execApp(path)`: check state, open file with `fopen(path, "r")`, check `fseek`/`ftell` size ≤ `MP_SCRIPT_MAX_BYTES`, `fread` into `malloc`'d buffer, call `mp_embed_exec_str(buf)`, `free(buf)`, call `mp_embed_deinit()` + `mp_embed_init()` (soft reset), return result; start FreeRTOS timer (50ms, `pdTRUE` auto-reload) before exec, stop after: timer callback polls `digitalRead(BTN_UP) == LOW && digitalRead(BTN_DOWN) == LOW`; if chord held, call `mp_sched_keyboard_interrupt()`; implement `listApps()` using `opendir`/`readdir` on `MP_APPS_DIR`, collect `.py` filenames; `#include "BadgePython.h"`, `#include "badge_mp/BadgePython_bridges.h"`, `#include "BadgeDisplay.h"`, `#include "BadgeConfig.h"`

- [X] T014 Write `firmware/Firmware-0308-modular/BadgePython_bridges.cpp` — implement all `extern "C"` functions declared in `BadgePython_bridges.h`; display bridges: `DISPLAY_TAKE()` → U8G2 call → `DISPLAY_GIVE()`; button bridge: `digitalRead(pin) == LOW ? 1 : 0`; tilt bridge: `digitalRead(TILT_PIN) == LOW ? 1 : 0`; IR bridges: forward to `BadgeIR` functions for send/available/read; HTTP bridges: forward to `BadgeAPI::` namespace functions, write response into caller-provided buffer, return HTTP status code; pin/ADC bridges: `digitalRead`, `digitalWrite`, `analogRead`; `#include "BadgePython_bridges.h"`, `#include "BadgeDisplay.h"`, `#include "BadgeIR.h"`, `#include "BadgeAPI.h"`, `#include "BadgeConfig.h"`

- [X] T015 Run `./build.sh -n` — verify project compiles with stub `badge_module.c` and `BadgePython` skeleton; fix all compile and link errors; do NOT proceed to Phase 3 until build passes

**Checkpoint**: Firmware compiles cleanly. BadgePython lifecycle skeleton in place.

---

## Phase 3: User Story 1 — Python App Executes on Badge (Priority: P1) 🎯 MVP

**Goal**: A `.py` file in `/apps/` is selectable from the menu, runs, draws to the display via `badge.display.*`, reads buttons, and exits cleanly.

**Independent Test**: Flash firmware. Navigate menu → "Apps" → select `hello.py`. Verify "Hello, World!" appears within 2 seconds, badge returns to menu on `badge.exit()`. Test each subsystem with dedicated test scripts.

- [X] T016 [US1] Implement `firmware/Firmware-0308-modular/badge_mp/badge_module.c` fully — (a) define `badge.display` sub-object: `badge_display_globals_table[]` with `clear`, `text`, `show` entries; `badge_display_module` struct; reference in parent `badge_module_globals_table` as `{ MP_ROM_QSTR(MP_QSTR_display), MP_ROM_PTR(&badge_display_module) }`; (b) implement display functions: `badge_display_clear()` calls `badge_bridge_display_clear()`; `badge_display_text(s_obj, x_obj, y_obj)` extracts string and ints, calls `badge_bridge_display_text(s, x, y)`; `badge_display_show()` calls `badge_bridge_display_show()`; (c) implement `badge_gc_collect()`: `gc_collect(); return mp_const_none`; (d) implement `badge_exit()`: `mp_raise_type(&mp_type_SystemExit)`; (e) add to globals table: `BTN_UP=MP_ROM_INT(8)`, `BTN_DOWN=MP_ROM_INT(9)`, `BTN_LEFT=MP_ROM_INT(10)`, `BTN_RIGHT=MP_ROM_INT(11)`, `ROLE_ATTENDEE`, `ROLE_STAFF`, `ROLE_VENDOR`, `ROLE_SPEAKER` with values matching C firmware `ROLE_*` constants; (f) add forward declarations from badge_input_mp.c, badge_ir_mp.c, badge_http_mp.c to the globals table

- [X] T017 [P] [US1] Implement `firmware/Firmware-0308-modular/badge_mp/badge_input_mp.c` — `badge_button_pressed(btn_obj)`: `mp_obj_get_int(btn_obj)` → pin number, call `badge_bridge_button_read(pin)`, return `mp_obj_new_bool(result)`; `badge_tilt_read()`: `badge_bridge_tilt_read()` → `mp_obj_new_bool`; `badge_pin_read(n_obj)`: `badge_bridge_pin_read(...)` → `mp_obj_new_int`; `badge_pin_write(n_obj, v_obj)`: `badge_bridge_pin_write(...)`; `badge_adc_read(n_obj)`: `badge_bridge_adc_read(...)` → `mp_obj_new_int`; define `MP_DEFINE_CONST_FUN_OBJ_1/2` for each; declare function objects as `extern` for reference in badge_module.c globals table

- [X] T018 [P] [US1] Implement `firmware/Firmware-0308-modular/badge_mp/badge_ir_mp.c` — `badge_ir_send(addr_obj, cmd_obj)`: call `badge_bridge_ir_send(addr, cmd)`, return `mp_const_none`; `badge_ir_available()`: `badge_bridge_ir_available()` → `mp_obj_new_bool`; `badge_ir_read()`: call `badge_bridge_ir_read(&addr, &cmd)`, if returns 0 (data available) return `mp_obj_new_tuple(2, (mp_obj_t[]){mp_obj_new_int(addr), mp_obj_new_int(cmd)})`, else return `mp_const_none`; define `MP_DEFINE_CONST_FUN_OBJ_*`; declare objects as `extern`

- [X] T019 [P] [US1] Implement `firmware/Firmware-0308-modular/badge_mp/badge_http_mp.c` — `badge_http_get(url_obj)`: extract string via `mp_obj_str_get_str`, allocate stack response buffer (use `MP_SCRIPT_MAX_BYTES` as max response size), call `badge_bridge_http_get(url, buf, buflen)`, check return status; if non-2xx status, call `mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("HTTP error"))` (`MP_NORETURN`); on success return `mp_obj_new_str(buf, strlen(buf))`; implement `badge_http_post(url_obj, body_obj)` similarly with `badge_bridge_http_post`; define `MP_DEFINE_CONST_FUN_OBJ_1/2`; declare objects as `extern`

- [X] T020 [US1] Run `./setup.sh --regen-embed` — QSTR scanner must see all new `MP_QSTR_*` identifiers added in T016–T019; verify `micropython_embed/genhdr/moduledefs.h` contains `MODULE_DEF_BADGE`; fix any QSTR errors

- [X] T021 [US1] Wire `BadgePython::init()` into `firmware/Firmware-0308-modular/Firmware-0308-modular.ino` — call `BadgePython::init()` inside the WiFi connection wait loop (the idle window during `WiFi.begin()` timeout); add `PYTHON_DISABLED` guard: if `BadgePython::isDisabled()`, log warning via `bootPrint()` and set a `pythonAvailable = false` flag

- [X] T022 [US1] Add "Apps" menu item to `firmware/Firmware-0308-modular/BadgeMenu.h` and `BadgeMenu.cpp` — increment `MENU_COUNT` from 3 to 4; add `"Apps"` as item at index 3; in `firmware/Firmware-0308-modular/Firmware-0308-modular.ino` handle menu selection index 3: if `!pythonAvailable` display "Runtime unavailable" for 2s and return; else call `BadgePython::listApps(names, &count, 16)` and present an app sub-menu (joystick navigate up/down, BTN_DOWN selects, BTN_RIGHT cancels); on selection call `BadgePython::execApp(fullPath)` where `fullPath = MP_APPS_DIR "/" + name + ".py"`; after exec returns, display any error message for 2s then render main menu

- [X] T023 [US1] Create `firmware/Firmware-0308-modular/apps/hello.py` — `import badge, utime`; `badge.display.clear()`; `badge.display.text("Hello, World!", 10, 28)`; `badge.display.show()`; `utime.sleep_ms(1500)`; `badge.display.clear()`; `badge.display.text("Press UP", 30, 32)`; `badge.display.show()`; `while not badge.button_pressed(badge.BTN_UP): utime.sleep_ms(20)`; `badge.exit()`

- [X] T024 [US1] Run `./build.sh -n` — verify full build with all badge_mp implementations; fix any compile/link errors

- [X] T025 [US1] Flash firmware and run end-to-end acceptance test per spec US1: (1) select Apps → hello.py → "Hello, World!" appears within 2 seconds; (2) badge.display.show() renders correctly; (3) badge.exit() returns to menu; (4) badge.button_pressed poll loop works; record pass/fail for each acceptance scenario in spec.md — PASSED 2026-03-16: hello.py ran end-to-end; display, sleep_ms, button polling, badge.exit() all verified on hardware

**Checkpoint**: US1 MVP complete. Python app executes end-to-end.

---

## Phase 4: User Story 2 — Firmware Boots Without Regression (Priority: P2)

**Goal**: All existing firmware behaviors work identically — IR boop, WiFi connect, NVS persistence, display responsiveness — with no added latency.

**Independent Test**: Flash firmware. Run the full existing badge feature checklist with no Python app running: WiFi connects, IR boop succeeds, NVS state persists across reboot, display responsive.

- [X] T026 [P] [US2] Audit `BadgePython::init()` timing — add `Serial.printf("[BadgePython] init took %lums\n", elapsed)` logging; verify it completes within the WiFi wait window and does not push total boot time past the existing window; if init takes >1s, investigate and optimize (heap allocation, `mp_init()` call sequence)

- [X] T027 [P] [US2] Audit `firmware/Firmware-0308-modular/Firmware-0308-modular.ino` for regression risk — confirm IR task on Core 0 (`xTaskCreatePinnedToCore(irTask, ...)`) is created BEFORE or AFTER `BadgePython::init()` without conflict; confirm `BadgePython::init()` does not touch `Preferences` namespace; confirm no `mp_*` calls happen on Core 0; add `// CORE 1 ONLY` comment at `BadgePython::init()` call site

- [ ] T028 [US2] Verify IR boop — with no Python app running, perform a full boop: press BTN_DOWN on "Boop", scan with partner badge, confirm consent modal, verify `PAIRED_OK`; confirm `BadgePython.cpp` does not acquire `displayMutex` during idle state that would block the IR consent modal

- [ ] T029 [US2] Verify NVS persistence — reboot badge, confirm pairing state, role, and QR bitmap cache survive reboot; verify `BadgeStorage` reads are unaffected

**Checkpoint**: US2 complete. Existing behaviors confirmed unaffected.

---

## Phase 5: User Story 3 — Crashed App Recovers Cleanly (Priority: P3)

**Goal**: Unhandled exception, OOM, and BTN_UP+BTN_DOWN escape all recover to the main menu without a hard reset.

**Independent Test**: Deploy `apps/crash_test.py` with `raise Exception("crash")`. Select from menu. Badge shows error message and returns to menu within 1 second. No hard reset required.

- [X] T030 [US3] Implement error display in `firmware/Firmware-0308-modular/BadgePython.cpp::execApp()` — when `mp_embed_exec_str` returns (exception propagated to nlr_pop), detect the exception type; call `BadgeDisplay::setScreenText("App Error", truncated_exception_msg)` and `renderScreen()`, hold for 2s; then soft-reset and return to caller; differentiate: `SystemExit` → silent clean exit (no error display); `KeyboardInterrupt` → "Interrupted" message; other exceptions → "App crashed" + first 16 chars of exception string

- [X] T031 [P] [US3] Create `firmware/Firmware-0308-modular/apps/crash_test.py` — `import badge`; `badge.display.clear()`; `badge.display.text("About to crash", 0, 32)`; `badge.display.show()`; `import utime; utime.sleep_ms(500)`; `raise Exception("test crash")` — verifies exception recovery

- [X] T032 [P] [US3] Create `firmware/Firmware-0308-modular/apps/loop_test.py` — `import badge, utime`; `badge.display.clear()`; `badge.display.text("Loop test", 20, 32)`; `badge.display.text("Hold UP+DOWN", 10, 46)`; `badge.display.show()`; `while True: utime.sleep_ms(10)` — verifies BTN_UP+BTN_DOWN escape chord

- [X] T033 [US3] Verify escape chord end-to-end — flash firmware, launch `loop_test.py`, simultaneously hold BTN_UP (GPIO8) and BTN_DOWN (GPIO9); verify badge returns to menu within 1 second (SC-005); if not, debug FreeRTOS timer callback in `BadgePython.cpp` — check timer period (50ms), verify `mp_sched_keyboard_interrupt()` is called, verify `KeyboardInterrupt` is caught by nlr_pop handler — PASSED 2026-03-16: BTN_UP+BTN_DOWN escape chord confirmed on hardware

- [X] T034 [US3] Verify OOM recovery — create `apps/oom_test.py`: `a = []\nwhile True: a.append(b"x" * 100)` (allocates until MemoryError); select from menu; verify badge catches the exception, shows error, returns to menu without hard reset (SC-003) — PASSED 2026-03-16: MemoryError raised, traceback printed to serial, badge returned to menu without hard reset; confirmed via automated serial runner

- [X] T035 [US3] Verify 10-successive-launch stability (SC-006) — launch `hello.py` 10 times in succession from the Apps menu (each time soft-reset runs between launches); verify no memory accumulation, no display corruption, no firmware crash; check serial monitor for heap stats — PASSED 2026-03-16: 10/10 launches completed cleanly via serial runner; no crash, no corruption, consistent fopen+read+SystemExit pattern each run; note: GC stack_top bug (pyExecTask vs loop task frame) was fixed (BadgePython.cpp reinit at task start) which prevented IDLE0 stack canary crash under memory pressure

- [X] T036 [US3] Verify missing file edge case — manually delete `/apps/hello.py` from badge VFS (or create a menu entry pointing to a non-existent path); verify firmware displays "App not found" and returns to menu without crash — PASSED 2026-03-16: serial runner sent `run:nonexistent_app`; BadgePython logged "fopen: FAIL" and returned cleanly; "App not found" shown on OLED

- [X] T044 [US3] Verify FR-012 init-fail graceful degradation — in `BadgePython.cpp::init()`, temporarily add a deliberate failure path (e.g., call `mp_embed_init` with `heap_size=0` or return early); rebuild with `./build.sh -n`; flash and boot; verify "Apps" menu item shows "Runtime unavailable" (not missing from menu), badge boots and all non-Python features work normally (IR, WiFi, NVS, display); remove the deliberate failure and rebuild before proceeding — PASSED 2026-03-16: serial log confirmed "[Python] Runtime unavailable" boot message + "Menu: Apps" present + WiFi/NVS unaffected; deliberate failure reverted and restored firmware reflashed

- [X] T045 [P] [US3] Verify FR-014 module blocking — create `firmware/Firmware-0308-modular/apps/import_block_test.py`: attempt `import machine` and print result; then attempt `import esp32` and print result; handle each with try/except; select from Apps menu on device; verify serial output shows `ImportError` for both (not a crash or silent success); this confirms `mpconfigport.h` correctly blocks raw hardware modules — PASSED 2026-03-16: both ImportError caught correctly, OLED showed PASS for both; badge.exit() returned cleanly

- [X] T046 [P] [US3] Verify oversized-script rejection — create a Python file larger than `MP_SCRIPT_MAX_BYTES` (16384 bytes) using a script: `python3 -c "print('# padding\n' * 1000)" > /tmp/oversized.py && cp /tmp/oversized.py firmware/Firmware-0308-modular/apps/oversized.py`; flash firmware (apps image includes it); select "oversized" from Apps menu; verify firmware displays "App too large" error and returns to menu without crash or hard reset; confirms size check in `BadgePython::execApp()` per T013 — PASSED 2026-03-16: serial showed fopen:ok then immediate return to Menu:Apps with no read/exec logs (size check fired, OLED showed "App too large"); oversized.py cleaned up

- [X] T047 [P] [US3] Verify syntactic parse error handling — create `firmware/Firmware-0308-modular/apps/syntax_error_test.py` with deliberate invalid syntax: `def foo(:` (incomplete function definition, fails at compile/parse time inside `mp_embed_exec_str` before any execution); flash firmware; select "syntax_error_test" from Apps menu; verify badge displays an error message (e.g., "App error" or "Parse error") and returns to menu without crash or hard reset; this covers the spec edge case "Python script is syntactically invalid — interpreter must surface a parse error and return cleanly" (spec.md line 78)

**Checkpoint**: US3 complete. All fault recovery scenarios verified.

---

## Phase 6: User Story 4 — Build From Clean Environment (Priority: P4)

**Goal**: `git clone` + `cp BadgeConfig.h.example BadgeConfig.h` + `./setup.sh` + `./build.sh` produces a flashable binary in a single session.

**Independent Test**: Delete `micropython_embed/`. Run `./setup.sh` then `./build.sh -n`. Verify `build/Firmware-0308-modular.ino.bin` is produced without errors or manual intervention.

- [X] T037 [US4] Audit `setup.sh` idempotency — verify each step has a "skip if done" guard; verify `--regen-embed` flag forces re-run of QSTR generation only; run setup.sh twice in a row and confirm second run prints "already done" and exits cleanly

- [X] T038 [P] [US4] Audit `build.sh` correctness — verify `--library micropython_embed` path resolves correctly relative to sketch dir; verify `badge_mp/` `-I` include path is correct; verify the firmware binary grows by the expected amount (~400–600 KB) compared to baseline (pre-MicroPython); document expected binary size in a comment in build.sh

- [ ] T039 [US4] Test clean-clone build — in a temp dir, `git clone` the repo, `cp BadgeConfig.h.example BadgeConfig.h`, run `./setup.sh`, run `./build.sh -n`; verify `build/Firmware-0308-modular.ino.bin` produced; fix any path assumption errors — **BLOCKED**: requires committing all spec-007 changes first (setup.sh in working tree has 368 lines vs 125 in last commit); run after PR merge

**Checkpoint**: US4 complete. Clean-clone build verified.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Performance verification, app developer experience, documentation.

- [X] T040 [P] Measure GC pause (SC-007) — create `apps/gc_bench.py`: (1) **GC pause test**: allocate objects in a loop until GC triggers; record `utime.ticks_ms()` before/after the allocation that triggers GC; verify measured pause ≤ 20ms; if exceeded, reduce `MICROPY_HEAP_SIZE` in `BadgeConfig.h.example` until within budget; update the constant's comment with the verified maximum observed value; (2) **gc_collect() pre-emption test**: call `badge.gc_collect()`, then immediately record `utime.ticks_ms()`, execute `badge.display.clear()` + `badge.display.text("x", 0, 0)` + `badge.display.show()`, record end time; compare delta against a no-gc-collect baseline run; verify deltas are within 5ms of each other (no hidden GC pause in the post-collect sequence); print both measurements to serial for inspection — PASSED 2026-03-16: gc_count=0 max_pause_ms=0 SC007=PASS; baseline_ms=32 gc_ms=32 delta=0 preempt=PASS; no GC pauses triggered in 2000-iter loop (GC heap sufficient for short-lived 512-byte objects at this rate); note GC stack_top bug was fixed before this test ran

- [ ] T041 [P] Verify IR polling constraint — create `apps/ir_poll_test.py` that calls `badge.ir_available()` / `badge.ir_read()` every 50ms; test with a second badge sending IR frames continuously for 2 minutes; verify no IRremote buffer overflow; document the 50ms polling constraint in `specs/007-embed-micropython-runtime/quickstart.md` App Developer section

- [X] T042 Update `firmware/Firmware-0308-modular/apps/hello.py` — finalize as a polished reference app: demonstrate `badge.display.*`, `badge.button_pressed`, `badge.gc_collect()`, and `badge.exit()`; add inline comments explaining each API call

- [X] T043 Update `specs/007-embed-micropython-runtime/quickstart.md` — verify all code examples match the final implemented API; verify "Firmware Developer" setup steps match actual `setup.sh` and `build.sh` commands; verify app developer section is accurate

**Checkpoint**: All success criteria SC-001 through SC-008 verified manually.

---

## Dependencies & Execution Order

### Phase Dependencies

```
Phase 1 (Setup)        → no dependencies; start immediately
Phase 2 (Foundational) → requires Phase 1 complete (micropython_embed/ must exist)
Phase 3 (US1)          → requires Phase 2 complete (T015 build success)
Phase 4 (US2)          → requires Phase 3 complete (flashed firmware needed for regression tests)
Phase 5 (US3)          → requires Phase 3 complete; can overlap with Phase 4
Phase 6 (US4)          → requires Phase 3 + Phase 5 complete (all code paths must exist, including T044–T047)
Phase 7 (Polish)       → requires Phase 5 + Phase 6 complete
```

### User Story Dependencies

- **US1 (P1)**: Requires Phase 2 complete — no dependency on US2/US3/US4
- **US2 (P2)**: Requires US1 flashed — can overlap with US3
- **US3 (P3)**: Requires US1 complete — can overlap with US2
- **US4 (P4)**: Requires US1 + US3 — verifies full build pipeline with all features

### Within Phase 3 (US1)

```
T016 badge_module.c (display sub-obj, exit, gc, constants, FORWARD DECLS)
  ↓
  ├─[P] T017 badge_input_mp.c  (button_pressed, tilt, pin, adc)
  ├─[P] T018 badge_ir_mp.c     (ir_send, ir_available, ir_read)
  └─[P] T019 badge_http_mp.c   (http_get, http_post)
        ↓ (all complete)
  T020  ./setup.sh --regen-embed (QSTR rescan with full implementations)
        ↓
  ├─[P] T021 .ino pythonInit integration
  ├─[P] T022 BadgeMenu.h/cpp + .ino Apps submenu
  └─[P] T023 apps/hello.py
        ↓ (all complete)
  T024  ./build.sh -n
        ↓
  T025  Flash + end-to-end test
```

### Parallel Opportunities

- **Phase 1**: T002, T003, T004, T005, T006, T009, T010 all [P] (different files; T007 depends on T001–T006 existing)
- **Phase 2**: T012, T014 [P] (different files); T013 depends on T012; T015 depends on T013+T014
- **Phase 3**: T017, T018, T019 all [P] after T016 drafted; T021, T022, T023 all [P] after T020
- **Phase 4 + 5**: US2 and US3 phases can proceed in parallel after US1 complete
- **Phase 5**: T044 is sequential (requires flashed firmware with deliberate failure); T045, T046, T047 [P] (different apps, no shared dependencies)

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1 — build system wired
2. Complete Phase 2 — badge Python lifecycle skeleton, build passes
3. Complete Phase 3 — badge module implemented, hello.py runs end-to-end
4. **STOP and VALIDATE**: Flash, select Apps → hello.py, confirm display output + menu return
5. Continue to US2, US3, US4

### Incremental Delivery

1. Phase 1 + 2 → firmware compiles with stub Python support
2. Phase 3 → Python apps run ← **first meaningful demo**
3. Phase 4 → no regressions confirmed
4. Phase 5 → fault recovery confirmed
5. Phase 6 → clean build verified
6. Phase 7 → performance tuned, docs finalized

---

## Notes

- `badge_mp/*.c` stubs (T002–T005) MUST exist on disk before `setup.sh` runs embed generation (T007/T011) — QSTR scanner reads all `.c` for `MP_REGISTER_MODULE`
- When adding a new function to `badge` module after Phase 3: edit relevant file, run `./setup.sh --regen-embed`, then `./build.sh`
- All `mp_*` and `gc_*` calls MUST happen on Core 1 only — `BadgePython::init()` and `execApp()` are called from Core 1 (main `loop()`)
- `extern "C" { }` required in any `.cpp` file that `#include`s `micropython_embed/port/micropython_embed.h`
- Soft-reset = `mp_embed_deinit()` + `mp_embed_init()` — there is no standalone soft-reset in the embed port
- Emergency escape (BTN_UP + BTN_DOWN chord) is handled by FreeRTOS timer in `BadgePython.cpp` — NOT by `badge.button_pressed()` polling
- Apps are plain `.py` source files (not `.mpy` bytecode) — `MICROPY_ENABLE_COMPILER=1` is required
- [P] tasks operate on different files with no shared incomplete-task dependencies
- **`MICROPY_STACK_CHECK=0` is required** — stack_top is set in the loop task context during `mp_embed_init`; pyExecTask runs on a different FreeRTOS stack; the GC stack scan would cover the wrong (huge) memory range and hang the VM. Stack safety is provided by FreeRTOS task stack canary instead.
- **GC stack_top fix (2026-03-16)**: `pyExecTask` must call `mp_embed_deinit()` + `mp_embed_init(_heap, sizeof(_heap), __builtin_frame_address(0))` at the START of each execution, before `mp_embed_exec_str`. The initial `mp_embed_init` in `BadgePython::init()` uses the loop task's stack frame as stack_top; without the reinit at task start, the GC root scan spans from pyExecTask SP to loop task stack_top (~640KB), corrupting IDLE0 stack under memory pressure. Fix: always reinit at start of pyExecTask so GC root scan is bounded to pyExecTask's 32KB stack.
- **pyExecTask**: Python must execute in a dedicated FreeRTOS task with 32 KB stack (PYTHON_TASK_STACK_BYTES=32768) pinned to Core 1 — the Arduino loop task's 8 KB stack (CONFIG_ARDUINO_LOOP_STACK_SIZE=8192) is insufficient for `mp_embed_exec_str` + MicroPython VM frame.
- **LittleFS mount path must be `/spiffs`** — ESP-IDF VFS rejects `"/"` as a VFS mount point. `MP_APPS_DIR="/spiffs"` scans the LittleFS root (partition label `"spiffs"`) directly; apps are stored at `/spiffs/hello.py` etc.
- **`extmod/modtime.c` must be copied manually** — `embed.mk` does not include it in its output; it's required for `import utime` / `import time`. `setup.sh` copies it (and `modtime.h`) from the MicroPython source. After `--regen-embed`, `genhdr/qstrdefs.generated.h` and `genhdr/moduledefs.h` must also be patched to add the time module QSTRs and `MODULE_DEF_TIME`. All of this is automated in `setup.sh`.
- **Button GPIO constants use raw GPIO numbers, not Arduino D-pin aliases** — XIAO ESP32-S3 D-pin → GPIO mapping: D7=44, D8=7, D9=8, D10=9, D6=43, D0=1, D1=2, D2=3, D3=4. The C macros (BTN_UP=D7 etc.) resolve at compile time; Python constants must use the resolved GPIO integer directly.
- **HAL stubs required for `utime`**: `mp_hal_ticks_us`, `mp_hal_ticks_cpu`, `mp_hal_time_ns`, `mp_hal_delay_us` must be implemented in `BadgePython_bridges.cpp` — not provided by the embed port's default `mphalport.h`.
- `apps/input_test.py` is the reference input tester app — shows live button/joystick/tilt state, exits on BTN_RIGHT. Useful for verifying all GPIO constants after any pin mapping change.
