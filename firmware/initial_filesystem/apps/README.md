# Badge Apps

Python apps for the Temporal Badge. Files under `initial_filesystem/` are
embedded into the firmware image and provisioned onto the badge's FatFS
`ffat` partition at boot.

In dev-menu builds, each `.py` file in this directory appears in the Apps menu.
A folder with a `main.py` entry point is also treated as an app. Normal firmware
does not expose the generic Apps menu; production apps need an explicit launcher
in the native menu.

## Creating an App

Create a `.py` file in this directory, or create a folder with a `main.py`
entry point. It runs top-to-bottom when selected from the menu. Call `exit()`
when done to return cleanly to the menu.

Prefer the folder form once an app is more than a small demo:

```text
initial_filesystem/apps/my_app/
  main.py
  icon.py
  state.py
  screens.py
```

`main.py` should be tiny. Add the app folder to `sys.path`, then import and run
the real entry point:

```python
import sys

APP_DIR = "/apps/my_app"

if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

from engine import main

from badge_app import run_app

run_app("My App", main)
```

All badge API functions are auto-imported into the entry script's global scope,
so tiny one-file apps do not need `import badge` for hardware access. Modules
that are imported from a folder app should import what they use explicitly,
usually with `from badge import *`. See `../docs/API_REFERENCE.md` for the full
function list, and `../docs/MicroPythonDeveloperGuide.md` for the longer app
authoring guide.

Minimal example:

```python
import time

oled_clear()
oled_set_cursor(32, 28)
oled_print("Hello!")
oled_show()

time.sleep_ms(2000)
exit()
```

For app screens that should match the built-in firmware chrome, import
`badge_ui` and use its header/footer helpers. These call the native C++ UI
layout and button glyph code when the firmware provides it:

```python
import badge_ui as ui

ui.chrome("My App", "Score 0", "OK", "start", "BACK", "quit")
ui.line(0, "Menu item")
oled_show()
```

Use this helper before copying UI code into Python. It keeps MicroPython apps
aligned with the C++ menu chrome and avoids duplicating button glyph bitmaps.
The most common helpers are:

```python
ui.chrome("Title", "Right", "OK", "select", "BACK", "quit")
ui.chrome_tall("Title", "Right", (("X", "tool"), ("Y", "mode")), "BACK", "quit", "OK", "go")
ui.header("Title", "Score 120")
ui.action_bar("OK", "again", "BACK", "quit")
ui.center(28, "Centered text")
ui.line(0, "List row")
ui.selected_row(1, "Selected row")
ui.inline_hint(0, 53, "OK:start")
ui.hint_row((("X", "next"), ("Y", "mode"), ("BACK", "quit")), 43)
```

For apps that own both the OLED and LED matrix, import `badge_app` before
duplicating lifecycle and timing code. The shared helpers cover native-looking
crash screens, local high-score files, LED override cleanup, frame timers,
joystick thresholds, and button latches:

```python
from badge_app import ButtonLatch, DualScreenSession, GCTicker, read_stick_4way, with_led_override

def play_once():
    game = new_game()
    session = DualScreenSession(FRAME_MS)
    ok_latch = ButtonLatch(BTN_CONFIRM)

    def loop():
        while True:
            now = session.now()
            x_dir, y_dir = read_stick_4way()
            ok_edge = ok_latch.poll()
            if session.frame_due(game, now):
                tick_oled_game(game, ok_latch.consume(), now)
            tick_led_game(game, x_dir, y_dir, ok_edge, now)
            session.sleep()

    return with_led_override(loop)
```

Use `GCTicker()` directly for long-running apps that do not use
`DualScreenSession`. It sets the MicroPython GC allocation threshold when
available and runs a full collection every few seconds when `tick()` is called.

`run_app("My App", main, cleanup)` writes uncaught exceptions to
`/last_mpy_error.txt` and shows a badge-native crash screen instead of dropping
back to a blank or stale display. The optional cleanup callback runs on normal
return, crash, and `exit()`.

Dev firmware also includes a Crash Log app that reads `/last_mpy_error.txt` on
the badge. In normal firmware, use the Files screen or serial tooling to inspect
that file.

## App Manifest Dunders

Folder apps are auto-discovered by `AppRegistry` (see
`firmware/src/apps/AppRegistry.cpp`). Drop a `main.py` under
`/apps/<slug>/` and the firmware surfaces it on the main grid menu and in the
MATRIX APPS picker without any C++ changes — the registry text-scans the first
~2 KB of `main.py` for top-level dunder assignments and uses them to decorate
the menu tile:

```python
"""My Game — Tamagotchi-style desk pet."""

__title__       = "My Game"        # max 19 chars; falls back to slug-as-title
__description__ = "A tiny pet that lives in your pocket."  # max 63 chars
__icon__        = "icon.py"        # path to a 12×12 packed XBM tuple
__matrix_title__ = "Pet"           # only used when matrix.py is present
__order__       = 50               # signed int; lower = earlier on grid

# ... rest of main.py ...
```

The slug is the folder name (`/apps/my_game/` → `my_game`). It must be
alphanumeric plus `_` or `-`; anything else is skipped by the scanner. There
is a hard cap of 32 dynamic apps per badge — older slots win on ties.

### Home-screen Icon (`icon.py`)

`__icon__ = "icon.py"` (or any filename inside the app folder) points at a
12×12 monochrome icon for the main grid tile. The file just needs a top-level
`DATA = (...)` tuple of 24 bytes — packed XBM order, 2 bytes per row × 12
rows, bit 0 of each byte being the leftmost pixel (matches U8G2's
`drawXBM`). The upper 4 bits of every odd byte are unused.

```python
"""My Game app icon — bytes match firmware AppIcons style."""

WIDTH = 12
HEIGHT = 12
# Two bytes per row. Binary literals make the dot pattern visible in
# the source; XBM is LSB-first so the literal reads mirrored relative
# to the rendered icon — accepted, the bit values are unchanged.
DATA = (
    0b01110111, 0b00000111,
    0b01110111, 0b00000111,
    0b00000000, 0b00000000,
    0b01100000, 0b00000000,
    0b01100000, 0b00000000,
    0b11111100, 0b00000011,
    0b00000000, 0b00000000,
    0b00111110, 0b00000000,
    0b00100000, 0b00000000,
    0b11100000, 0b00000011,
    0b00000000, 0b00000010,
    0b11000000, 0b00000011,
)
```

If `__icon__` is omitted, the registry opportunistically reads
`/apps/<slug>/icon.py` and uses it if it parses; otherwise the tile falls
back to the generic apps icon. Inline tuples are also accepted
(`__icon__ = "(0xFF, 0x..., )"`) but reading from a separate file keeps
`main.py` readable.

### Tile Order (`__order__`)

The main grid is rendered in stable-sort order by a signed `int16` "order"
key. Three layers feed the key, each one overriding the layer above it:

1. **Defaults.** Curated tiles (the C++ array in
   `firmware/src/ui/GUI.cpp::kCuratedMenuItems`) get `10 × array_index`,
   leaving room (1, 2, …, 9) for inserts. Dynamic apps default to
   `10000 + AppRegistry_index`. `SETTINGS` is pinned to `30000` so it
   stays at the back of the grid by default.
2. **App manifest.** A folder app can declare `__order__ = 50` (or any
   signed integer) at the top of `main.py` to claim a specific slot.
3. **User override.** The Reorder Menu screen
   (Settings → Menu → Reorder) writes per-label overrides to NVS; those
   override both of the above for the labels they cover.

Ties resolve by insertion order (curated array index first, then
AppRegistry discovery order). That means duplicate `__order__` values are
fine — items with the same key just keep the order in which they were
placed.

A few common patterns:

```python
__order__ = -10   # before all curated tiles (BOOP, CONTACTS, …)
__order__ = 25    # between MAP (idx 30 → 30) and SCHEDULE (idx 40 → 40)
__order__ = 100   # right after the curated block, before other dyn apps
__order__ = 9999  # nearly last (just before SETTINGS)
```

Because the keys are signed, you can prepend an app with a negative
order without having to rewrite the curated table.

After editing icon bytes you can hot-refresh the menu without rebooting:

```python
import badge
badge.rescan_apps()
```

### Persistent Matrix Apps (`matrix.py`)

A sibling `matrix.py` next to `main.py` registers the app as a persistent
LED-matrix ambient. The MATRIX APPS picker (firmware menu → MATRIX APPS) lists
every app with a `matrix.py` past the built-in modes; selecting one calls
`commitMatrixApp(slug)`, which:

1. Persists the slug in `/led_state.json` so the choice survives reboots.
2. Tears down whatever Python matrix callback was running.
3. Sources `/apps/<slug>/matrix.py` once (via `mpy_gui_exec_file`) so it can
   register a callback with `badge.matrix_app_start(...)`.

The script's job is to install the callback and return — *do not* spin a main
loop. The callback then runs forever from the firmware service pump.

```python
"""Tardigotchi ambient matrix — slowly drifting ziggy."""

__matrix_title__ = "Ziggy"   # label shown in the MATRIX APPS picker

import badge

_phase = 0

def _tick(now_ms):
    global _phase
    _phase = (_phase + 1) & 7
    frame = [0] * 8
    frame[7] = 0x80 >> _phase
    badge.led_set_frame(frame)

# 250 ms cadence, 24/255 brightness; persists across reboots.
badge.matrix_app_start(_tick, 250, 24)
```

Constraints:

- The script runs once on commit and once at every boot when this slug is the
  active matrix app. Module-level state is reinitialised each time.
- `_tick` is invoked from the matrix service pump — keep it fast (no
  blocking I/O, no `time.sleep_ms`). Long work means dropped frames.
- Reading state from `/apps/<slug>/...` or other persisted JSON is fine; just
  don't write to flash on every tick. Throttle saves to once a minute or less.
- Selecting any non-Python mode in the MATRIX APPS picker (Sparkle, Off, etc.)
  clears the slug and stops the callback immediately.
- `__matrix_title__` is optional; falls back to the foreground app's
  `__title__`.

See `apps/tardigotchi/matrix.py` for a full Tamagotchi-style ambient
that reads the foreground game's save file.

## Manual Reorder Screen

Settings → **Menu → Reorder** opens an in-place reorder UI:

| Button | Action |
|--------|--------|
| Joystick Y | Move cursor up/down |
| `X` | Pick up the highlighted row (or drop it) |
| `A` (confirm) | Save and rebuild the menu |
| `B` (back) | Cancel without saving |

While picked up, joystick Y drags the row through the list — the swap is
performed at every cursor step so what you see is what you'll save. On
save, every row's index becomes its NVS override (spaced × 10 so future
inserts can land between user picks without bumping every override).
**Settings → Menu → Reset Order** wipes the NVS namespace and rebuilds,
returning every tile to its default order.

The screen takes a snapshot of labels at entry, so a concurrent
`badge.rescan_apps()` can't surprise it. Apps without a `main.py` (i.e.
removed since the snapshot) simply lose their override on the next
rebuild — the orphaned NVS keys stay around but are inert.

## Promoting an App to a Curated Native Tile

The auto-discovery path covers almost every case. The native curated tiles
in `firmware/src/ui/GUI.cpp` (`kCuratedMenuItems`) only exist for apps that
need a hardcoded display order, a C++ icon entry in `AppIcons.h`, or a
non-Python target screen. Most attendee-facing apps don't need this — the
dunder-based registration is enough.

Keep one-off diagnostics and API demos in the dev Apps menu instead of adding
them to the normal firmware menu.

## Building and Validating App Changes

Run these from `firmware/` before committing an app change:

```sh
black initial_filesystem/apps/<app> initial_filesystem/lib/badge_app.py initial_filesystem/lib/badge_ui.py
python3 -m py_compile initial_filesystem/apps/<app>/*.py initial_filesystem/lib/badge_app.py initial_filesystem/lib/badge_ui.py
python3 scripts/generate_startup_files.py
pio run -e echo
```

The PlatformIO build also runs `scripts/generate_startup_files.py`, but running
it manually makes the generated diff visible before the build. Commit both
`src/micropython/StartupFilesData.h` and
`scripts/startup_hash_history.json` when app files change. Do not edit
`StartupFilesData.h` by hand.

Use `echo-dev` when you need the generic Apps menu or internal diagnostics:

```sh
pio run -e echo-dev
pio run -e echo-dev -t upload --upload-port /dev/cu.usbmodemXXXX
```

Dev firmware also exposes `badge.dev("fb")` for framebuffer captures. To render
a MicroPython app screen from the badge into a PNG, use:

```sh
python3 scripts/capture_oled_fb.py --port /dev/cu.usbmodemXXXX --screen synth-live --out /tmp/synth-live.png
python3 scripts/capture_oled_fb.py --port /dev/cu.usbmodemXXXX --screen synth-sounds --out /tmp/synth-sounds.png
```

Use normal `echo` builds for attendee-facing smoke tests:

```sh
pio run -e echo
pio run -e echo -t upload --upload-port /dev/cu.usbmodemXXXX
```

`black` and `py_compile` are quick host-side checks. They do not replace an
on-device smoke test for input, timing, display ownership, LED override, or
MicroPython heap behavior.

## Deploying

```sh
# Flash firmware + embedded startup files together
pio run -e echo -t upload --upload-port /dev/cu.usbmodemXXXX

# Upload the raw FatFS image from firmware/data/ when you need data files
# such as doom1.wad. This does not replace generated startup files.
pio run -e echo -t uploadfs --upload-port /dev/cu.usbmodemXXXX
```

App source is compiled into `src/micropython/StartupFilesData.h` and written to
the FatFS partition by the firmware's startup provisioning pass. App changes are
not saved in GitHub until the source files and generated startup files are both
committed.

## API Quick Reference

### Display (128×64 SSD1306 OLED)

```python
oled_clear()                      # clear buffer (optional: oled_clear(True) to refresh)
oled_set_cursor(x, y)             # set text cursor position
oled_print(text)                  # print text at cursor (does not refresh)
oled_println(text)                # print text + newline + auto refresh
oled_show()                       # flush buffer to screen
oled_set_text_size(size)          # text size 1-4
oled_set_font(name)               # set font by name
oled_text_width(text)             # pixel width of string
oled_text_height()                # pixel height of current font
oled_set_pixel(x, y, color)       # set pixel (0=black, 1=white)
oled_draw_box(x, y, w, h)         # filled rectangle
oled_set_draw_color(color)        # 0=black, 1=white, 2=XOR
oled_invert(enable)               # invert display
oled_get_framebuffer()            # raw framebuffer as bytes
oled_set_framebuffer(data)        # replace framebuffer
oled_get_framebuffer_size()       # (width, height, bytes)
```

### Buttons & Joystick

```python
button(BTN_CONFIRM)               # True if held down
button_pressed(BTN_CONFIRM)       # True once per press (edge-triggered)
button_held_ms(BTN_CONFIRM)       # ms button has been held
joy_x()                           # joystick X (0-4095)
joy_y()                           # joystick Y (0-4095)
```

Constants: `BTN_RIGHT` (0), `BTN_DOWN` (1), `BTN_LEFT` (2), `BTN_UP` (3).
Aliases: `BTN_CIRCLE`, `BTN_CROSS`, `BTN_SQUARE`, `BTN_TRIANGLE`,
`BTN_CONFIRM`, `BTN_SAVE`, `BTN_BACK`, `BTN_PRESETS`.
Semantic defaults: `BTN_CONFIRM`/`BTN_SAVE` use B/Circle, `BTN_BACK` uses
A/Cross, and `BTN_PRESETS` uses Y/Triangle. Set `swap_ok = 0` for A/Cross
confirm and B/Circle back. The physical and shape constants always refer to
the actual hardware button.

### LED Matrix (8×8 IS31FL3731)

```python
led_brightness(value)             # global brightness 0-255
led_clear()                       # all LEDs off
led_fill()                        # all LEDs on
led_set_pixel(x, y, brightness)   # single LED (0-7, 0-255)
led_get_pixel(x, y)               # read LED brightness
led_show_image(IMG_HEART)         # builtin image
led_set_frame([row0..row7], brt)  # 8 bitmask rows
led_start_animation(ANIM_SPINNER) # builtin animation
led_stop_animation()              # stop animation
led_override_begin()              # pause ambient LED mode
led_override_end()                # restore ambient LED mode
```

Images: `IMG_SMILEY`, `IMG_HEART`, `IMG_ARROW_UP`, `IMG_ARROW_DOWN`, `IMG_X_MARK`, `IMG_DOT`
Animations: `ANIM_SPINNER`, `ANIM_BLINK_SMILEY`, `ANIM_PULSE_HEART`

Use `led_override_begin()` before foreground LED drawing and
`led_override_end()` in cleanup so the saved LED app mode resumes after your app.

### IMU (LIS2DH12 Accelerometer)

```python
imu_ready()                       # True if IMU initialized
imu_tilt_x()                      # X tilt in milli-g
imu_tilt_y()                      # Y tilt in milli-g
imu_accel_z()                     # Z acceleration in milli-g
imu_face_down()                   # True if face-down
imu_motion()                      # consume motion event
```

### Haptics & Tone

```python
haptic_pulse()                    # default vibration pulse
haptic_pulse(strength, ms, hz)    # custom pulse
haptic_strength([value])          # get/set default strength (0-255)
haptic_off()                      # stop motor
tone(freq_hz, [ms], [duty])       # audible coil tone
no_tone()                         # stop tone
tone_playing()                    # True if tone active
```

### IR (NEC Protocol)

```python
ir_send(addr, cmd)                # transmit NEC frame
ir_start()                        # enable IR receive
ir_stop()                         # disable receive, flush queue
ir_available()                    # True if frame waiting
ir_read()                         # (addr, cmd) tuple or None
```

### Mouse Overlay

```python
mouse_overlay(True)               # enable cursor overlay
mouse_x()                         # cursor X (0-127)
mouse_y()                         # cursor Y (0-63)
mouse_set_pos(x, y)               # warp cursor
mouse_clicked()                   # last click button ID (-1 if none)
mouse_set_speed(speed)            # cursor speed 1-20
mouse_set_mode(MOUSE_RELATIVE)    # MOUSE_ABSOLUTE or MOUSE_RELATIVE
mouse_set_bitmap(data, w, h)      # custom cursor sprite (XBM, max 16x16)
```

### Control

```python
exit()                            # clean exit back to menu
```

Use `import gc; gc.collect()` for manual garbage collection.

## Standard Library

Available MicroPython modules:

| Module | Key functions |
|--------|---------------|
| `time` | `sleep_ms()`, `ticks_ms()`, `ticks_diff()`, `ticks_add()` |
| `math` / `cmath` | Standard math (single-precision float) |
| `json` | `dumps()`, `loads()` |
| `gc` | `collect()`, `mem_free()`, `mem_alloc()` |
| `random` | `randint()`, `choice()`, `random()` |
| `struct` | `pack()`, `unpack()` |
| `binascii` | `hexlify()`, `unhexlify()` |
| `sys` | `path`, `version`, `implementation` |
| `os` | `listdir()`, `mkdir()`, `remove()`, `rename()`, `stat()`, `statvfs()`, `uname()` |

## Escape Chord

Users can force-exit a running app by holding all four face buttons for about
1 second.
This raises `SystemExit` and returns to the menu. You don't need to
handle this in your app.

## Limitations

- **2 MB Python heap** (PSRAM) — use `with_led_override`, `DualScreenSession`,
  or `GCTicker` to collect before tight loops and periodically during long
  polling loops.
- **Large files compile slowly** — there is no firmware-enforced source size
  limit, but folder apps with smaller modules are easier to edit and test.
- **Core 1 only** — all Python execution happens on Core 1.

## Tips

- Call `gc.collect()` before entering a polling loop, or use `GCTicker` to keep
  collection cadence predictable in long-running loops.
- Use `exit()` rather than letting your script fall off the end.
- Keep display update loops at ~30-80ms intervals to balance responsiveness
  with CPU usage.

## Example Apps

| File | Demonstrates |
|------|-------------|
| `hello.py` | Display, button polling, timed exit |
| `api_test.py` | Interactive test menu for all badge API functions |
| `input_test.py` | All inputs: buttons, joystick, IMU |
| `mouse_demo.py` | Mouse overlay cursor with absolute/relative modes |
| `synth/` | Joystick synthesizer with loop recorder and loadable sounds |
| `tilt_ball.py` | IMU tilt → LED matrix dot position |
| `font_demo.py` | Cycle through available OLED fonts |
| `ir_test.py` | IR receive — display incoming NEC frames |
| `ir_poll_test.py` | IR receive polling at 50ms for 2 minutes |
| `gc_bench.py` | GC pause measurement benchmark |
| `loop_test.py` | Infinite loop (test escape chord: all four face buttons) |
| `crash_test.py` | Unhandled exception (test error display) |
| `oom_test.py` | Out-of-memory behavior |
| `import_block_test.py` | Verify blocked module imports |
| `syntax_error_test.py` | Syntax error handling |
| `testImports.py` | Module import verification |
| `http_test.py` | Explicit MicroPython HTTP GET smoke test against a public API |
