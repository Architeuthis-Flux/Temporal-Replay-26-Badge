"""Power sweep — auto-find the minimum reliable TX power against a peer.

Walks LEVELS upward, sending a fixed number of probes at each level and
counting peer reflections. Stops at the first level that hits the success
threshold. Useful for setting `ir_tx_power(...)` for low-power badge
mini-games where you DON'T want to bother distant badges."""

import time

from badge import *
import badge_ui as ui

from ir_lib import IrSession


LEVELS = (1, 2, 3, 5, 8, 12, 18, 25, 33, 45)
PROBES = 8
THRESHOLD = 6  # fraction of PROBES that must reflect to "pass"


def run():
    found = None
    rows = []
    with IrSession("badge"):
        for level in LEVELS:
            try:
                ir_tx_power(level)
            except Exception:
                pass
            ir_flush()
            hits = 0
            for n in range(PROBES):
                try:
                    ir_send_words([0xBA00C400 | (level << 8) | n])
                except Exception:
                    pass
                deadline = time.ticks_add(time.ticks_ms(), 300)
                while time.ticks_diff(deadline, time.ticks_ms()) > 0:
                    try:
                        got = ir_read_words()
                    except Exception:
                        got = None
                    if got is not None:
                        hits += 1
                        break
                    time.sleep_ms(10)
            rows.append((level, hits))

            oled_clear()
            ui.chrome("Power sweep", "%d%% %d/%d" % (level, hits, PROBES),
                      "X", "stop", None, None)
            visible = 5
            top = max(0, len(rows) - visible)
            for i in range(visible):
                idx = top + i
                if idx >= len(rows):
                    break
                lv, h = rows[idx]
                ok = "✓" if h >= THRESHOLD else "·"
                ui.line(i, "%2d%%  %d/%d  %s" % (lv, h, PROBES, ok))
            oled_show()

            if hits >= THRESHOLD and found is None:
                found = level
                break
            if button_pressed(BTN_BACK):
                return

        oled_clear()
        ui.chrome("Power sweep", "done", "X", "back", None, None)
        if found:
            ui.center(22, "min reliable")
            ui.center(34, str(found) + "%")
        else:
            ui.center(22, "no level reached")
            ui.center(34, str(THRESHOLD) + "/" + str(PROBES))
        oled_show()
        while not button_pressed(BTN_BACK) and not button_pressed(BTN_CONFIRM):
            time.sleep_ms(40)
