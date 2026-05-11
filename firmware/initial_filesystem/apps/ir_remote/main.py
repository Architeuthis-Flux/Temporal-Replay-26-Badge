"""IR Playground — multitool launcher.

Sub-apps live as sibling modules. Each one is a `run()` function that owns
its OLED + LED + IR session and returns when the user presses BACK.
"""

import sys
import time

APP_DIR = "/apps/ir_remote"
if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

from badge import *
from badge_app import run_app
import badge_ui as ui


SUB_APPS = (
    ("Sniffer",       "sniffer",     "Live IR frame dump"),
    ("Inspector",     "inspector",   "Single-shot timing scope"),
    ("Universal",     "uremote",     "8-slot remote: bind + replay"),
    ("Macro",         "macro",       "Record/replay frame sequences"),
    ("Codebook",      "codebook",    "Curated NEC vendor menu"),
    ("TV-B-Gone",     "tvbgone",     "Cycle NEC TV power-off codes"),
    ("Bouncer",       "bouncer",     "Peer ping-pong + latency"),
    ("Range probe",   "range_probe", "Sweep TX power vs success"),
    ("Spam scope",    "spam_scope",  "Frames-per-second meter"),
    ("Power sweep",   "power_sweep", "Auto-find min reliable power"),
    ("Echo game",     "echo",        "Reflect-the-frame mini-game"),
    ("Quick draw",    "quickdraw",   "First to TX wins"),
    ("Beacon tag",    "beacon_tag",  "Broadcast + radar nearby badges"),
    ("Disco",         "disco",       "LED matrix pulses on RX"),
)


def _draw_menu(cursor):
    oled_clear()
    ui.chrome("IR Playground", str(cursor + 1) + "/" + str(len(SUB_APPS)),
              "X", "exit", "B", "open")
    visible = 5
    top = max(0, min(cursor - 2, len(SUB_APPS) - visible))
    for i in range(visible):
        idx = top + i
        if idx >= len(SUB_APPS):
            break
        title = SUB_APPS[idx][0]
        if idx == cursor:
            ui.selected_row(i, title)
        else:
            ui.line(i, title)
    oled_show()


def _stick_dir():
    """Returns -1 / 0 / +1 for joystick Y, debounced on edge."""
    y = joy_y()
    if y < 1100:
        return -1
    if y > 3000:
        return 1
    return 0


def _launch(slug):
    try:
        mod = __import__(slug)
    except Exception as exc:
        ui.chrome("IR Playground", slug, "X", "back", None, None)
        ui.center(20, "import failed")
        ui.center(32, str(exc)[:24])
        oled_show()
        time.sleep_ms(2500)
        return
    fn = getattr(mod, "run", None)
    if fn is None:
        ui.chrome("IR Playground", slug, "X", "back", None, None)
        ui.center(24, "no run() in " + slug)
        oled_show()
        time.sleep_ms(2000)
        return
    try:
        fn()
    except Exception as exc:
        ui.chrome("IR Playground", slug, "X", "back", None, None)
        ui.center(20, "crashed:")
        ui.center(32, str(exc)[:24])
        oled_show()
        time.sleep_ms(2500)


def main():
    cursor = 0
    last_dir = 0
    last_step = time.ticks_ms()
    _draw_menu(cursor)
    while True:
        if button_pressed(BTN_BACK):
            return
        if button_pressed(BTN_CONFIRM):
            _launch(SUB_APPS[cursor][1])
            _draw_menu(cursor)
            continue

        d = _stick_dir()
        now = time.ticks_ms()
        if d != 0 and (d != last_dir or time.ticks_diff(now, last_step) > 220):
            cursor = (cursor + d) % len(SUB_APPS)
            last_dir = d
            last_step = now
            _draw_menu(cursor)
        if d == 0:
            last_dir = 0

        time.sleep_ms(30)


run_app("IR Playground", main)
