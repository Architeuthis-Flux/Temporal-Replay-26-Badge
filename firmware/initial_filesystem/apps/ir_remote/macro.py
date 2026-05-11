"""Macro recorder — capture up to 16 NEC frames with inter-frame gaps,
replay as a single sequence on demand. Useful for "TV on -> input HDMI2
-> volume +5" canned actions."""

import time

from badge import *
import badge_ui as ui

from ir_lib import IrSession, fmt_nec


MAX_STEPS = 16


def _draw(steps, recording):
    oled_clear()
    title = "Macro REC" if recording else "Macro"
    ui.chrome(title, "%d/%d" % (len(steps), MAX_STEPS),
              "X", "back", "B", "rec" if not recording else "stop")
    visible = 4
    start = max(0, len(steps) - visible)
    for i in range(visible):
        idx = start + i
        if idx >= len(steps):
            break
        addr, cmd, gap = steps[idx]
        ui.line(i, "%2d %s +%dms" % (idx + 1, fmt_nec(addr, cmd), gap))
    if not steps:
        ui.line(0, "(empty)")
    ui.inline_hint(0, 43, "A:play  Y:clear")
    oled_show()


def _record(steps):
    last_t = None
    ir_flush()
    while True:
        _draw(steps, recording=True)
        if button_pressed(BTN_CONFIRM) or button_pressed(BTN_BACK):
            return
        try:
            f = ir_nec_read()
        except Exception:
            f = None
        if f is not None and len(steps) < MAX_STEPS:
            addr, cmd, is_repeat = f
            now = time.ticks_ms()
            gap = 0 if last_t is None else max(0, time.ticks_diff(now, last_t))
            last_t = now
            if not is_repeat:
                steps.append((addr, cmd, gap))
        time.sleep_ms(20)


def _play(steps):
    if not steps:
        return
    for addr, cmd, gap in steps:
        if gap > 0:
            time.sleep_ms(min(gap, 5000))
        try:
            ir_nec_send(addr, cmd, 0)
        except Exception:
            pass
        time.sleep_ms(80)


def run():
    steps = []
    with IrSession("nec"):
        while True:
            _draw(steps, recording=False)
            if button_pressed(BTN_BACK):
                return
            if button_pressed(BTN_CONFIRM):
                _record(steps)
                continue
            if button_pressed(BTN_CIRCLE):
                _play(steps)
                ui.center(28, "played %d steps" % len(steps))
                oled_show()
                time.sleep_ms(900)
                continue
            if button_pressed(BTN_TRIANGLE):
                steps = []
                continue
            time.sleep_ms(40)
