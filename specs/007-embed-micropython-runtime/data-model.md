# Data Model: Embed MicroPython Scripting Runtime

**Branch**: `007-embed-micropython-runtime` | **Date**: 2026-03-13

## Entities

---

### 1. MicroPython Runtime

The embedded interpreter instance. A singleton per firmware run.

| Field | Type | Notes |
|-------|------|-------|
| `state` | `PythonRuntimeState` enum | See state transitions below |
| `heap` | `static uint8_t[MP_HEAP_SIZE]` | Stack-allocated in BSS; size from `BadgeConfig.h` |
| `heap_size` | `size_t` | Compile-time constant = `MP_HEAP_SIZE` |

**State transitions**:
```
PYTHON_UNINIT
  → (pythonInit() called during WiFi wait) → PYTHON_IDLE
  → (pythonExec() called by menu) → PYTHON_RUNNING
  → (exec returns normally) → PYTHON_IDLE (after deinit+reinit soft-reset)
  → (exec returns with exception) → PYTHON_FAULTED → PYTHON_IDLE (after deinit+reinit)
  → (init fails at boot) → PYTHON_DISABLED (no further state changes; all features disabled)
```

**Validation rules**:
- `pythonExec()` MUST NOT be called when state is `PYTHON_RUNNING` or `PYTHON_DISABLED`.
- `mp_embed_init()` MUST be called on Core 1 only.
- All `mp_*` / `gc_*` calls MUST occur on the task that called `mp_embed_init()`.

---

### 2. Badge Python App

A single `.py` source file on the badge VFS. Identified by filename. Source files live in `apps/` in the repository and are uploaded to the badge VFS at flash time.

| Field | Type | Notes |
|-------|------|-------|
| `name` | `String` | Filename without `.py` extension; used as menu display name |
| `path` | `String` | Full VFS path, e.g. `/apps/hello.py` |
| `src_buf` | `char*` | Heap-allocated buffer; read from VFS before exec; freed after exec |
| `src_len` | `size_t` | Byte count of source buffer |

**State transitions** (per app launch):
```
IDLE → LOADING (menu selection triggers file read from VFS)
LOADING → RUNNING (mp_embed_exec_str called with source buffer)
RUNNING → COMPLETED (exec returns — normal script end or badge.exit())
RUNNING → FAULTED (exec returns — unhandled exception)
RUNNING → INTERRUPTED (BTN_UP+BTN_DOWN chord → KeyboardInterrupt)
COMPLETED / FAULTED / INTERRUPTED → IDLE (deinit+reinit soft-reset; return to menu)
```

**Validation rules**:
- `src_len` MUST be ≤ `MP_SCRIPT_MAX_BYTES` (default 16 384) before allocation.
  If exceeded: display error, return to menu without executing.
- File MUST exist on VFS before exec. If missing: display error, return to menu.
- Invalid Python syntax is caught by `mp_embed_exec_str` (compile error) and returned as a fault.

---

### 3. Display Mutex

A FreeRTOS synchronization primitive that serializes OLED access. Already exists in the firmware.

| Field | Type | Notes |
|-------|------|-------|
| `displayMutex` | `SemaphoreHandle_t` | Defined in `BadgeDisplay.cpp`; created in `displayInit()` |

**Python integration rules**:
- Every `badge.display.*` function that calls a U8G2 draw operation MUST acquire `displayMutex`
  via `DISPLAY_TAKE()` before the call and release via `DISPLAY_GIVE()` after.
- `badge.display.show()` acquires the mutex for the full `sendBuffer()` call.
- `badge.display.clear()` and `badge.display.text()` each acquire and release per call.
- The display mutex is NOT exposed to Python; it is an internal concern of `badge_module.c`.

---

### 4. Python Module: `badge`

The single C extension module registered via `MP_REGISTER_MODULE`. All hardware and service access goes through this module. `badge.display` is a nested `mp_obj_module_t` created once at VM init (no per-call allocation). See `contracts/badge-module-api.md` for the full contract.

**Display sub-object** (`badge.display.*`) — all calls acquire `displayMutex`:

| Symbol | Python signature | C++ binding |
|--------|-----------------|-------------|
| `badge.display.clear()` | `() → None` | Mutex → `u8g2.clearBuffer()` |
| `badge.display.text(s, x, y)` | `(str, int, int) → None` | Mutex → `u8g2.drawStr(x, y, s)` |
| `badge.display.show()` | `() → None` | Mutex → `u8g2.sendBuffer()` |

**Input** — reads GPIO directly:

| Symbol | Python signature | C++ binding |
|--------|-----------------|-------------|
| `badge.button_pressed(btn)` | `(int) → bool` | `digitalRead(btn) == LOW` |
| `badge.BTN_UP/DOWN/LEFT/RIGHT` | `int` constants | GPIO 8/9/10/11 |
| `badge.tilt_read()` | `() → bool` | `digitalRead(TILT_PIN) == LOW` |

**WiFi/HTTP** — blocking, raise `OSError` on non-2xx or failure:

| Symbol | Python signature | C++ binding |
|--------|-----------------|-------------|
| `badge.http_get(url)` | `(str) → str` | Bridge → `BadgeAPI`; returns body string |
| `badge.http_post(url, body)` | `(str, str) → str` | Bridge → `BadgeAPI`; returns body string |

**IR** — ISR on Core 0 continues running; Python polls via bridge:

| Symbol | Python signature | C++ binding |
|--------|-----------------|-------------|
| `badge.ir_send(addr, cmd)` | `(int, int) → None` | `BadgeIR` bridge |
| `badge.ir_available()` | `() → bool` | `BadgeIR` bridge |
| `badge.ir_read()` | `() → tuple\|None` | Returns `(addr, cmd)` or `None` |
| `badge.ROLE_ATTENDEE/STAFF/VENDOR/SPEAKER` | `int` constants | Mirror C firmware role values |

**Hardware** — direct GPIO/ADC:

| Symbol | Python signature | C++ binding |
|--------|-----------------|-------------|
| `badge.pin_read(n)` | `(int) → int` | `digitalRead(n)` |
| `badge.pin_write(n, v)` | `(int, int) → None` | `digitalWrite(n, v)` |
| `badge.adc_read(n)` | `(int) → int` | `analogRead(n)` (0–4095) |

**Control**:

| Symbol | Python signature | C++ binding |
|--------|-----------------|-------------|
| `badge.gc_collect()` | `() → None` | `gc_collect()` |
| `badge.exit()` | `() → None` | `mp_raise_type(&mp_type_SystemExit)` |

**Notes**:
- Emergency escape (BTN_UP+BTN_DOWN chord) is handled by `BadgePython.cpp` FreeRTOS timer — not Python. Apps do not poll for it.
- `badge.display` is created once at `mp_embed_init()` time; no per-call allocation.
- Standard MicroPython built-ins: `utime`, `math`, `ujson`.

---

### 5. `mpconfigport.h` Feature Flags

Compile-time constants that configure the MicroPython embed build.

| Flag | Value | Reason |
|------|-------|--------|
| `MICROPY_ENABLE_COMPILER` | 1 | Required for `mp_embed_exec_str` (plain `.py` source files) |
| `MICROPY_ENABLE_SCHEDULER` | 1 | Required for `mp_sched_keyboard_interrupt()` |
| `MICROPY_KBD_EXCEPTION` | 1 | Enables `KeyboardInterrupt` scheduling |
| `MICROPY_PERSISTENT_CODE_LOAD` | 0 | Not needed — not using `.mpy` bytecode in v1 |
| `MICROPY_ENABLE_GC` | 1 | Required for heap allocation |
| `MICROPY_HELPER_REPL` | 0 | No REPL; script execution only |
| `MICROPY_PY_BUILTINS_FLOAT` | 1 | `float` type — needed for joystick values |
| `MICROPY_PY_UTIME` | 1 | `import utime` — `sleep_ms`, `ticks_ms` |
| `MICROPY_PY_MATH` | 1 | `import math` |
| `MICROPY_PY_UJSON` | 1 | `import ujson` — JSON serialization for HTTP bodies |
| `MICROPY_STACK_CHECK` | 1 | Runtime stack overflow detection |
| `MICROPY_ENABLE_VM_ABORT` | 0 | `mp_sched_keyboard_interrupt` is sufficient for v1 |

---

## Configuration Constants (new in `BadgeConfig.h.example`)

| Constant | Default | Notes |
|----------|---------|-------|
| `MICROPY_HEAP_SIZE` | `131072` (128 KB) | MicroPython GC heap; sized for ≤20ms GC pause on internal SRAM |
| `MP_SCRIPT_MAX_BYTES` | `16384` (16 KB) | Maximum `.py` source file size to load; enforced before malloc |
| `MP_APPS_DIR` | `"/apps"` | VFS root directory for Python apps |
