"""Sniffer — live dump of incoming IR frames, in NEC mode by default."""

import time

from badge import *
import badge_ui as ui

from ir_lib import IrSession, fmt_nec, raw_summary


def run():
    modes = ("nec", "raw", "badge")
    mode_idx = 0
    last_frame = "—"
    count = 0
    last_count_t = time.ticks_ms()
    fps = 0
    last_seen_t = 0

    with IrSession(modes[mode_idx]) as sess:
        while True:
            now = time.ticks_ms()

            mode = modes[mode_idx]
            try:
                if mode == "nec":
                    f = ir_nec_read()
                    if f is not None:
                        addr, cmd, is_repeat = f
                        count += 1
                        last_seen_t = now
                        last_frame = fmt_nec(addr, cmd) + (" R" if is_repeat else "")
                elif mode == "raw":
                    buf = ir_raw_capture()
                    if buf:
                        count += 1
                        last_seen_t = now
                        last_frame = raw_summary(buf)
                else:
                    f = ir_read_words()
                    if f is not None:
                        count += 1
                        last_seen_t = now
                        head = f[0] if f else 0
                        last_frame = "%dw [%08X]" % (len(f), head & 0xFFFFFFFF)
            except Exception as exc:
                last_frame = "err " + str(exc)[:18]

            if time.ticks_diff(now, last_count_t) >= 1000:
                fps = count
                count = 0
                last_count_t = now

            oled_clear()
            ui.chrome("Sniffer", mode, "X", "back", "B", "mode")
            ui.line(0, "Last:")
            ui.line(1, last_frame)
            ui.line(2, "rate: %d frames/s" % fps)
            quiet_ms = time.ticks_diff(now, last_seen_t) if last_seen_t else 0
            if last_seen_t == 0:
                ui.line(3, "(no traffic yet)")
            else:
                ui.line(3, "since: %d ms" % quiet_ms)
            oled_show()

            if button_pressed(BTN_BACK):
                return
            if button_pressed(BTN_CONFIRM):
                mode_idx = (mode_idx + 1) % len(modes)
                try:
                    ir_set_mode(modes[mode_idx])
                    ir_flush()
                except Exception:
                    pass
                last_frame = "—"
                count = 0
                last_seen_t = 0

            time.sleep_ms(30)
