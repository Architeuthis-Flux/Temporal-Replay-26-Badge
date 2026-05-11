"""Shared helpers for IR Playground sub-apps.

Owns the mode set/restore guard, NEC frame pretty-printers, raw-symbol
buffer helpers, and the slot-binding JSON IO under /ir_remotes/.
"""

import os
import struct
import time

from badge import *
import badge_ui as ui


SLOTS_DIR = "/ir_remotes"
DEFAULT_LAYOUT = "default"


def _safe(callback, *args, **kwargs):
    try:
        return callback(*args, **kwargs)
    except Exception:
        return None


def ensure_slots_dir():
    try:
        os.stat(SLOTS_DIR)
    except OSError:
        try:
            os.mkdir(SLOTS_DIR)
        except OSError:
            pass


# ── Mode guard ──────────────────────────────────────────────────────────────


class IrSession:
    """Bring IR up, switch mode, restore on exit.

    Use as a context manager around any sub-app. Falls back gracefully if
    the firmware doesn't expose ir_set_mode (older builds)."""

    def __init__(self, mode="badge", power=None):
        self.mode = mode
        self.power = power
        self._prev_mode = None
        self._prev_power = None
        self._started = False

    def __enter__(self):
        ir_start()
        self._started = True
        try:
            self._prev_mode = ir_get_mode()
        except Exception:
            self._prev_mode = None
        try:
            self._prev_power = ir_tx_power()
        except Exception:
            self._prev_power = None
        if self._prev_mode is not None:
            try:
                ir_set_mode(self.mode)
            except Exception:
                pass
        if self.power is not None:
            try:
                ir_tx_power(int(self.power))
            except Exception:
                pass
        ir_flush()
        return self

    def __exit__(self, exc_type, exc, tb):
        try:
            ir_flush()
        except Exception:
            pass
        if self._prev_power is not None:
            _safe(ir_tx_power, self._prev_power)
        if self._prev_mode is not None:
            _safe(ir_set_mode, self._prev_mode)
        if self._started:
            _safe(ir_stop)
        return False


# ── NEC helpers ─────────────────────────────────────────────────────────────


def fmt_nec(addr, cmd):
    return "0x%02X / 0x%02X" % (addr & 0xFF, cmd & 0xFF)


def read_nec_frame():
    try:
        return ir_nec_read()
    except Exception:
        return None


def send_nec_frame(addr, cmd, repeats=0):
    try:
        ir_nec_send(int(addr) & 0xFF, int(cmd) & 0xFF, int(repeats))
        return True
    except Exception:
        return False


# ── Raw symbol helpers ──────────────────────────────────────────────────────


def raw_capture(timeout_ms=2000):
    """Wait up to timeout_ms for the next raw IR burst. Returns bytes or None."""
    deadline = time.ticks_add(time.ticks_ms(), int(timeout_ms))
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        try:
            buf = ir_raw_capture()
        except Exception:
            buf = None
        if buf:
            return buf
        time.sleep_ms(20)
    return None


def raw_pairs(buf):
    """Iterate (mark_us, space_us) pairs in a raw capture buffer."""
    if not buf:
        return
    n = len(buf) // 4
    for i in range(n):
        yield struct.unpack_from("<HH", buf, i * 4)


def raw_summary(buf):
    if not buf:
        return "empty"
    n = len(buf) // 4
    total_us = 0
    for mark, space in raw_pairs(buf):
        total_us += mark + space
    return "%d pairs, %d us" % (n, total_us)


def encode_pairs(pairs):
    """Build a raw capture buffer from an iterable of (mark, space) tuples."""
    out = bytearray()
    for mark, space in pairs:
        out += struct.pack("<HH", int(mark) & 0xFFFF, int(space) & 0xFFFF)
    return bytes(out)


def raw_send(buf, carrier_hz=38000):
    try:
        ir_raw_send(buf, int(carrier_hz))
        return True
    except Exception:
        return False


# ── Slot persistence ────────────────────────────────────────────────────────


def _slot_path(name):
    return SLOTS_DIR + "/" + name + ".txt"


def list_layouts():
    ensure_slots_dir()
    out = []
    try:
        for entry in os.listdir(SLOTS_DIR):
            if entry.endswith(".txt"):
                out.append(entry[:-4])
    except OSError:
        pass
    out.sort()
    return out


def _hex(buf):
    return "".join("{:02x}".format(b) for b in buf)


def _unhex(s):
    out = bytearray()
    for i in range(0, len(s), 2):
        out.append(int(s[i : i + 2], 16))
    return bytes(out)


def save_layout(name, slots):
    """slots is dict[str slot_id] -> dict with kind/data fields.

    Stored as a small text file with one line per slot. Kept simple
    so the badge can load it without a JSON parser per file.
    """
    ensure_slots_dir()
    lines = []
    for slot_id, value in slots.items():
        kind = value.get("kind", "")
        if kind == "nec":
            lines.append(
                "%s|nec|%d|%d|%d"
                % (
                    slot_id,
                    int(value.get("addr", 0)) & 0xFF,
                    int(value.get("cmd", 0)) & 0xFF,
                    int(value.get("repeats", 0)),
                )
            )
        elif kind == "raw":
            blob = value.get("data") or b""
            carrier = int(value.get("carrier_hz", 38000))
            lines.append("%s|raw|%d|%s" % (slot_id, carrier, _hex(blob)))
    try:
        with open(_slot_path(name), "w") as fh:
            fh.write("\n".join(lines) + "\n")
        return True
    except OSError:
        return False


def load_layout(name):
    out = {}
    try:
        with open(_slot_path(name), "r") as fh:
            data = fh.read()
    except OSError:
        return out
    for line in data.split("\n"):
        line = line.strip()
        if not line:
            continue
        parts = line.split("|")
        slot = parts[0]
        kind = parts[1] if len(parts) > 1 else ""
        if kind == "nec" and len(parts) >= 5:
            out[slot] = {
                "kind": "nec",
                "addr": int(parts[2]),
                "cmd": int(parts[3]),
                "repeats": int(parts[4]),
            }
        elif kind == "raw" and len(parts) >= 4:
            out[slot] = {
                "kind": "raw",
                "carrier_hz": int(parts[2]),
                "data": _unhex(parts[3]),
            }
    return out


def delete_layout(name):
    try:
        os.remove(_slot_path(name))
    except OSError:
        pass


# ── Cross-app UI bits ───────────────────────────────────────────────────────


def header_with_mode(title):
    try:
        mode = ir_get_mode()
    except Exception:
        mode = "?"
    ui.chrome(title, mode, "X", "back", "B", "ok")


def big_message(title, *lines):
    oled_clear()
    ui.center(2, title)
    y = 16
    for line in lines:
        ui.center(y, line)
        y += 11
    oled_show()


def wait_choice(timeout_ms=None):
    """Block until BTN_CONFIRM (True), BTN_BACK (False), or timeout (None)."""
    start = time.ticks_ms()
    while True:
        if button_pressed(BTN_CONFIRM):
            return True
        if button_pressed(BTN_BACK):
            return False
        if timeout_ms is not None and time.ticks_diff(time.ticks_ms(), start) > timeout_ms:
            return None
        time.sleep_ms(30)
