"""Bouncer — peer ping-pong over the badge multi-word transport.

Each badge TXes a magic-stamped probe word every ~250 ms and reflects
incoming probes once. Reads round-trip time off the reflection so two
badges show "RTT 35 ms" instead of just "alive". Uses badge mode so
both sides can negotiate without reconfiguring."""

import os
import time

from badge import *
import badge_ui as ui

from ir_lib import IrSession


MAGIC_PING = 0xBA00B100
MAGIC_PONG = 0xBA00B200


def _seq16():
    b = os.urandom(2)
    return (b[0] << 8) | b[1]


def run():
    sent = 0
    seen = 0
    last_rtt = 0
    rtt_avg = 0
    pending = {}
    last_tx_t = 0

    with IrSession("badge", power=10):
        while True:
            now = time.ticks_ms()

            if time.ticks_diff(now, last_tx_t) > 250:
                seq = _seq16()
                pending[seq] = now
                # prune old slots
                for k in list(pending.keys()):
                    if time.ticks_diff(now, pending[k]) > 2000:
                        del pending[k]
                try:
                    ir_send_words([MAGIC_PING | seq])
                    sent += 1
                except Exception:
                    pass
                last_tx_t = now

            try:
                got = ir_read_words()
            except Exception:
                got = None
            if got is not None and len(got) >= 1:
                w = int(got[0]) & 0xFFFFFFFF
                tag = w & 0xFFFF0000
                seq = w & 0xFFFF
                if tag == MAGIC_PING:
                    try:
                        ir_send_words([MAGIC_PONG | seq])
                    except Exception:
                        pass
                elif tag == MAGIC_PONG and seq in pending:
                    sent_t = pending.pop(seq)
                    last_rtt = time.ticks_diff(now, sent_t)
                    rtt_avg = (rtt_avg * 7 + last_rtt) // 8
                    seen += 1

            oled_clear()
            ui.chrome("Bouncer", "rtt %dms" % last_rtt,
                      "X", "back", None, None)
            ui.line(0, "tx %d  rx %d" % (sent, seen))
            loss = 0
            if sent:
                loss = 100 - min(100, (seen * 100) // sent)
            ui.line(1, "loss %d%%  avg %dms" % (loss, rtt_avg))
            ui.line(3, "(needs another badge)")
            oled_show()

            if button_pressed(BTN_BACK):
                return
            time.sleep_ms(20)
