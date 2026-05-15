"""Scrollable on-badge README for the community-app submission flow.

Acts as both the "hello world" example and the in-pocket reference for
how to write + ship your own community app. The dunders below feed
the home-screen grid (AppRegistry scans this file as plain text — the
module is never imported during the scan).
"""

__title__ = "Submit App"
__description__ = "How to write and submit a community app"
__icon__ = "icon.py"
__order__ = 100

import badge
import time


# Each line is one rendered row on the 128x64 OLED.
# Headings are uppercased; blank strings render as gaps.
PAGES = [
    "COMMUNITY APP DEMO",
    "",
    "This tile is both the",
    "example and a guide.",
    "",
    "Use Up / Down to scroll.",
    "Confirm or Back exits.",
    "",
    "WHAT IS A COMMUNITY",
    "APP?",
    "",
    "A folder of MicroPython",
    "files vended through",
    "the Community Apps tile.",
    "Badges download it over",
    "WiFi - no flash needed.",
    "",
    "FOLDER LAYOUT",
    "",
    "community/your-id/",
    "  main.py    (entry)",
    "  icon.py    (12x12)",
    "  manifest.toml (opt)",
    "",
    "DUNDERS IN main.py",
    "",
    "__title__       short",
    "__description__ blurb",
    "__icon__ = 'icon.py'",
    "__order__ = 100  (opt)",
    "",
    "Set these so the home",
    "screen shows your app",
    "with a nice icon and",
    "label after install.",
    "",
    "ICON FORMAT",
    "",
    "icon.py exports DATA,",
    "a tuple of 24 bytes",
    "(2 bytes/row x 12 rows)",
    "in U8G2 XBM order. See",
    "icon.py in this folder",
    "for the canonical shape",
    "with a visual comment.",
    "",
    "SUBMITTING",
    "",
    "Path A: PR a folder.",
    "Open a PR adding",
    "community/your-id/",
    "with main.py + icon.py.",
    "CI validates schema,",
    "size budget (256KB),",
    "and Python AST parse.",
    "",
    "Path B: external host.",
    "File the community-app",
    "issue or PR an entry",
    "into community/",
    "external.json with",
    "url + sha256 fields.",
    "",
    "On merge to main, CI",
    "regenerates registry/",
    "community_apps.json",
    "and badges fetch it on",
    "the next WiFi connect.",
    "",
    "WHEN IT APPEARS",
    "",
    "Open Community Apps,",
    "pick your entry, hit",
    "Install. The home grid",
    "auto-rebuilds and your",
    "icon shows up next to",
    "the built-in apps.",
    "",
    "THE BADGE API",
    "",
    "badge.display.clear()",
    "badge.display.text(",
    "  s, x, y)",
    "badge.display.show()",
    "",
    "badge.button_pressed(",
    "  badge.BTN_UP)",
    "  ... DOWN/LEFT/RIGHT",
    "  ... CONFIRM/BACK",
    "",
    "badge.ir_send(addr,",
    "  cmd)",
    "badge.http_get(url)",
    "badge.boops()",
    "",
    "Full reference lives",
    "in /API_REFERENCE.md",
    "and on the docs site.",
    "",
    "HAPPY HACKING!",
    "",
    "github.com/Architeuthis-",
    "Flux/Temporal-Replay-",
    "26-Badge",
    "",
    "Confirm/Back to exit.",
]


LINE_H = 8
VISIBLE = 8  # 64 / 8


def render(top):
    badge.display.clear()
    end = min(top + VISIBLE, len(PAGES))
    y = 0
    for i in range(top, end):
        badge.display.text(PAGES[i], 0, y)
        y += LINE_H
    badge.display.show()


def main():
    top = 0
    max_top = max(0, len(PAGES) - VISIBLE)
    last_input_ms = 0
    render(top)

    while True:
        now = time.ticks_ms()
        # Throttle held-button repeat to ~6 lines/sec.
        if time.ticks_diff(now, last_input_ms) >= 160:
            moved = False
            if badge.button_pressed(badge.BTN_DOWN):
                if top < max_top:
                    top += 1
                    moved = True
            elif badge.button_pressed(badge.BTN_UP):
                if top > 0:
                    top -= 1
                    moved = True
            elif badge.button_pressed(badge.BTN_RIGHT):
                # Page-down jump for fast skim.
                top = min(max_top, top + VISIBLE)
                moved = True
            elif badge.button_pressed(badge.BTN_LEFT):
                top = max(0, top - VISIBLE)
                moved = True
            if moved:
                last_input_ms = now
                render(top)

        if (badge.button_pressed(badge.BTN_CONFIRM)
                or badge.button_pressed(badge.BTN_BACK)):
            return
        time.sleep_ms(20)


main()
