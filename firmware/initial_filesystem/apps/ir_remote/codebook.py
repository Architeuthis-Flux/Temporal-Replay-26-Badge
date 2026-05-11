"""Codebook — curated NEC vendor menu (TV / amp / set-top boxes).

Joystick navigates Vendor -> Category -> Button. Confirm transmits.
"""

import time

from badge import *
import badge_ui as ui

from ir_lib import IrSession

from data.nec_codes import VENDORS


def _menu(items, title, hint_label):
    cursor = 0
    while True:
        oled_clear()
        ui.chrome(title, "%d/%d" % (cursor + 1, len(items)),
                  "X", "back", "B", hint_label)
        visible = 5
        top = max(0, min(cursor - 2, len(items) - visible))
        for i in range(visible):
            idx = top + i
            if idx >= len(items):
                break
            label = items[idx]
            if idx == cursor:
                ui.selected_row(i, label)
            else:
                ui.line(i, label)
        oled_show()
        if button_pressed(BTN_BACK):
            return -1
        if button_pressed(BTN_CONFIRM):
            return cursor
        if button_pressed(BTN_UP):
            cursor = (cursor - 1) % len(items)
            time.sleep_ms(120)
        elif button_pressed(BTN_DOWN):
            cursor = (cursor + 1) % len(items)
            time.sleep_ms(120)
        time.sleep_ms(40)


def run():
    with IrSession("nec"):
        while True:
            v_idx = _menu([v[0] for v in VENDORS], "Vendor", "ok")
            if v_idx < 0:
                return
            cats = VENDORS[v_idx][1]
            while True:
                c_idx = _menu([c[0] for c in cats], VENDORS[v_idx][0], "ok")
                if c_idx < 0:
                    break
                buttons = cats[c_idx][1]
                cursor = 0
                while True:
                    oled_clear()
                    label, addr, cmd = buttons[cursor]
                    ui.chrome(VENDORS[v_idx][0] + "/" + cats[c_idx][0],
                              "%d/%d" % (cursor + 1, len(buttons)),
                              "X", "back", "B", "send")
                    ui.center(20, label)
                    ui.center(33, "0x%02X / 0x%02X" % (addr, cmd))
                    oled_show()
                    if button_pressed(BTN_BACK):
                        break
                    if button_pressed(BTN_CONFIRM):
                        try:
                            ir_nec_send(addr, cmd, 1)
                        except Exception:
                            pass
                        ui.center(43, "TX")
                        oled_show()
                        time.sleep_ms(200)
                    if button_pressed(BTN_UP):
                        cursor = (cursor - 1) % len(buttons)
                        time.sleep_ms(120)
                    elif button_pressed(BTN_DOWN):
                        cursor = (cursor + 1) % len(buttons)
                        time.sleep_ms(120)
                    time.sleep_ms(40)
