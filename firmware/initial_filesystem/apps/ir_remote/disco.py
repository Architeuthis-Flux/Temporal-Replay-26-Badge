"""Disco — LED matrix pulses on every received IR frame, OLED shows the
last frame as text. Set the badge near someone using a TV remote and the
matrix flashes in time with their button mash."""

import time

from badge import *
import badge_ui as ui

from ir_lib import IrSession, fmt_nec, raw_summary


PATTERNS = (
    (0b00011000, 0b00111100, 0b01111110, 0b11111111,
     0b11111111, 0b01111110, 0b00111100, 0b00011000),
    (0b10000001, 0b01000010, 0b00100100, 0b00011000,
     0b00011000, 0b00100100, 0b01000010, 0b10000001),
    (0b11111111, 0b10000001, 0b10000001, 0b10000001,
     0b10000001, 0b10000001, 0b10000001, 0b11111111),
)


def run():
    pat = 0
    last_event_t = 0
    last_label = "—"
    rx_count = 0
    modes = ("nec", "raw", "badge")
    mode_idx = 0
    led_override_begin()
    try:
        with IrSession(modes[mode_idx]):
            while True:
                now = time.ticks_ms()
                mode = modes[mode_idx]
                got = False
                try:
                    if mode == "nec":
                        f = ir_nec_read()
                        if f is not None:
                            got = True
                            addr, cmd, is_repeat = f
                            last_label = fmt_nec(addr, cmd) + (" R" if is_repeat else "")
                    elif mode == "raw":
                        buf = ir_raw_capture()
                        if buf:
                            got = True
                            last_label = raw_summary(buf)
                    else:
                        f = ir_read_words()
                        if f is not None:
                            got = True
                            last_label = "%dw" % len(f)
                except Exception:
                    pass

                if got:
                    rx_count += 1
                    last_event_t = now
                    pat = (pat + 1) % len(PATTERNS)
                    led_set_frame(PATTERNS[pat], 200)

                if last_event_t and time.ticks_diff(now, last_event_t) > 250:
                    led_clear()

                oled_clear()
                ui.chrome("Disco", mode + " ×" + str(rx_count),
                          "X", "back", "B", "mode")
                ui.center(20, last_label)
                ui.center(34, "(LED matrix on RX)")
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

                time.sleep_ms(40)
    finally:
        led_clear()
        led_override_end()
