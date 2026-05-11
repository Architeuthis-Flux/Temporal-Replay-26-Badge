"""Inspector — capture one raw IR burst and show its timing histogram.

Useful for identifying unknown protocols. NEC has 2 distinct space widths
(560 us / 1690 us); Sony 12-bit has long marks; RC5 is bi-phase.
"""

import time

from badge import *
import badge_ui as ui

from ir_lib import IrSession, raw_capture, raw_pairs


def _bucket(values, bucket_us=200, max_us=4000):
    n = (max_us // bucket_us) + 1
    hist = [0] * n
    for v in values:
        idx = min(int(v) // bucket_us, n - 1)
        hist[idx] += 1
    return hist


def _histogram(buf):
    marks = []
    spaces = []
    leader_mark = 0
    leader_space = 0
    for i, (m, s) in enumerate(raw_pairs(buf)):
        if i == 0:
            leader_mark = m
            leader_space = s
        else:
            if m:
                marks.append(m)
            if s:
                spaces.append(s)
    return leader_mark, leader_space, marks, spaces


def _draw_hist(label, y, values, max_us=4000):
    if not values:
        ui.line(y // 9, label + " —")
        return
    bucket_us = 200
    hist = _bucket(values, bucket_us=bucket_us, max_us=max_us)
    peak = max(hist) or 1
    width = 80
    n = len(hist)
    for i in range(min(n, width)):
        bar_h = (hist[i] * 7) // peak
        if bar_h:
            for k in range(bar_h):
                oled_set_pixel(40 + i, y + 7 - k, 1)
    ui.text(0, y, label)
    ui.text(width + 42, y, str(len(values)))


def _guess_protocol(leader_mark, leader_space, n_pairs):
    if leader_mark > 8000 and 4000 <= leader_space <= 5000 and n_pairs >= 33:
        return "NEC?"
    if leader_mark > 8000 and 2000 <= leader_space <= 2500:
        return "NEC repeat"
    if 2000 <= leader_mark <= 2700 and 400 <= leader_space <= 700:
        return "Sony?"
    if leader_mark < 1000 and n_pairs > 10:
        return "RC5/RC6?"
    return "?"


def run():
    last_buf = None
    with IrSession("raw") as sess:
        while True:
            oled_clear()
            ui.chrome("Inspector", "raw", "X", "back", "B", "scan")
            if last_buf is None:
                ui.line(0, "Press B to capture")
                ui.line(2, "the next IR burst.")
            else:
                lm, ls, marks, spaces = _histogram(last_buf)
                guess = _guess_protocol(lm, ls, len(marks) + 1)
                ui.line(0, "leader %dus / %dus" % (lm, ls))
                ui.line(1, "%d pairs · %s" % (len(marks) + 1, guess))
                _draw_hist("M", 28, marks)
                _draw_hist("S", 38, spaces)
            oled_show()

            if button_pressed(BTN_BACK):
                return
            if button_pressed(BTN_CONFIRM):
                ir_flush()
                ui.center(28, "Listening...")
                oled_show()
                buf = raw_capture(timeout_ms=4000)
                last_buf = buf if buf else None
                if last_buf is None:
                    ui.center(28, "no burst seen")
                    oled_show()
                    time.sleep_ms(800)
            time.sleep_ms(40)
