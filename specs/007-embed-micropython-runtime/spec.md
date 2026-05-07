# Feature Specification: Embed MicroPython Scripting Runtime

**Feature Branch**: `007-embed-micropython-runtime`
**Created**: 2026-03-13
**Status**: Draft
**Input**: User description: "Embed MicroPython as a scripting runtime component inside the Arduino C++ firmware (firmware/Firmware-0308-modular/). The C++ firmware remains the host OS — MicroPython is initialized once during the boot sequence (while WiFi is connecting) and soft-reset between app launches. Target: ESP32-S3-MINI-1, Arduino ESP32 core 3.x on top of ESP-IDF 5.x. The interpreter must coexist with the existing FreeRTOS dual-core setup (IR task on Core 0, display/input on Core 1). MicroPython runs on Core 1. The display mutex must be respected when apps call display functions. Apps are loaded from the badge filesystem via mp_embed_exec_file or equivalent. The build system (build.sh/setup.sh) must be updated to include MicroPython as a dependency. No existing firmware behavior changes."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Python App Executes on Badge (Priority: P1)

A badge developer writes a Python script, copies it onto the badge filesystem, and selects it from the menu. The script runs to completion — drawing to the display, reading buttons, or doing simple logic — and when it exits the badge returns cleanly to the main menu.

**Why this priority**: This is the core value of the entire feature. Without a Python app running end-to-end, nothing else matters.

**Independent Test**: Copy a minimal "Hello, World" Python script to the badge filesystem, select it from the menu, and confirm the badge displays expected output then returns to the menu. Requires no other feature work.

**Acceptance Scenarios**:

1. **Given** a valid Python script is on the badge filesystem, **When** a user selects the corresponding menu item, **Then** the script executes and its output appears on the display within 2 seconds.
2. **Given** a running Python script calls a display function, **When** the call executes, **Then** the display updates correctly with no corruption or freeze.
3. **Given** a Python script reaches its end or calls `badge.exit()`, **When** execution completes, **Then** the badge returns to the main menu within 1 second and the interpreter is soft-reset.
4. **Given** a Python script is misbehaving and the user holds BTN_UP + BTN_DOWN, **When** the chord is detected, **Then** the firmware forcibly terminates the script, soft-resets the interpreter, and returns to the menu.

---

### User Story 2 - Firmware Boots Without Regression (Priority: P2)

A developer flashes the updated firmware. The badge boots normally — WiFi connects, the menu appears, IR works, and the display is responsive — exactly as before. The addition of the MicroPython runtime is invisible when no Python app is running.

**Why this priority**: Preserving all existing behavior is a hard constraint. Any regression blocks the feature from shipping.

**Independent Test**: Flash the new firmware and run through the existing badge feature checklist (IR boop, WiFi connect, menu navigation, NVS persistence) without launching any Python app.

**Acceptance Scenarios**:

1. **Given** the updated firmware is flashed, **When** the badge boots, **Then** the WiFi connection sequence completes in the same time window as before the MicroPython integration.
2. **Given** the badge is on the main menu, **When** the user uses IR to boop another badge, **Then** the boop registers correctly with no delay or interference.
3. **Given** no Python app has been launched, **When** the badge runs for 10 minutes, **Then** memory usage, display responsiveness, and IR reception are unchanged compared to the baseline firmware.

---

### User Story 3 - Crashed or Misbehaving App Recovers Cleanly (Priority: P3)

A Python script raises an unhandled exception or is written incorrectly. The badge does not lock up, corrupt its display, or require a hardware reset. The firmware catches the fault, resets the interpreter, and returns the user to the main menu.

**Why this priority**: Fault isolation is critical for a scripting runtime on a badge — apps will be written by third parties and may be buggy.

**Independent Test**: Deploy a Python script with a deliberate unhandled `raise Exception("crash")`. Select it from the menu and confirm the badge recovers to the main menu without a hard reset.

**Acceptance Scenarios**:

1. **Given** a Python script raises an unhandled exception, **When** the exception propagates, **Then** the badge displays a brief error message for up to 2 seconds, then returns to the main menu (total time ≤ 3 seconds from exception to menu).
2. **Given** a Python script is killed mid-execution (e.g., a specific hardware button press exits the app), **When** the exit is triggered, **Then** the interpreter is soft-reset and the badge returns to the menu within 1 second.
3. **Given** a Python script exhausts available scripting memory, **When** an allocation fails, **Then** the firmware catches the fault, resets the interpreter, and the badge returns to the menu.

---

### User Story 4 - Build From Clean Environment (Priority: P4)

A developer clones the repository and runs `setup.sh` followed by `build.sh` on a machine that has never built the firmware before. The MicroPython dependency is fetched and compiled automatically. The resulting binary is flashable and functions correctly.

**Why this priority**: Developer experience and reproducibility — the runtime must not require manual steps outside the documented build process.

**Independent Test**: Run `setup.sh` and `build.sh` in a clean environment (fresh clone, no pre-installed MicroPython artifacts) and confirm the binary is produced without errors.

**Acceptance Scenarios**:

1. **Given** a clean environment with only the Arduino toolchain installed, **When** `setup.sh` runs, **Then** all MicroPython dependencies are downloaded and prepared without manual intervention.
2. **Given** setup has completed, **When** `build.sh` runs, **Then** the firmware binary is produced successfully and includes the MicroPython runtime.
3. **Given** the produced binary is flashed to the badge, **When** the badge boots, **Then** Python app execution works as specified in User Story 1.

---

### Edge Cases

- Python script file is missing from the filesystem when the menu item is selected — firmware must report an error and return to the menu rather than hang.
- Python script is syntactically invalid — interpreter must surface a parse error and return cleanly.
- Python script attempts to call a display function concurrently with the C++ display loop — mutex prevents corruption; the Python call blocks briefly until the display is free.
- Python script runs an infinite loop — a designated hardware button must interrupt and terminate the app, returning to the menu.
- Python script file is larger than available interpreter heap — loading fails gracefully with an error message.
- MicroPython initialization fails at boot (heap unavailable, runtime corruption) — badge must continue booting normally with Python app support disabled, logging the failure.

## Clarifications

### Session 2026-03-14

- Q: How should the MicroPython VM run within the FreeRTOS scheduler? → A: Inline in `loop()` — apps are full-screen and formally exit; no FreeRTOS task created for the VM.
- Q: What IPC mechanism should button input and hardware events use from Python? → A: Direct C-backed polling via `badge` module functions; inline model is valid because WiFi/IR tasks run on Core 0 regardless. API scope expanded to include WiFi/HTTP, IR, GPIO/ADC.
- Q: Which hardware button terminates a running Python app? → A: BTN_UP + BTN_DOWN chord (emergency escape only); expected exit is `badge.exit()` or end-of-script.
- Q: How should the Hard C / Soft Python boundary be defined? → A: Explicit prohibited categories in C (ISRs, sub-ms timing, register access, crash-survivable ops, FreeRTOS primitives); `badge` module is the sole registered C extension; `machine` and `esp32` modules blocked.
- Q: What performance guardrails apply to MicroPython GC? → A: Fixed heap with ≤20ms GC pause budget enforced at build time, plus `badge.gc_collect()` exposed for app-developer-controlled collection at safe points.
- API scope review: Display API is namespaced (`badge.display.*`). HTTP raises on non-2xx, returns body string on success. `badge.ir_read()` returns `(addr, cmd)` tuple. Button and IR role constants are named (`badge.BTN_*`, `badge.ROLE_*`). Tilt sensor added as `badge.tilt_read()` (GPIO7).

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The firmware MUST initialize the MicroPython interpreter once during the boot sequence, during the existing WiFi connection window. Init MUST complete within 1 second (measured from `BadgePython::init()` call to return) and MUST NOT increase total boot time to main menu by more than 500ms compared to baseline. Verified by T026 timing audit.
- **FR-002**: The MicroPython interpreter MUST execute on Core 1, inline in `loop()` — no dedicated FreeRTOS task is created for the VM. While a Python app is running, `loop()` is blocked and all Core 1 C tasks (display loop, input scan) are suspended. This is acceptable because all Python apps are full-screen and formally exit before control returns to the badge menu.
- **FR-003**: The MicroPython interpreter MUST NOT interfere with the IR receiver task running on Core 0.
- **FR-004**: The firmware MUST load and execute Python scripts from the badge filesystem by filename when triggered by the menu system.
- **FR-005**: The `badge` Python module MUST expose the following subsystems as synchronous C-backed functions:
  - **Display** (namespaced): `badge.display.clear()`, `badge.display.text(s, x, y)`, `badge.display.show()`. Additional draw primitives (line, rect, pixel) may be added but these three are the v1 minimum. All calls acquire the shared display mutex before invoking U8G2. The `badge.display` sub-object is created once at VM init — no per-call allocation.
  - **Input**: `badge.button_pressed(btn)` reads GPIO/debounce state directly (C input scanner is suspended during Python execution). Button constants: `badge.BTN_UP`, `badge.BTN_DOWN`, `badge.BTN_LEFT`, `badge.BTN_RIGHT`. `badge.tilt_read()` returns current tilt state from GPIO7.
  - **WiFi/HTTP**: `badge.http_get(url)` and `badge.http_post(url, body)` — blocking calls backed by the existing `HTTPClient` C layer. Return the response body as a string on success (2xx). Raise a Python exception on non-2xx status or connection failure. WiFi stack continues on Core 0.
  - **IR**: `badge.ir_send(addr, cmd)` transmits an NEC frame. `badge.ir_available()` returns bool. `badge.ir_read()` returns a `(addr, cmd)` tuple. Role address constants (`badge.ROLE_ATTENDEE`, etc.) mirror those in the C firmware. IR ISR continues on Core 0.
  - **Hardware**: `badge.pin_read(n)`, `badge.pin_write(n, v)`, `badge.adc_read(n)` for direct GPIO and ADC access.
  - **GC control**: `badge.gc_collect()` forces an immediate MicroPython GC cycle at a known-safe point chosen by the app developer (e.g., before a blocking WiFi call or at the top of a draw loop)
- **FR-006**: The firmware MUST soft-reset the MicroPython interpreter between successive app launches, ensuring no Python state persists across apps.
- **FR-007**: The firmware MUST catch unhandled Python exceptions and script execution errors, display a brief error indicator, and return to the main menu without requiring a hardware reset.
- **FR-008**: The firmware MUST support an emergency escape chord — BTN_UP + BTN_DOWN held simultaneously — that forcibly terminates a running Python app and returns to the main menu. This is an unexpected-use escape hatch for misbehaving apps, not the intended exit path. The expected exit paths are `badge.exit()` (explicit) and natural end-of-script.
- **FR-013**: The `badge` Python module MUST expose a `badge.exit()` function that a Python app can call to formally terminate execution and return control to the C firmware and main menu. Calling `badge.exit()` MUST perform a clean soft-reset of the interpreter before returning to `loop()`.
- **FR-009**: All existing firmware behaviors — IR receive/transmit, WiFi connectivity, NVS persistence, display menu navigation, and HTTP API calls — MUST function identically after integrating the MicroPython runtime.
- **FR-010**: The `setup.sh` script MUST download and prepare the MicroPython runtime dependency automatically with no manual steps required.
- **FR-011**: The `build.sh` script MUST compile and link the MicroPython runtime into the firmware binary without manual configuration.
- **FR-012**: If MicroPython initialization fails at boot, the badge MUST continue to operate normally with all non-Python features available; Python app menu items MUST be disabled or marked unavailable.
- **FR-014**: The MicroPython VM MUST be configured with only the `badge` C module registered. Modules `machine`, `esp32`, and any other raw hardware or FFI modules MUST NOT be accessible from Python app scripts.

### Key Entities

- **MicroPython Runtime**: The embedded interpreter instance. Has a lifecycle: initialized once at boot, soft-reset between app launches, and optionally de-initialized on shutdown. Holds its own heap, separate from the Arduino/FreeRTOS heap.
- **Badge Python App**: A `.py` script file stored on the badge filesystem. Identified by filename. Has a lifecycle: loaded, executed, and exited (normally, via exception, or via interrupt). Each launch starts with a clean interpreter state.
- **Display Mutex**: A FreeRTOS synchronization primitive that serializes access to the OLED display. While a Python app runs inline in `loop()`, the C display loop is suspended; the mutex is still acquired by Python display calls for correctness in case ISR-level or Core 0 code accesses the display.
- **Python Module: `badge`**: The C extension module exposed to Python apps. Acts as the Hard C / Soft Python boundary. Namespaced display sub-object (`badge.display`), named button constants (`badge.BTN_*`, `badge.ROLE_*`), synchronous HTTP helpers (raise on non-2xx), IR tuple returns `(addr, cmd)`, tilt sensor (`badge.tilt_read()`), GPIO/ADC, `badge.exit()`, and `badge.gc_collect()`. All subsystem implementations live in C.

## Hard C / Soft Python Layer Boundary

The `badge` C module is the sole crossing point between layers. Python apps interact only through its API; they cannot directly access firmware internals.

**Hard C — must remain in C, prohibited in Python:**

- ISRs and interrupt handlers (IR receive ISR, button debounce ISR)
- PWM generation and any timing-critical output requiring precision < 1ms
- Direct peripheral register access (SPI, I2C bus transactions, GPIO matrix config)
- Any operation that must survive a Python crash or soft-reset: NVS writes, WiFi keepalive, IR receive buffering
- FreeRTOS primitive creation (tasks, queues, semaphores, mutexes) — Python may not create or destroy these
- Display driver calls below the mutex boundary (raw U8G2 calls without mutex acquisition)

**Soft Python — permitted in app scripts:**

- All functions exposed by the `badge` module: `badge.display.*`, `badge.button_pressed(badge.BTN_*)`, `badge.tilt_read()`, `badge.http_get()`, `badge.http_post()`, `badge.ir_send()`, `badge.ir_available()`, `badge.ir_read()`, `badge.pin_read()`, `badge.pin_write()`, `badge.adc_read()`, `badge.exit()`, `badge.gc_collect()`
- Standard MicroPython built-ins (math, collections, re, struct, time, etc.) as configured by the port
- Pure logic, state machines, UI rendering loops, data formatting

**Enforcement:** The `badge` module is the only C extension registered with the MicroPython VM. No other C modules are exposed. Python has no `import esp32`, `import machine`, or raw FFI access.

## Performance Guardrails

The MicroPython GC runs a stop-the-world collection when the heap is exhausted. While Core 1 C tasks are suspended during Python execution, Core 0 IR and WiFi tasks continue — an unexpectedly long GC pause can overflow the IRremote receive buffer before the Python app calls `badge.ir_read()`.

**Rules:**

- **Heap size**: set at build time in `BadgeConfig.h`; sized so worst-case GC pause ≤ 20ms (verified empirically during integration testing).
- **GC pause budget**: 20ms maximum. Any heap configuration that produces pauses exceeding this MUST be rejected.
- **`badge.gc_collect()`**: exposed in the `badge` module so app developers can force a collection at known-safe points (e.g., before a blocking WiFi call, at the top of a render loop). App dev documentation MUST include guidance on calling this proactively.
- **IR polling requirement**: Python apps that use IR MUST call `badge.ir_read()` within a polling interval ≤ 50ms to avoid IRremote buffer overflow. App dev documentation MUST state this constraint explicitly.
- **No Python-level allocation in tight loops**: app dev documentation MUST advise against allocating new objects (lists, dicts, strings) inside high-frequency draw loops; pre-allocate at script startup instead.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A Python "Hello, World" app loads and displays output within 2 seconds of being selected from the menu, on a badge that has been running for at least 5 minutes (no cold-boot advantage).
- **SC-002**: All existing firmware acceptance tests pass on the firmware binary that includes the MicroPython runtime — zero regressions.
- **SC-008**: `BadgePython::init()` completes within 1 second (logged via `Serial.printf("[BadgePython] init took %lums\n")`), and total boot time to main menu increases by ≤ 500ms compared to the pre-MicroPython baseline.
- **SC-003**: A Python app that raises an unhandled exception causes the badge to display a brief error message (up to 2 seconds) and return to the main menu within 3 seconds total, with no hard reset required.
- **SC-004**: Running `setup.sh` and `build.sh` on a clean machine produces a valid firmware binary in a single uninterrupted session, requiring no commands beyond what the scripts invoke.
- **SC-005**: A Python app that runs an infinite loop is terminable via button press, returning to the menu within 1 second of the button event.
- **SC-006**: After 10 successive Python app launches (each with a soft-reset), the badge remains stable — no memory exhaustion, display corruption, or firmware crash.
- **SC-007**: A Python app that allocates objects until GC triggers produces a measured GC pause ≤ 20ms on the target hardware (verified by T040 timing benchmark). A Python app that calls `badge.gc_collect()` before a `badge.display.*` render sequence then executes that sequence without triggering an automatic GC pause, verified by measuring `utime.ticks_ms()` delta across the sequence and confirming it matches the no-GC baseline (T040).

## Assumptions

- The Python v1 API surface covers: display drawing, button input, WiFi/HTTP, IR send/receive, and direct hardware access (GPIO, ADC). NVS persistence is out of scope for v1. Each subsystem is exposed via the `badge` C module as synchronous, C-backed functions that Python polls or calls directly. This is safe because WiFi, IR, and hardware peripheral tasks run on Core 0 and continue processing while Python blocks `loop()` on Core 1.
- The app exit mechanism for v1 has two tiers: (1) **Expected** — `badge.exit()` or natural end-of-script; app developers are expected to use one of these. (2) **Emergency** — BTN_UP + BTN_DOWN chord, for misbehaving apps only. A timeout-based forced exit is out of scope.
- Python scripts are plain `.py` files stored in a `/apps/` directory on the badge VFS filesystem, following the naming convention `<app_name>.py`. The menu item name maps directly to the filename.
- The MicroPython interpreter heap is allocated from the ESP32-S3's PSRAM or a dedicated internal SRAM region. Sizing is a build-time constant defined in `BadgeConfig.h`. The heap size MUST be chosen such that worst-case GC pause ≤ 20ms (verified empirically during integration testing).
- The `badge` Python module is the only custom C extension in v1. Standard MicroPython built-ins (math, collections, re, etc.) are available as configured by the MicroPython port.
- The MicroPython port used is the upstream `ports/embed` port, generating C sources via `micropython_embed.mk` that are compiled as part of the arduino-cli sketch build. This is distinct from the `esp-idf` standalone port; the embed port is specifically designed to be hosted inside an existing application.
