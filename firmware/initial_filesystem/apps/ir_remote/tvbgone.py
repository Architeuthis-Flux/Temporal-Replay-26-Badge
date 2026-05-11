"""TV-B-Gone — cycle through bundled NEC TV power-off codes.

Limited to NEC vendors. Sony / RC5 / RC6 TVs are unreachable from this
sub-app — the on-screen footer makes that clear so users don't expect
universal coverage.
"""

import time

from badge import *
import badge_ui as ui

from ir_lib import IrSession

from data.tvbgone_codes import CODES


def _confirm():
    oled_clear()
    ui.chrome("TV-B-Gone", str(len(CODES)) + " codes",
              "X", "cancel", "B", "fire")
    ui.center(16, "Spam every TV in")
    ui.center(28, "range with NEC")
    ui.center(40, "power-off codes?")
    oled_show()
    while True:
        if button_pressed(BTN_BACK):
            return False
        if button_pressed(BTN_CONFIRM):
            return True
        time.sleep_ms(30)


def run():
    if not _confirm():
        return
    with IrSession("nec", power=50):
        i = 0
        paused = False
        while i < len(CODES):
            if button_pressed(BTN_BACK):
                return
            if button_pressed(BTN_CONFIRM):
                paused = not paused
                while button_pressed(BTN_CONFIRM):
                    time.sleep_ms(20)
            if paused:
                ui.chrome("TV-B-Gone (paused)",
                          "%d/%d" % (i + 1, len(CODES)),
                          "X", "back", "B", "go")
                vendor, addr, cmd = CODES[i]
                ui.center(28, vendor)
                ui.center(40, "0x%02X / 0x%02X" % (addr, cmd))
                oled_show()
                time.sleep_ms(60)
                continue

            vendor, addr, cmd = CODES[i]
            oled_clear()
            ui.chrome("TV-B-Gone",
                      "%d/%d" % (i + 1, len(CODES)),
                      "X", "back", "B", "pause")
            ui.center(20, vendor)
            ui.center(32, "0x%02X / 0x%02X" % (addr, cmd))
            bar_w = (i * 124) // len(CODES)
            ui.fill(2, 46, max(1, bar_w), 4)
            ui.frame(2, 46, 124, 4)
            ui.inline_hint(0, 53, "NEC only")
            oled_show()

            try:
                ir_nec_send(addr, cmd, 2)
            except Exception:
                pass
            time.sleep_ms(220)
            i += 1

        ui.center(28, "Done.")
        oled_show()
        time.sleep_ms(900)
