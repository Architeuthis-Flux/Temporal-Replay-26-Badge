# example-app/main.py
#
# Badge app starter template.
#
# How to run on your badge:
#   ./scripts/compile-app.sh apps/example-app/ --upload
#
# Then open the launcher from the main badge screen (hold joystick down)
# and select "Example App".
#
# Available modules:
#   import badge      — display, buttons, joystick, tilt, device info
#   import badge_api  — boops, pings, badge info
#   import badge_ir   — IR pairing
#
# Press BTN_LEFT to exit back to the launcher at any time.

import badge

# ── Display ───────────────────────────────────────────────────────────────────
# badge.display works like a framebuf — draw calls buffer until .show()
badge.display.fill(0)
badge.display.text("Example App", 0, 0, 1)
badge.display.text("BTN_LEFT to exit", 0, 54, 1)
badge.display.show()

# ── Main loop ─────────────────────────────────────────────────────────────────
running = True
frame = 0

while running:
    # Read inputs
    buttons = badge.input.buttons()   # dict: {up, down, left, right} → bool (True = pressed)
    joy     = badge.input.joystick()  # dict: {x, y} → float -1.0..1.0
    tilt    = badge.input.tilt()      # bool: True = tilted

    # BTN_LEFT exits back to launcher
    if buttons["left"]:
        running = False
        break

    # Draw frame
    badge.display.fill(0)
    badge.display.text("Example App", 0, 0, 1)

    # Show joystick position
    joy_x = int(64 + joy["x"] * 20)
    joy_y = int(32 + joy["y"] * 10)
    badge.display.text("joy: ({},{})".format(joy_x, joy_y), 0, 12, 1)

    # Show tilt state
    badge.display.text("tilt: {}".format("yes" if tilt else "no"), 0, 24, 1)

    # Show which buttons are pressed
    held = [k for k, v in buttons.items() if v]
    badge.display.text("btn: {}".format(",".join(held) or "none"), 0, 36, 1)

    badge.display.text("BTN_LEFT to exit", 0, 54, 1)
    badge.display.show()

    frame += 1
    badge.sleep_ms(50)

# Clean up — clear display before returning to launcher
badge.display.fill(0)
badge.display.show()
