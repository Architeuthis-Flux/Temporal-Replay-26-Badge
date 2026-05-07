# Research: Embed MicroPython Scripting Runtime

**Branch**: `007-embed-micropython-runtime` | **Date**: 2026-03-13

## Decision 1 — Integration Mechanism

**Decision**: `ports/embed` source-file approach — generate `micropython_embed/` once via `make -f micropython_embed.mk`, commit the generation scripts, add output to `.gitignore`, wrap output as an Arduino library.

**Rationale**:
- `ports/embed` is the canonical, actively maintained embedding path. It produces a self-contained
  directory of `.c`/`.h` files with no build-time dependency on the MicroPython repository.
- The generated `micropython_embed/` directory can be made an Arduino library by adding
  `library.properties` — arduino-cli then compiles all `.c` files in it automatically.
- This requires a one-time `make -f micropython_embed.mk` step during `setup.sh`, but the
  actual firmware compilation remains 100% arduino-cli with no additional toolchain.

**Alternatives considered**:
- **Pre-built static `.a`**: No official ESP32-S3 prebuilt exists. Building one out-of-band is
  fragile: QSTR regeneration is required whenever a custom module is added, meaning the build
  pipeline cannot be avoided anyway.
- **ESP-IDF component (`ports/esp32`)**: `arduino-esp32` core 3.x is itself an ESP-IDF component.
  Adding MicroPython as a peer component from inside an `.ino` sketch has no supported path.
  Tracking issue #8615 was closed with a dual-build workaround (robdobsn/MicroPythonESP32Embedding)
  that is complex and not maintainable at conference timescales.
- **arduino-esp32 CMake component**: No supported mechanism to inject CMake components from a
  sketch directory in arduino-cli.

**Implementation notes**:
- All `#include "port/micropython_embed.h"` from `.cpp` files must be wrapped in `extern "C" { ... }`.
- `badge_module.c` must exist and be present in the source tree **before** running
  `make -f micropython_embed.mk` — the QSTR/moduledefs generation step scans all `.c` source files
  for `MP_REGISTER_MODULE`. When adding a new C module, re-run the embed generation step.
- `mpconfigport.h` is hand-authored and controls feature flags; it is committed to the repo.

## Decision 2 — MicroPython Version

**Decision**: v1.27.0 (tarball pinned in `setup.sh`).

**Rationale**: v1.27.0 is the latest stable release (2024-12-09). The `ports/embed` approach is
platform-agnostic — it produces C files that compile on any target — so ESP-IDF version
compatibility is not a constraint for embedding. The version is pinned by URL in `setup.sh`
for reproducibility (Principle V).

**Alternatives considered**: v1.24.0 (spec assumption) is stale; v1.25–v1.26 are superseded.
Master is v1.28.0-preview — not stable.

## Decision 3 — MicroPython Heap Sizing

**Decision**: Default 64 KB static buffer allocated from internal DRAM. Constant `MP_HEAP_SIZE`
defined in `BadgeConfig.h` (added to `BadgeConfig.h.example`). Tunable to 128 KB without
firmware changes.

**Rationale**:
- ESP32-S3-MINI-1 has 512 KB SRAM. Arduino core 3.x + FreeRTOS + WiFi stack consume ~100–150 KB
  at idle, leaving ~300–350 KB headroom.
- 64 KB is comfortable for v1 apps (display drawing, string formatting, button polling, `utime`).
- 128 KB is the practical upper limit for DRAM allocation; above this, the WiFi stack's
  peak allocation could cause fragmentation issues on a busy event floor.
- ESP32-S3-MINI-1 does NOT have PSRAM. If PSRAM were present, 512 KB via
  `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` would be appropriate.

**Alternatives considered**: 32 KB is technically sufficient for trivial scripts but leaves
little room for list/dict operations or string concatenation. 128 KB is the safe ceiling
without PSRAM.

## Decision 4 — File Execution API

**Decision**: Apps are plain `.py` source files. `BadgePython::execApp()` reads the file from VFS into a heap buffer using `fopen`/`fread`, validates the size, then calls `mp_embed_exec_str(buffer)`. `MICROPY_ENABLE_COMPILER` must be 1.

**Rationale**:
- The `ports/embed` public API:
  ```c
  void mp_embed_init(void *gc_heap, size_t gc_heap_size, void *stack_top);
  void mp_embed_deinit(void);
  void mp_embed_exec_str(const char *src);        // requires MICROPY_ENABLE_COMPILER=1
  void mp_embed_exec_mpy(const uint8_t*, size_t); // requires MICROPY_PERSISTENT_CODE_LOAD=1
  ```
- The spec Assumptions section states: "Python scripts are plain `.py` files." Using `.py` + `exec_str` is the correct v1 path. No `mpy-cross` needed for app developers.
- Reading in C gives explicit control over error handling: file-not-found returns before any exec, size check prevents OOM before allocation.
- A maximum script size cap (`MP_SCRIPT_MAX_BYTES`, default 16 KB) is enforced before the `malloc`.

**Alternatives considered**:
- Pre-compile `.py` → `.mpy` + `mp_embed_exec_mpy`: saves ~30 KB flash (no compiler) and faster launch. Deferred to v2 — requires `mpy-cross` in the developer workflow and is a breaking change to the app format. Not compatible with the spec's "plain `.py` files" assumption.
- `mp_lexer_new_from_file` with `MICROPY_READER_VFS`: More complex; avoids loading whole file into RAM. Unnecessary for v1 apps expected to be small (≤ 16 KB).

## Decision 5 — Soft-Reset Between App Launches

**Decision**: Call `mp_embed_deinit()` followed by `mp_embed_init()` between each app launch.

**Rationale**: There is no standalone `mp_embed_soft_reset()` in the embed port. The
deinit+reinit cycle fully clears all Python state, GC heap, and module imports —
exactly the behavior specified in FR-006. Heap buffer (`static uint8_t mp_heap[MP_HEAP_SIZE]`)
is allocated once in `BadgePython.cpp` and reused across cycles.

## Decision 6 — FreeRTOS Coexistence

**Decision**: Python interpreter runs on Core 1 (the display/input core, same as specified in FR-002). All `mp_*` and `gc_*` calls happen exclusively on Core 1. Cross-task communication uses only `mp_sched_keyboard_interrupt()` from the button ISR/callback.

**Rationale**:
- The `ports/embed` interpreter has no internal threading. It runs synchronously on the calling
  FreeRTOS task. No GIL; no background MicroPython tasks.
- Core 0 runs the IR task (existing). Core 1 runs display rendering, input polling, and now
  the Python interpreter task. This matches FR-002 and FR-003.
- `mp_sched_keyboard_interrupt()` writes to `MP_STATE_MAIN_THREAD(mp_pending_exception)` inside
  an atomic section — it is ISR-safe and cross-task safe. The VM checks the pending exception
  at each bytecode dispatch (typically within microseconds), so the button-press-to-abort
  latency is negligible.
- Stack size for the Python FreeRTOS task: 16 KB (Python call stack lives on C stack).

**Alternatives considered**: Running Python on a dedicated third task (beyond IR and main loop)
would require a stack-size budget decision and a separate FreeRTOS task for every app launch.
Running Python on the existing `loop()` task on Core 1 is simpler and matches the spec.

## Decision 7 — Build System Integration (arduino-cli)

**Decision**: The generated `micropython_embed/` directory includes a `library.properties` file,
making it discoverable as a local Arduino library by arduino-cli. The `-I` include path is
handled via a `build_opt.h` file in the sketch directory. `build.sh` passes
`--library micropython_embed` to arduino-cli.

**Rationale**:
- arduino-cli's `--library <path>` flag includes a local directory as a library without
  installing it globally. This keeps the generated artifact local to the sketch.
- `build_opt.h` in the sketch folder is auto-included by the platform's compilation recipes
  (`@{build.opt.path}`), making it the correct place for `-I` flags that apply to all sources.
- `build.extra_libs` in `platform.local.txt` can inject the library path into the linker;
  however, wrapping as an Arduino library avoids needing `platform.local.txt` entirely.

**Alternatives considered**: Adding `.c` files directly to the sketch folder works but pollutes
the sketch directory with generated files. The Arduino library wrapper is cleaner and keeps the
distinction between authored and generated code.

## Decision 8 — Script Interrupt Mechanism (Emergency Escape)

**Decision**: A FreeRTOS timer (Core 1, 50ms period) polls BTN_UP (GPIO8) and BTN_DOWN (GPIO9) simultaneously while a Python app is running. When both read LOW at the same sample, `mp_sched_keyboard_interrupt()` is called. The running script receives `KeyboardInterrupt` at the next bytecode dispatch boundary. The `nlr_pop` handler in `BadgePython.cpp` catches it and returns to `loop()`. Timer is started at app launch and stopped before returning.

**Rationale** (per spec FR-008 and 2026-03-14 clarifications):
- **BTN_UP + BTN_DOWN chord is the emergency escape** — not BTN_RIGHT. BTN_RIGHT is the C++ menu system "cancel/back" button and the QR screen exit. Using BTN_RIGHT for Python escape would conflict with its existing role.
- `mp_sched_keyboard_interrupt()` is safe from any FreeRTOS timer callback.
- 50ms timer + bytecode-boundary delivery is well within the 1-second SC-005 requirement.
- Both `badge.exit()` (normal) and the chord (emergency) unwind to the same `nlr_pop` handler.

**Alternatives considered**:
- BTN_RIGHT as exit (prior plan): Superseded by the 2026-03-14 spec clarification that explicitly designated BTN_UP+BTN_DOWN as the emergency chord and kept BTN_RIGHT for the C++ menu.
- GPIO ISR: Detecting a two-button chord in ISR context requires shared state between two handlers — more complex than a polling timer.
- `mp_sched_vm_abort()`: requires `MICROPY_ENABLE_VM_ABORT`; no error message. Rejected.

## Decision 9 — `badge` Module API Surface (v1)

**Decision**: The `badge` module exposes all six subsystems clarified on 2026-03-14:
- **Display** (sub-object): `badge.display.clear()`, `badge.display.text(s, x, y)`, `badge.display.show()`
- **Input**: `badge.button_pressed(btn)` with named constants `badge.BTN_UP/DOWN/LEFT/RIGHT`; `badge.tilt_read()`
- **WiFi/HTTP**: `badge.http_get(url)`, `badge.http_post(url, body)` — raise on non-2xx
- **IR**: `badge.ir_send(addr, cmd)`, `badge.ir_available()`, `badge.ir_read()` → `(addr, cmd)` tuple; `badge.ROLE_*` constants
- **Hardware**: `badge.pin_read(n)`, `badge.pin_write(n, v)`, `badge.adc_read(n)`
- **Control**: `badge.gc_collect()`, `badge.exit()`

There is **no** `badge_api` module. HTTP is in `badge` directly. Display API uses `badge.display.*` sub-object — not top-level `badge.clear()`. Button API is `badge.button_pressed(badge.BTN_UP)` — not `badge.btn_up()`.

**Rationale**: The 2026-03-14 clarification session expanded the API scope beyond the initial "display + input only" assumption in the spec's first draft. The prior plan's `badge_api` module concept is superseded. The spec's FR-005 is authoritative.

## Decision 10 — App Filesystem Layout

**Decision**: Python apps are single `.py` files stored in `/apps/` on the badge VFS. The menu
item name is derived from the filename (minus the `.py` extension). A dedicated
`BadgePython_listApps()` function scans the directory.

**Rationale**: Per spec Assumptions section: "Python scripts are plain `.py` files stored in a
`/apps/` directory on the badge VFS filesystem, following the naming convention `<app_name>.py`."
The `manifest.json` format described in the constitution's Badge App Platform section is deferred
to a future spec (explicitly out of scope for v1 per the spec's Assumptions).

## Unresolved Issues

None. All "NEEDS CLARIFICATION" items from the spec have been resolved above.
