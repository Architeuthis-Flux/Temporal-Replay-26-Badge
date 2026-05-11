"""Spam scope — graph IR frames-per-second over time.

Useful for finding ambient interference (sunlight, plasma TVs, fluorescent
lighting, AC-remote spam) before relying on IR for messaging. Mode toggle
between badge / nec / raw so you can scope each protocol separately."""

import time

from badge import *
import badge_ui as ui

from ir_lib import IrSession


HISTORY = 96  # graph width in pixels
GRAPH_TOP = 14
GRAPH_H = 32


def run():
    history = [0] * HISTORY
    bucket = 0
    bucket_t = time.ticks_ms()
    modes = ("nec", "raw", "badge")
    mode_idx = 0
    peak = 1

    with IrSession(modes[mode_idx]):
        while True:
            now = time.ticks_ms()
            mode = modes[mode_idx]

            try:
                if mode == "nec":
                    while ir_nec_read() is not None:
                        bucket += 1
                elif mode == "raw":
                    while ir_raw_capture() is not None:
                        bucket += 1
                else:
                    while ir_read_words() is not None:
                        bucket += 1
            except Exception:
                pass

            if time.ticks_diff(now, bucket_t) >= 250:
                history.pop(0)
                history.append(bucket)
                if bucket > peak:
                    peak = bucket
                if peak > 1:
                    peak = max(1, peak - 1)
                bucket = 0
                bucket_t = now

            oled_clear()
            ui.chrome("Spam scope", mode + " ×4Hz",
                      "X", "back", "B", "mode")
            for i in range(HISTORY):
                v = history[i]
                if v <= 0:
                    continue
                bar_h = (v * GRAPH_H) // max(1, peak)
                if bar_h < 1:
                    bar_h = 1
                if bar_h > GRAPH_H:
                    bar_h = GRAPH_H
                x = 16 + i
                for k in range(bar_h):
                    oled_set_pixel(x, GRAPH_TOP + GRAPH_H - 1 - k, 1)
            ui.line(5, "peak %d / 250ms" % peak)
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
                history = [0] * HISTORY
                peak = 1
            time.sleep_ms(20)
