"""Beacon tag — broadcast a 4-byte tag derived from this badge's UID
and listen for nearby badges doing the same. Renders a live "radar"
of currently-visible badges with last-seen ages."""

import time

from badge import *
import badge_ui as ui

from ir_lib import IrSession


MAGIC = 0xBEAC0000
TX_PERIOD_MS = 600
DROP_AFTER_MS = 6000


def _self_tag():
    try:
        uid = my_uuid()
    except Exception:
        uid = "000000000000"
    val = 0
    for ch in uid[-6:]:
        val = (val << 4) | (int(ch, 16) if ch.isalnum() else 0)
    return val & 0xFFFF


def run():
    tag = _self_tag()
    last_tx = 0
    seen = {}  # peer_tag -> last_seen_ms
    with IrSession("badge", power=12):
        while True:
            now = time.ticks_ms()
            if time.ticks_diff(now, last_tx) > TX_PERIOD_MS:
                try:
                    ir_send_words([MAGIC | tag])
                except Exception:
                    pass
                last_tx = now

            try:
                got = ir_read_words()
            except Exception:
                got = None
            if got is not None and len(got) >= 1:
                w = int(got[0]) & 0xFFFFFFFF
                if (w & 0xFFFF0000) == MAGIC:
                    peer = w & 0xFFFF
                    if peer != tag:
                        seen[peer] = now

            for k in list(seen.keys()):
                if time.ticks_diff(now, seen[k]) > DROP_AFTER_MS:
                    del seen[k]

            oled_clear()
            ui.chrome("Beacon tag", "me %04X" % tag,
                      "X", "back", None, None)
            ui.line(0, "%d nearby" % len(seen))
            sorted_peers = sorted(seen.items(), key=lambda kv: -kv[1])
            for i, (peer, t) in enumerate(sorted_peers[:3]):
                age = time.ticks_diff(now, t)
                ui.line(1 + i, "%04X  %dms" % (peer, age))
            oled_show()

            if button_pressed(BTN_BACK):
                return
            time.sleep_ms(50)
