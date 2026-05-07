"""View the last MicroPython app crash log."""

import time

from badge import *
from badge_app import LAST_ERROR_PATH, clamp, read_axis, run_app
import badge_ui as ui

NAV_MS = 150
WRAP_CHARS = 21
VISIBLE_LINES = 4


def wrap_line(line):
    if not line:
        return [" "]
    rows = []
    while line:
        rows.append(line[:WRAP_CHARS])
        line = line[WRAP_CHARS:]
    return rows


def load_lines():
    try:
        with open(LAST_ERROR_PATH, "r") as error_file:
            text = error_file.read()
    except Exception:
        return ["No crash log", LAST_ERROR_PATH]

    if not text.strip():
        return ["No crash log", LAST_ERROR_PATH]

    rows = []
    for line in text.split("\n"):
        rows.extend(wrap_line(line))
        if len(rows) >= 80:
            break
    return rows or ["No crash log", LAST_ERROR_PATH]


def clear_log():
    try:
        with open(LAST_ERROR_PATH, "w") as error_file:
            error_file.write("")
    except Exception:
        pass


def draw(lines, top):
    total = len(lines)
    right = str(top + 1) + "/" + str(total) if total else ""
    ui.chrome("Crash Log", right, "BACK", "quit", "OK", "clear")
    for row in range(VISIBLE_LINES):
        index = top + row
        if index < total:
            ui.text(3, 13 + row * 10, lines[index], 122)
    oled_show()


def main():
    lines = load_lines()
    top = 0
    last_nav = 0

    while True:
        draw(lines, top)

        if button_pressed(BTN_BACK):
            return

        if button_pressed(BTN_CONFIRM):
            clear_log()
            lines = load_lines()
            top = 0

        now = time.ticks_ms()
        y_dir = read_axis(joy_y())
        if y_dir and time.ticks_diff(now, last_nav) >= NAV_MS:
            top = clamp(top + y_dir, 0, max(0, len(lines) - VISIBLE_LINES))
            last_nav = now

        time.sleep_ms(40)


run_app("Crash Log", main)
