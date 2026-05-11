"""Universal remote — capture an IR frame per slot and replay it later.

8 slots map to up/down/left/right + face X/Y/A/B. Each slot stores either
a NEC (addr, cmd) pair or a raw symbol buffer. Layouts are saved to
/ir_remotes/<name>.txt so the same physical hardware can swap personalities.
"""

import time

from badge import *
import badge_ui as ui

from ir_lib import (
    IrSession,
    delete_layout,
    fmt_nec,
    list_layouts,
    load_layout,
    raw_capture,
    raw_send,
    save_layout,
)


SLOTS = (
    ("UP",    BTN_UP),
    ("DOWN",  BTN_DOWN),
    ("LEFT",  BTN_LEFT),
    ("RIGHT", BTN_RIGHT),
    ("X",     BTN_CROSS),
    ("Y",     BTN_TRIANGLE),
    ("A",     BTN_CIRCLE),
    ("B",     BTN_SQUARE),
)


def _format_slot(slot):
    if not slot:
        return "—"
    if slot.get("kind") == "nec":
        return "NEC " + fmt_nec(slot["addr"], slot["cmd"])
    if slot.get("kind") == "raw":
        return "raw %dB" % len(slot.get("data") or b"")
    return "?"


def _pick_layout():
    layouts = list_layouts()
    if not layouts:
        return "default"
    cursor = 0
    while True:
        oled_clear()
        ui.chrome("Pick layout", str(cursor + 1) + "/" + str(len(layouts)),
                  "X", "new", "B", "use")
        visible = 4
        top = max(0, min(cursor - 1, len(layouts) - visible))
        for i in range(visible):
            idx = top + i
            if idx >= len(layouts):
                break
            if idx == cursor:
                ui.selected_row(i, layouts[idx])
            else:
                ui.line(i, layouts[idx])
        oled_show()
        if button_pressed(BTN_BACK):
            return "default"
        if button_pressed(BTN_CONFIRM):
            return layouts[cursor]
        if button_pressed(BTN_UP):
            cursor = (cursor - 1) % len(layouts)
        elif button_pressed(BTN_DOWN):
            cursor = (cursor + 1) % len(layouts)
        time.sleep_ms(40)


def _capture_slot(slot_label):
    """Listen for the next NEC frame; if none, fall back to raw."""
    oled_clear()
    ui.chrome("Capture " + slot_label, "nec", "X", "skip", None, None)
    ui.center(28, "Aim a remote at the")
    ui.center(40, "badge and press a key.")
    oled_show()

    deadline = time.ticks_add(time.ticks_ms(), 8000)
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        if button_pressed(BTN_BACK):
            return None
        try:
            f = ir_nec_read()
        except Exception:
            f = None
        if f is not None:
            addr, cmd, is_repeat = f
            if not is_repeat:
                return {"kind": "nec", "addr": addr, "cmd": cmd, "repeats": 0}
        time.sleep_ms(20)

    # NEC failed — try raw
    try:
        ir_set_mode("raw")
    except Exception:
        return None
    ir_flush()
    ui.center(40, "Trying raw...")
    oled_show()
    buf = raw_capture(timeout_ms=4000)
    try:
        ir_set_mode("nec")
    except Exception:
        pass
    if buf:
        return {"kind": "raw", "carrier_hz": 38000, "data": buf}
    return None


def _replay(slot):
    if not slot:
        return False
    if slot["kind"] == "nec":
        try:
            ir_nec_send(slot["addr"], slot["cmd"], slot.get("repeats", 0))
            return True
        except Exception:
            return False
    if slot["kind"] == "raw":
        try:
            ir_set_mode("raw")
            ok = raw_send(slot["data"], slot.get("carrier_hz", 38000))
            ir_set_mode("nec")
            return ok
        except Exception:
            return False
    return False


def _slot_pressed():
    for label, btn in SLOTS:
        if button_pressed(btn):
            return label
    return None


def _draw_main(layout, slots):
    oled_clear()
    ui.chrome("U-Remote", layout, "X", "back", "B", "save")
    rows = (
        ("UP",    "DOWN"),
        ("LEFT",  "RIGHT"),
        ("X",     "Y"),
        ("A",     "B"),
    )
    for i, (a, b) in enumerate(rows):
        a_state = "+" if slots.get(a) else "-"
        b_state = "+" if slots.get(b) else "-"
        ui.line(i, "%s%s  %s%s" % (a_state, a, b_state, b))
    oled_show()


def run():
    layout = _pick_layout()
    slots = load_layout(layout)
    with IrSession("nec"):
        capture_mode = False
        last_msg = ""
        last_msg_t = 0
        while True:
            now = time.ticks_ms()
            _draw_main(layout, slots)
            if last_msg and time.ticks_diff(now, last_msg_t) < 1500:
                ui.center(43, last_msg)
                oled_show()

            if button_pressed(BTN_BACK):
                return
            if button_pressed(BTN_CONFIRM):
                if save_layout(layout, slots):
                    last_msg = "saved " + layout
                else:
                    last_msg = "save failed"
                last_msg_t = time.ticks_ms()
                continue
            if button_pressed(BTN_PRESETS):
                capture_mode = not capture_mode
                last_msg = "capture: " + ("ON" if capture_mode else "OFF")
                last_msg_t = time.ticks_ms()
                continue

            slot = _slot_pressed()
            if slot is not None:
                if capture_mode:
                    captured = _capture_slot(slot)
                    if captured:
                        slots[slot] = captured
                        last_msg = slot + " bound"
                    else:
                        last_msg = slot + " no signal"
                    capture_mode = False
                    ir_flush()
                else:
                    if _replay(slots.get(slot)):
                        last_msg = "TX " + slot
                    else:
                        last_msg = slot + " empty"
                last_msg_t = time.ticks_ms()
                while _slot_pressed() == slot:
                    time.sleep_ms(20)

            time.sleep_ms(40)
