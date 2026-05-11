"""Echo chamber — TX a random nonce, peer must reflect within 500 ms.

Score increments per round; miss resets the streak. Two-badge game."""

import os
import time

from badge import *
import badge_ui as ui

from ir_lib import IrSession


MAGIC = 0xBADE0000


def _rand():
    b = os.urandom(2)
    return (b[0] << 8) | b[1]


def run():
    streak = 0
    best = 0
    last_state = "press B to ping"
    pending = None
    pending_t = 0
    with IrSession("badge", power=20):
        while True:
            now = time.ticks_ms()
            oled_clear()
            ui.chrome("Echo chamber", "best %d" % best,
                      "X", "back", "B", "ping")
            ui.center(20, last_state)
            ui.center(34, "streak %d" % streak)
            oled_show()

            if button_pressed(BTN_BACK):
                return
            if button_pressed(BTN_CONFIRM) and pending is None:
                nonce = _rand()
                pending = nonce
                pending_t = now
                try:
                    ir_send_words([MAGIC | nonce])
                except Exception:
                    pass
                last_state = "waiting echo..."

            try:
                got = ir_read_words()
            except Exception:
                got = None
            if got is not None and len(got) >= 1:
                w = int(got[0]) & 0xFFFFFFFF
                if (w & 0xFFFF0000) == MAGIC:
                    seq = w & 0xFFFF
                    if pending is not None and seq == pending and \
                            time.ticks_diff(now, pending_t) <= 500:
                        streak += 1
                        if streak > best:
                            best = streak
                        last_state = "ECHO!"
                        pending = None
                    else:
                        # Reflect a peer's ping so they can play too.
                        try:
                            ir_send_words([MAGIC | seq])
                        except Exception:
                            pass

            if pending is not None and time.ticks_diff(now, pending_t) > 500:
                pending = None
                streak = 0
                last_state = "miss"

            time.sleep_ms(30)
