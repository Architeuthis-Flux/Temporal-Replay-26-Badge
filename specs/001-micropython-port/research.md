# Research: MicroPython Port for Temporal Badge DELTA

**Phase**: 0 | **Date**: 2026-03-10 | **Feature**: `001-micropython-port`

This document resolves all NEEDS CLARIFICATION items from the Technical Context and documents design decisions with rationale.

---

## Decision 1: NEC IR TX via esp32.RMT

**Question**: How should NEC IR transmission be implemented on MicroPython v1.24.0 ESP32-S3?

**Decision**: Use `esp32.RMT` in TX mode with a 38 kHz carrier.

**Implementation sketch**:
```python
import esp32, machine

class NECTX:
    # RMT at 1 µs/tick: clock_div = 80 (80 MHz / 80 = 1 MHz)
    CLOCK_DIV = 80
    CARRIER_FREQ = 38_000   # Hz
    CARRIER_DUTY = 50       # %

    # NEC timing in µs (round to nearest tick)
    LEADER_BURST = 9000
    LEADER_SPACE = 4500
    BIT_BURST    = 562
    BIT_0_SPACE  = 562
    BIT_1_SPACE  = 1688
    END_BURST    = 562

    def __init__(self, pin_num):
        self._rmt = esp32.RMT(
            0,
            pin=machine.Pin(pin_num),
            clock_div=self.CLOCK_DIV,
            tx_carrier=(self.CARRIER_FREQ, self.CARRIER_DUTY, 1),
            idle_level=False,
        )

    def _encode_frame(self, address, command):
        """Return flat tuple of (high, low, high, low, ...) durations for one NEC frame."""
        pulses = [self.LEADER_BURST, self.LEADER_SPACE]
        byte_sequence = [address, ~address & 0xFF, command, ~command & 0xFF]
        for byte in byte_sequence:
            for bit in range(8):
                pulses.append(self.BIT_BURST)
                pulses.append(self.BIT_1_SPACE if (byte >> bit) & 1 else self.BIT_0_SPACE)
        pulses.append(self.END_BURST)
        pulses.append(0)   # trailing 0 stops the RMT
        return tuple(pulses)

    def send(self, address, command):
        self._rmt.write_pulses(self._encode_frame(address, command), start=1)
        self._rmt.wait_done(timeout=100)
```

**Rationale**: RMT is the correct hardware peripheral for generating precise IR timing. The `tx_carrier` parameter generates the 38 kHz modulation automatically — no software bit-banging needed. This matches how the Arduino `IRremote` library uses the same peripheral internally.

**Alternatives considered**:
- UART-based IR (as in shy-micropython-build): Does not generate carrier; only works if the IR LED driver circuits include their own oscillator. The DELTA hardware uses a raw LED + driver, so carrier must be generated in software/hardware. **Rejected**.
- Pure Python bit-banging: Timing jitter from MicroPython GIL and GC is too large for NEC (± few µs tolerance). **Rejected**.

---

## Decision 2: NEC IR RX via GPIO IRQ pulse measurement

**Question**: Should RX use `esp32.RMT` receive mode or GPIO interrupts?

**Decision**: Use `machine.Pin.irq` on both RISING and FALLING edges + `utime.ticks_us()` for pulse width measurement. This runs in a dedicated `_thread` thread.

**Rationale**: The IR_RX_PIN (GPIO4) connects to a **TSOP demodulator** which outputs an already-demodulated digital signal (active-low when carrier detected). The RMT receive mode in MicroPython v1.24.0 on ESP32-S3 has a known limitation: the Python-level `wait_done()` / `read_pulses()` API blocks until the RMT DMA buffer is full or a timeout occurs, which prevents real-time multi-frame reassembly. A GPIO IRQ approach:

1. Fires an ISR on every edge — records `(edge_type, ticks_us)`.
2. A second thread (not an ISR) drains the edge queue and decodes NEC timing.
3. This separates interrupt latency (ISR, must be fast) from NEC logic (thread, can block safely).

```python
import machine, utime, _thread

class NECRX:
    BIT_THRESHOLD_US = 1000  # < 1000 µs = bit 0, > 1000 µs = bit 1

    def __init__(self, pin_num, on_frame):
        self._pin = machine.Pin(pin_num, machine.Pin.IN)
        self._on_frame = on_frame   # callback(address, command)
        self._edges = []            # [(level, ticks_us)] — filled by ISR
        self._last_us = 0

        self._pin.irq(self._isr, machine.Pin.IRQ_RISING | machine.Pin.IRQ_FALLING,
                      hard=True)

    def _isr(self, pin):
        now = utime.ticks_us()
        self._edges.append((pin.value(), now))

    def decode_loop(self):
        """Call from a background thread. Drains edge queue and decodes NEC."""
        state = 'idle'
        bits  = []
        last_fall = 0
        while True:
            if self._edges:
                level, t = self._edges.pop(0)
                dt = utime.ticks_diff(t, self._last_us)
                self._last_us = t
                # NEC decode state machine (leader + 32 bits)
                # ... (detailed in ir_nec.py)
            utime.sleep_ms(1)
```

**Alternatives considered**:
- `esp32.RMT` receive: Simpler API but blocks per-frame; multi-frame reassembly requires polling a buffer. MicroPython v1.24.0 RMT receive does not expose per-frame callbacks, making the 500 ms timeout guard between UID bytes awkward to implement. **Acceptable but more complex than GPIO IRQ for multi-frame use case**.
- Single-thread polling (`Pin.value()` in a tight loop): Prevents the main loop from running, violates FR-013. **Rejected**.

**Thread safety**: The `_edges` list is modified in an ISR and consumed in a thread. MicroPython ISRs on ESP32 are micropython task preemptive; `list.append()` and `list.pop(0)` are individually atomic at the MicroPython level. For safety, a fixed-size ring buffer (using `bytearray`) is preferred in the final `ir_nec.py` implementation to avoid heap allocation in the ISR.

---

## Decision 3: XBM → Python bytes literal conversion

**Question**: How should `graphics.h` PROGMEM arrays be converted to `graphics.py` Python bytes?

**Decision**: Manual one-time conversion via a short Python script run during development. Output is a `graphics.py` file with named `bytes` literals checked into the repo.

**Rationale**: The conversion is a one-time operation. The PROGMEM arrays in `graphics.h` are already in the XBM byte format (LSB-first, row-major). The conversion is:

```python
# converter run once on dev machine (not part of badge runtime)
import re

def progmem_to_bytes(c_array_literal: str) -> bytes:
    hex_values = re.findall(r'0x[0-9a-fA-F]+', c_array_literal)
    return bytes(int(v, 16) for v in hex_values)
```

The resulting `graphics.py` contains:

```python
# graphics.py — XBM bitmaps translated from firmware/Firmware-0306/graphics.h
# All arrays are 1024 bytes (128×64 pixels, MONO_HLSB format) unless noted.

GRAPHICS_BASE = bytes([0x00, 0x00, ...])     # 1024 bytes
MOCK_QR       = bytes([...])                 # 1024 bytes
MOCK_NAMETAG  = bytes([...])                 # 1024 bytes

# Arrow sprites (small, used as OLED overlays)
DOWN_ARROW_FILLED = bytes([0x10, 0x00, 0x10, 0x00, 0x7c, 0x00,
                            0x38, 0x00, 0x10, 0x00, 0x00, 0x00])  # 9×6
DOWN_ARROW        = bytes([0x10, 0x00, 0x10, 0x00, 0x7c, 0x00,
                            0x38, 0x00, 0x10, 0x00, 0x00, 0x00])  # 9×6
UP_ARROW_FILLED   = bytes([0xef, 0x01, 0xc7, 0x01, 0x83, 0x01,
                            0xef, 0x01, 0xef, 0x01, 0xff, 0x01])  # 9×6
UP_ARROW          = bytes([0x10, 0x00, 0x38, 0x00, 0x7c, 0x00,
                            0x10, 0x00, 0x10, 0x00, 0x00, 0x00])  # 9×6
```

**Display rendering**: MicroPython `framebuf.FrameBuffer` with `framebuf.MONO_HLSB` is the correct format. XBM is LSB-first which matches `MONO_HLSB`. No bit-reversal needed.

**Alternatives considered**:
- Run-time XBM parsing (parse hex text): Slower and wastes RAM. The `graphics.h` arrays are already in binary; including them as bytes literals is zero-runtime-overhead. **Rejected**.
- Storing as `.bin` files on VFS: Adds flash.sh complexity and VFS file-open overhead at boot. Bytes literals in a frozen module would be better — but for BYPASS mode (development only), VFS is fine. **Acceptable but not chosen** — the spec says graphics.py is a VFS file (per FR-003), so bytes literal in a Python file is the right approach.

---

## Decision 4: Threading model for IR RX

**Question**: How should IR RX concurrency be structured (MicroPython threading options)?

**Decision**: `_thread.start_new_thread(ir_rx_loop, ())` launched from `main.py` before entering the main event loop. The IR RX loop runs on Core 0 (same as the main thread) with a 1 ms sleep yielding back to the MicroPython scheduler.

**Rationale**:
- MicroPython's `_thread` is cooperative on single-core configurations but preemptive on ESP32's dual-core (Core 0 / Core 1). The main loop runs on Core 1 by default; the IR thread can run on Core 0.
- This mirrors the Arduino `xTaskCreatePinnedToCore(irReceiveTask, ..., 0)` pattern exactly.
- Shared state between threads: a Python list `_received_uids` protected by a `_thread.allocate_lock()` mutex.

**Thread structure**:
```python
import _thread

_rx_lock = _thread.allocate_lock()
_received_uids = []   # protected by _rx_lock

def _ir_rx_loop(rx):
    """Runs on background thread. rx is a NECRX instance."""
    rx.decode_loop()   # loops forever; calls _on_frame callback

def start_ir_rx(ir_rx_pin):
    def on_frame(addr, cmd):
        if addr != 0x42:
            return
        # accumulate 6 bytes → uid string
        ...
        with _rx_lock:
            _received_uids.append(uid_hex)

    rx = NECRX(ir_rx_pin, on_frame)
    _thread.start_new_thread(_ir_rx_loop, (rx,))
```

**Alternatives considered**:
- `asyncio` (uasyncio): MicroPython's asyncio does not support true parallelism; a long IR decode loop would block the event loop. **Rejected**.
- Polling IR state in the main loop: Fails FR-013 ("run concurrently"). **Rejected**.

---

## Decision 5: `machine.unique_id()` format on ESP32-S3

**Question**: What does `machine.unique_id()` return on ESP32-S3-MINI-1, and is it always 6 bytes?

**Decision**: On ESP32-S3, `machine.unique_id()` returns 6 bytes derived from the MAC address base (same as `esp_efuse_mac_get_default`). Always 6 bytes on this chip. Badge ID = `''.join(f'{b:02x}' for b in machine.unique_id())` → 12 lowercase hex characters.

**Safety**: The spec says to zero-pad if fewer than 6 bytes are returned. Implement as:
```python
uid_bytes = machine.unique_id()
uid_hex = ''.join(f'{b:02x}' for b in uid_bytes).ljust(12, '0')[:12]
```

**Rationale**: Consistent with Firmware-0306 which reads `ESP_EFUSE_OPTIONAL_UNIQUE_ID` (a different eFuse field — 128-bit chip ID). However, since badge_crypto is out of scope, `machine.unique_id()` is used as a reasonable 6-byte device identifier available without a C module. The display messages show the 12-char hex ID. This is sufficient for the port's scope (IR pairing, display, BYPASS mode); HMAC-authenticated requests require the proper UUID from NVS, which is deferred to the badge_crypto spec.

---

## Decision 6: Joystick ADC normalization

**Question**: How to normalize the 12-bit ADC joystick readings to ±1.0?

**Decision**: `machine.ADC(pin, atten=machine.ADC.ATTN_11DB)` gives 0–4095. Center is ~2048. Normalize: `norm = (raw - 2048) / 2048.0`, then clamp to circle: if `sqrt(nx²+ny²) > 1.0`, scale down.

```python
import machine, math

joy_x = machine.ADC(machine.Pin(JOY_X_PIN), atten=machine.ADC.ATTN_11DB)
joy_y = machine.ADC(machine.Pin(JOY_Y_PIN), atten=machine.ADC.ATTN_11DB)

def read_joy():
    nx = (joy_x.read() - 2048) / 2048.0
    ny = (joy_y.read() - 2048) / 2048.0
    dist = math.sqrt(nx*nx + ny*ny)
    if dist > 1.0:
        nx /= dist
        ny /= dist
    return nx, ny
```

The display maps (nx, ny) to a 3×3 dot within a 6-pixel-radius circle centered at (100, 53) — matching the Arduino `JOY_CIRCLE_CX=100`, `JOY_CIRCLE_CY=53`, `JOY_CIRCLE_R=6`.

**Attenuation**: `ATTN_11DB` extends range to ~3.3V (full rail), needed if joystick rail voltage is 3.3V. If joystick center reads off-center due to component tolerances, the ±2048 normalization compensates adequately for display purposes (no calibration procedure required for the conference use case).

---

## Decision 7: BYPASS mode structure

**Question**: How should BYPASS mode be structured across boot.py / main.py?

**Decision**: `BYPASS = True/False` in `config.py` (VFS, not frozen). Boot.py and main.py both import it. When `BYPASS = True`:
- WiFi connect: skip, display "Connected (bypass)" after `BYPASS_DELAY_MS` ms
- NTP sync: skip
- QR fetch: skip, display `graphics.MOCK_QR` for `QR_DISPLAY_MS` ms
- Nametag fetch: skip, display `graphics.MOCK_NAMETAG`
- Badge ID: from `machine.unique_id()` as normal (still works offline)
- HTTP requests: skip all

This matches Firmware-0306's `if (BYPASS) { delay(BYPASS_DELAY_MS); ... }` pattern.

**Rationale**: BYPASS mode must be achievable without a network, server, or badge_crypto enrollment. Using a flag in config.py (VFS-resident) means the developer can toggle it without rebuilding firmware.

---

## Resolved unknowns summary

| Unknown | Resolution |
|---------|-----------|
| NEC TX implementation | `esp32.RMT` with `tx_carrier` for 38 kHz modulation |
| NEC RX implementation | GPIO `machine.Pin.irq` + `_thread`, ring buffer for ISR safety |
| XBM conversion | One-time Python script → `graphics.py` bytes literals |
| IR concurrency | `_thread.start_new_thread` on Core 0; `allocate_lock()` for shared UID list |
| Badge ID source | `machine.unique_id()` → 6 bytes → 12-char hex, zero-padded if short |
| Joystick normalization | `machine.ADC` ATTN_11DB, 0–4095 → ±1.0, circle clamp |
| BYPASS mode | `config.py` flag, skips all network calls, uses `graphics.py` mock data |
| Board definition source | Adapt from `shy-micropython-build` patterns; re-verify all pins against constitution table |
