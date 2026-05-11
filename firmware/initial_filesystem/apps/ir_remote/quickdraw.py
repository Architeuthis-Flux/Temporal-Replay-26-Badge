"""Quick draw — IR duel. Wait for the on-screen "GO!" then mash B.
First TX sound wins the round."""

import os
import random
import time

from badge import *
import badge_ui as ui

from ir_lib import IrSession


MAGIC_GO = 0xC0FFEE00


def _state(msg):
    oled_clear()
    ui.chrome("Quick draw", None, "X", "back", None, None)
    ui.center(28, msg)
    oled_show()


def run():
    wins = 0
    losses = 0
    with IrSession("badge", power=20):
        while True:
            _state("Press B to start")
            while True:
                if button_pressed(BTN_BACK):
                    return
                if button_pressed(BTN_CONFIRM):
                    break
                time.sleep_ms(30)

            _state("steady...")
            ir_flush()
            wait_ms = 1500 + (os.urandom(1)[0] * 12)
            t_end = time.ticks_add(time.ticks_ms(), wait_ms)
            jumped = False
            while time.ticks_diff(t_end, time.ticks_ms()) > 0:
                if button_pressed(BTN_CONFIRM):
                    jumped = True
                    break
                time.sleep_ms(15)
            if jumped:
                losses += 1
                _state("False start! L:%d" % losses)
                time.sleep_ms(1200)
                continue

            _state("GO!")
            t0 = time.ticks_ms()
            done = False
            while not done:
                if time.ticks_diff(time.ticks_ms(), t0) > 2000:
                    losses += 1
                    _state("Too slow. L:%d" % losses)
                    time.sleep_ms(1200)
                    break
                if button_pressed(BTN_CONFIRM):
                    try:
                        ir_send_words([MAGIC_GO | (time.ticks_ms() & 0xFF)])
                    except Exception:
                        pass
                    rt = time.ticks_diff(time.ticks_ms(), t0)
                    wins += 1
                    _state("DRAW %d ms  W:%d" % (rt, wins))
                    time.sleep_ms(1500)
                    done = True
                    break
                try:
                    got = ir_read_words()
                except Exception:
                    got = None
                if got is not None and len(got) >= 1:
                    w = int(got[0]) & 0xFFFFFFFF
                    if (w & 0xFFFFFF00) == MAGIC_GO:
                        losses += 1
                        _state("Beaten! L:%d" % losses)
                        time.sleep_ms(1500)
                        done = True
                        break
                time.sleep_ms(5)
