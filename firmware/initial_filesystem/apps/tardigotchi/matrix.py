"""Tardigotchi ambient matrix app — Tamagotchi-style status panel.

Persistent 8x8 ambient on the LED matrix, kept alive by
badge.matrix_app_start so it runs even when the foreground tardigotchi
app is closed.

Layout (inspired by the Gen-1 Tamagotchi LCD: pet at bottom, status
icons cycling at top):

    rows 0..2  status icon, cycles each ~4 s
                  - heart   + happiness fill bar
                  - drumstick + hunger fill bar
                  - smile/frown face for mood
    row  3..4  empty (separation strip)
    rows 5..7  ground + walking pet glyph; the glyph grows with the
                pet's life stage (egg = 1 pixel, elder = full bug)

Game state:
- /tardigrade_save.json is loaded for hunger/happiness/xp.
- Passive tick runs at 1/10 the pace of the foreground engine: the
  foreground decays once per 60 s; here we decay once per 600 s.
- Updated state writes back to /tardigrade_save.json so the foreground
  app sees the ambient progress on its next launch.

Haptics: the badge clicks softly on every phase change to mimic the
Tamagotchi's beep, and pulses harder when a stat crosses below the
warning line (25) for the first time during this ambient session.
"""

__matrix_title__ = "Ziggy"

import json
import time

import badge


SAVE_PATH = "/tardigrade_save.json"

# Display cadence. 250 ms keeps the walking glyph smooth and the icon
# bars responsive without burning CPU.
TICK_MS = 250

# 16 ticks * 250 ms = 4 s per status panel.
PHASE_TICKS = 16

# Foreground passive tick fires every 60 s. 1/10 pace → every 600 s.
GAME_TICK_MS = 600 * 1000

# Pet level → drawable shape, anchored at row 7 col offset. Each tuple
# is a list of (dy, dx) cells where dy<=0 grows the body upward.
_PET_SHAPES = (
    # Level 0 — egg: single pixel
    ((0, 0),),
    # Level 1 — hatchling: 2 px stack
    ((0, 0), (-1, 0)),
    # Level 2 — baby: little 'T'
    ((0, 0), (-1, 0), (-1, 1)),
    # Level 3 — teen: 2x2 chunk
    ((0, 0), (0, 1), (-1, 0), (-1, 1)),
    # Level 4 — adult: small bug
    ((0, 0), (0, 2), (-1, 0), (-1, 1), (-1, 2)),
    # Level 5 — elder: full bug w/ leg
    ((0, 0), (0, 1), (0, 2), (0, 3), (-1, 1), (-1, 2), (-2, 2)),
)

# XP thresholds — must mirror icon.LEVEL_UNLOCKS.
_LEVEL_XP = (0, 25, 100, 250, 500, 1000)

# 3-row status icons (8 columns each, MSB = leftmost pixel). The dot
# pattern is visible directly in the binary literals; bottom-aligned in
# rows 0..2 so the right-edge fill bar lives in cols 6..7.
_HEART = (
    0b01010000,
    0b11111000,
    0b01110000,
)
_DRUMSTICK = (
    0b01110000,
    0b11110000,
    0b01101000,
)
_FACE_SMILE = (
    0b10101000,
    0b00000000,
    0b01110000,
)
_FACE_FROWN = (
    0b10101000,
    0b01110000,
    0b00000000,
)

# Hunger/happiness warning threshold (matches the foreground "okay/sad"
# moodscore breakpoints loosely). Crossing below this triggers a beep.
_WARN_THRESHOLD = 25


def _read_state():
    try:
        with open(SAVE_PATH, "r") as f:
            data = json.loads(f.read())
        if not isinstance(data, dict):
            return None
        return data
    except Exception:
        return None


def _write_state(state):
    try:
        keep = {k: v for k, v in state.items() if not str(k).startswith("_")}
        with open(SAVE_PATH, "w") as f:
            f.write(json.dumps(keep))
    except Exception:
        pass


def _level_for_xp(xp):
    lvl = 0
    for i, threshold in enumerate(_LEVEL_XP):
        if xp >= threshold:
            lvl = i
    return lvl


def _clamp(v, lo, hi):
    if v < lo:
        return lo
    if v > hi:
        return hi
    return v


def _bar_rows(value):
    """Return three row-bytes representing a 2-col vertical fill on the
    right edge (cols 6..7). 0..100 maps to 0..6 lit cells, filling
    bottom-up."""
    cells = (int(value) * 6) // 100
    if cells < 0:
        cells = 0
    if cells > 6:
        cells = 6
    rows = [0, 0, 0]
    # Order: row2 col7, row2 col6, row1 col7, row1 col6, row0 col7, row0 col6.
    # Masks are MSB-first 8-bit so col 7 = bit 0 = 0b00000001, col 6 = bit 1.
    pattern = (
        (2, 0b00000001),
        (2, 0b00000010),
        (1, 0b00000001),
        (1, 0b00000010),
        (0, 0b00000001),
        (0, 0b00000010),
    )
    for i in range(cells):
        r, m = pattern[i]
        rows[r] |= m
    return rows


def _draw_pet(frame, level, x, body_offset):
    shape = _PET_SHAPES[min(level, len(_PET_SHAPES) - 1)]
    base_y = 7 + body_offset  # body_offset is 0 or -1 for two-step gait
    for dy, dx in shape:
        y = base_y + dy
        col = x + dx
        if 0 <= y <= 7 and 0 <= col <= 7:
            frame[y] |= (0x80 >> col)


def _draw_icon(frame, icon_rows, value):
    bar = _bar_rows(value)
    for r in range(3):
        frame[r] = icon_rows[r] | bar[r]


def _passive_tick_if_due(state, now_ms):
    """Apply 1/10-paced decay if enough wall-time has elapsed since the
    last persisted tick. Mutates `state` in place; caller persists."""
    last = state.get("last_tick_ms", 0)
    elapsed = time.ticks_diff(now_ms, last)
    if elapsed < GAME_TICK_MS:
        return False
    ticks = elapsed // GAME_TICK_MS
    state["hunger"] = _clamp(state.get("hunger", 50) + 2 * ticks, 0, 100)
    state["happiness"] = _clamp(state.get("happiness", 50) - 1 * ticks, 0, 100)
    # Slow XP drift up over time so the pet ages even unattended (small).
    state["xp"] = state.get("xp", 0) + ticks
    state["age_secs"] = state.get("age_secs", 0) + ticks * (GAME_TICK_MS // 1000)
    state["last_tick_ms"] = now_ms
    return True


# Module-level runtime state. Survives across ticks; reset on reboot.
_run = {
    "phase": 0,        # 0=heart, 1=food, 2=face
    "phase_ticks": 0,  # ticks within current phase
    "step_ticks": 0,   # cadence for walking gait
    "x": 0,
    "dx": 1,
    "gait": 0,
    "last_warn": False,
    "save_dirty": False,
    "ticks_since_save": 0,
    "state": None,
}


def _ensure_state():
    if _run["state"] is None:
        s = _read_state() or {}
        # Coerce/default the fields we care about.
        s.setdefault("hunger", 50)
        s.setdefault("happiness", 50)
        s.setdefault("xp", 0)
        s.setdefault("age_secs", 0)
        s.setdefault("last_tick_ms", time.ticks_ms())
        _run["state"] = s
    return _run["state"]


def _tick(now_ms):
    state = _ensure_state()

    # Game decay (1/10 pace) — mutate state and mark dirty for save.
    if _passive_tick_if_due(state, now_ms):
        _run["save_dirty"] = True

    # Persist no more than once a minute, and only when something
    # changed (decay tick or phase warning).
    _run["ticks_since_save"] += 1
    if _run["save_dirty"] and _run["ticks_since_save"] > (60 * 1000 // TICK_MS):
        _write_state(state)
        _run["save_dirty"] = False
        _run["ticks_since_save"] = 0

    hunger = state.get("hunger", 50)
    happiness = state.get("happiness", 50)
    level = _level_for_xp(state.get("xp", 0))

    # Phase advance: tick counter per panel.
    _run["phase_ticks"] += 1
    if _run["phase_ticks"] >= PHASE_TICKS:
        _run["phase_ticks"] = 0
        _run["phase"] = (_run["phase"] + 1) % 3
        # Soft "click" on phase change — like a Tamagotchi UI beep.
        try:
            badge.haptic_pulse(60, 12)
        except Exception:
            pass

    # Walking cadence: step every ~500 ms. Bounce respects the pet's
    # current footprint so the body stays fully on the matrix.
    shape = _PET_SHAPES[min(level, len(_PET_SHAPES) - 1)]
    width = max(dx for _dy, dx in shape) + 1
    max_x = 8 - width
    _run["step_ticks"] += 1
    if _run["step_ticks"] >= 2:
        _run["step_ticks"] = 0
        _run["x"] += _run["dx"]
        if _run["x"] >= max_x:
            _run["x"] = max_x
            _run["dx"] = -1
        elif _run["x"] <= 0:
            _run["x"] = 0
            _run["dx"] = 1
        _run["gait"] ^= 1

    # Warning beep when a stat first dips below the threshold this run.
    warn = (hunger >= 75) or (happiness <= _WARN_THRESHOLD)
    if warn and not _run["last_warn"]:
        try:
            badge.haptic_pulse(140, 35)
        except Exception:
            pass
    _run["last_warn"] = warn

    # ── Compose the 8x8 frame ────────────────────────────────────────
    frame = [0] * 8

    if _run["phase"] == 0:
        _draw_icon(frame, _HEART, happiness)
    elif _run["phase"] == 1:
        _draw_icon(frame, _DRUMSTICK, 100 - hunger)
    else:
        face = _FACE_SMILE
        # Mood: roughly mirrors engine.mood() — sad when net score is low.
        if (happiness - hunger // 2) <= 30:
            face = _FACE_FROWN
        # No bar for the face — paint just the icon.
        for r in range(3):
            frame[r] = face[r]

    # Subtle ground line at row 6 only beneath the pet, so the bottom
    # half doesn't read as "empty" when the pet is at the edges.
    # (Disabled: the pet itself sits on rows 6..7; an extra dot would
    # blur with small-stage glyphs.)

    _draw_pet(frame, level, _run["x"], -_run["gait"])

    badge.led_set_frame(frame)


def _install():
    _ensure_state()
    badge.matrix_app_start(_tick, TICK_MS, 24)


_install()
