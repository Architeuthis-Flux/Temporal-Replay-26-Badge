"""Range probe — sweep TX power 1..50% and plot how many of N probes
each level lands on a peer. Run on the TX badge; the RX badge should be
in Bouncer or any badge-mode listener that does NOT reflect probes."""

import time

from badge import *
import badge_ui as ui

from ir_lib import IrSession


PROBE_MAGIC = 0xBA00C300
LEVELS = (1, 3, 5, 10, 20, 33, 50)
PROBES_PER_LEVEL = 6


def _bar(x, y, w, h, frac):
    ui.frame(x, y, w, h)
    fill_w = max(0, min(w - 2, int((w - 2) * frac)))
    if fill_w:
        ui.fill(x + 1, y + 1, fill_w, h - 2)


def run():
    results = []
    with IrSession("badge"):
        oled_clear()
        ui.chrome("Range probe", "—", "X", "back", "B", "go")
        ui.center(20, "Sweeps TX power.")
        ui.center(32, "Aim at a Bouncer peer.")
        oled_show()
        while True:
            if button_pressed(BTN_BACK):
                return
            if button_pressed(BTN_CONFIRM):
                break
            time.sleep_ms(40)

        for level in LEVELS:
            try:
                ir_tx_power(level)
            except Exception:
                pass
            ir_flush()
            hits = 0
            for n in range(PROBES_PER_LEVEL):
                try:
                    ir_send_words([PROBE_MAGIC | (level << 8) | n])
                except Exception:
                    pass
                # Wait for the reflection (Bouncer reflects all PINGs)
                deadline = time.ticks_add(time.ticks_ms(), 350)
                while time.ticks_diff(deadline, time.ticks_ms()) > 0:
                    try:
                        got = ir_read_words()
                    except Exception:
                        got = None
                    if got is not None:
                        hits += 1
                        break
                    time.sleep_ms(15)
            results.append((level, hits))

            oled_clear()
            ui.chrome("Range probe",
                      "%d/%d" % (len(results), len(LEVELS)),
                      "X", "stop", None, None)
            for i, (lv, h) in enumerate(results):
                ui.text(0, 10 + i * 6, "%2d%%" % lv)
                _bar(22, 10 + i * 6, 100, 5, h / float(PROBES_PER_LEVEL))
                ui.text(106, 10 + i * 6, "%d/%d" % (h, PROBES_PER_LEVEL))
            oled_show()

            if button_pressed(BTN_BACK):
                return

        ui.center(56, "B:exit")
        oled_show()
        while not button_pressed(BTN_CONFIRM) and not button_pressed(BTN_BACK):
            time.sleep_ms(50)
