#!/usr/bin/env python3
"""
Convert assets/credits/* headshots into 64×64 dithered XBM bitmaps and
emit a C++ header + MicroPython companion app for the AboutCredits
screen. Hand-authored .bin files in assets/credits/prebinned/ override
the dither path; same-stemmed .bin alongside `kev.jpeg` is auto-picked
without a JSON edit.

Each image is EXIF-rotated, center-cropped to a square, resized with
Lanczos to 64×64, then converted to 1-bit with the credit's selected
dither algorithm (Floyd-Steinberg / Atkinson / Bayer / threshold).

Usage:
  python3 scripts/gen_credit_xbms.py
  python3 scripts/gen_credit_xbms.py --preview

Inputs:
  assets/credits/*.{jpg,jpeg,png}
  assets/credits/credits.json          (optional — explicit roster)
  assets/credits/prebinned/*.bin       (optional — hand-tuned bitmaps)
  assets/credits/prebinned/<name>/     (optional — animation frames)

Outputs:
  firmware/src/screens/AboutCredits.h  — native AboutCreditsScreen data
  firmware/data/apps/credits.py        — Python companion app

Both outputs draw from the same source images / bins so the native
screen and the Python app show identical headshots.

The C++ header exposes:
  AboutCredits::kCount                 number of credits
  AboutCredits::kIconSize              constexpr (= 64)
  AboutCredits::kCreditBits            PROGMEM XBM blob, 512 B/frame
  AboutCredits::kFrameDurationsMs      per-frame durations for animations
  AboutCredits::kAnimLoopPauseMs       hold time at end of each loop
  AboutCredits::kCredits[]             metadata + frame info per credit
  AboutCredits::kImageInfos[]          ImageInfo wrappers for ImageScaler
"""

from __future__ import annotations

import argparse
import json
import sys
import textwrap
from pathlib import Path

try:
    from PIL import Image, ImageOps, ImageDraw, ImageFont
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

REPO = Path(__file__).resolve().parent.parent
CREDITS_DIR = REPO / "assets" / "credits"
ROSTER_FILE = CREDITS_DIR / "credits.json"
PREBINNED_DIR = CREDITS_DIR / "prebinned"
PREVIEW_DIR = CREDITS_DIR / "preview"
OUT_HEADER = REPO / "firmware" / "src" / "screens" / "AboutCredits.h"
OUT_APP = REPO / "firmware" / "data" / "apps" / "credits.py"

ICON_SIZE = 64
SUPPORTED_EXTS = {".jpg", ".jpeg", ".png"}

# .bin file header layout (matches the format the user is hand-authoring
# in assets/credits/prebinned/): two LE-uint16s for width/height, then
# packed XBM bytes (LSB-first). For 64×64 we expect 4 + 512 = 516 bytes.
BIN_HEADER_BYTES = 4

# Default per-frame display time when an animation directory uses
# filenames the parser can't extract a `_delay-Xs` token from.
DEFAULT_FRAME_MS = 100

# Trailing pause after the last frame of an animation before the cycle
# restarts. The screen renders the final frame for this whole window so
# the user gets a beat to read whatever the last frame says before the
# loop begins again.
ANIM_LOOP_PAUSE_MS = 1000

# ── Animation flag bits ──────────────────────────────────────────────────────
# Packed into Credit::flags so future per-loop behaviors can stack without
# growing the struct. Keep these in sync with the constants of the same
# name in firmware/src/screens/AboutCreditsScreen.cpp.
ANIM_FLAG_LOOP_BLANK       = 1 << 0  # blank cell during loop-end pause
ANIM_FLAG_RANDOM_TRANSFORM = 1 << 1  # rotate/flip every loop iteration

# ── Dithering catalog ────────────────────────────────────────────────────────
# Per-credit dither treatment is chosen in credits.json. The default is
# Floyd-Steinberg (best for continuous-tone faces) but ordered/error-
# diffusion variants are useful when FS produces noisy "snow" on
# high-contrast portraits or when a posterised look reads better at the
# OLED's pixel pitch.
#
# Each preset is a (label, callable) pair. The callable consumes a PIL
# "L" (8-bit grayscale) image of the target size and returns a PIL "1"
# (1-bit) image. Contrast / brightness / threshold knobs are applied
# upstream in `image_to_xbm` before the chosen dither runs.

DITHER_MODES = (
    "floyd",      # Floyd-Steinberg error diffusion (PIL built-in)
    "atkinson",   # Atkinson error diffusion (Mac-classic look, ~7/8 error spread)
    "bayer4",     # Ordered dither, 4×4 Bayer matrix
    "bayer8",     # Ordered dither, 8×8 Bayer matrix
    "threshold",  # Hard threshold (no dither) — gives a posterised mask
    "none",       # alias for threshold at 128
)
DEFAULT_DITHER = "floyd"


# Pre-computed Bayer threshold matrices (values 0..(n*n-1)).
BAYER_4 = (
    ( 0,  8,  2, 10),
    (12,  4, 14,  6),
    ( 3, 11,  1,  9),
    (15,  7, 13,  5),
)
BAYER_8 = (
    ( 0, 32,  8, 40,  2, 34, 10, 42),
    (48, 16, 56, 24, 50, 18, 58, 26),
    (12, 44,  4, 36, 14, 46,  6, 38),
    (60, 28, 52, 20, 62, 30, 54, 22),
    ( 3, 35, 11, 43,  1, 33,  9, 41),
    (51, 19, 59, 27, 49, 17, 57, 25),
    (15, 47,  7, 39, 13, 45,  5, 37),
    (63, 31, 55, 23, 61, 29, 53, 21),
)


def _dither_floyd(img: Image.Image, _threshold: int) -> Image.Image:
    return img.convert("1", dither=Image.FLOYDSTEINBERG)


def _dither_threshold(img: Image.Image, threshold: int) -> Image.Image:
    # `convert("1", dither=NONE)` snaps at 128; we need a configurable
    # cut so callers can shift the posterisation point.
    px = img.load()
    w, h = img.size
    out = Image.new("1", (w, h))
    op = out.load()
    for y in range(h):
        for x in range(w):
            op[x, y] = 1 if px[x, y] >= threshold else 0
    return out


def _dither_bayer(img: Image.Image, matrix, divisor: int) -> Image.Image:
    px = img.load()
    w, h = img.size
    out = Image.new("1", (w, h))
    op = out.load()
    n = len(matrix)
    for y in range(h):
        row = matrix[y % n]
        for x in range(w):
            # Scale the matrix entry into 0..255 so it directly compares
            # with the source pixel's 0..255 value.
            t = (row[x % n] * 256) // divisor
            op[x, y] = 1 if px[x, y] > t else 0
    return out


def _dither_atkinson(img: Image.Image, _threshold: int) -> Image.Image:
    # Atkinson distributes 6 of every 8 units of error to neighbours
    # ((x+1,y), (x+2,y), (x-1,y+1), (x,y+1), (x+1,y+1), (x,y+2)). The
    # missing 2/8 leaks brightness which is part of the Mac aesthetic
    # — softer midtones, fewer heavy black blobs than Floyd.
    px = list(img.getdata())
    w, h = img.size

    def get(x, y):
        return px[y * w + x]

    def put(x, y, v):
        px[y * w + x] = max(0, min(255, v))

    for y in range(h):
        for x in range(w):
            old = get(x, y)
            new = 255 if old >= 128 else 0
            put(x, y, new)
            err = (old - new) // 8
            if err == 0:
                continue
            if x + 1 < w: put(x + 1, y, get(x + 1, y) + err)
            if x + 2 < w: put(x + 2, y, get(x + 2, y) + err)
            if y + 1 < h:
                if x - 1 >= 0: put(x - 1, y + 1, get(x - 1, y + 1) + err)
                put(x, y + 1, get(x, y + 1) + err)
                if x + 1 < w: put(x + 1, y + 1, get(x + 1, y + 1) + err)
            if y + 2 < h:
                put(x, y + 2, get(x, y + 2) + err)

    out = Image.new("1", (w, h))
    out.putdata([1 if v >= 128 else 0 for v in px])
    return out


def apply_dither(img_l: Image.Image, mode: str, threshold: int) -> Image.Image:
    """Dispatch table for the per-entry dither selector."""
    if mode == "floyd":
        return _dither_floyd(img_l, threshold)
    if mode == "atkinson":
        return _dither_atkinson(img_l, threshold)
    if mode == "bayer4":
        return _dither_bayer(img_l, BAYER_4, 16)
    if mode == "bayer8":
        return _dither_bayer(img_l, BAYER_8, 64)
    if mode == "threshold":
        return _dither_threshold(img_l, threshold)
    if mode == "none":
        return _dither_threshold(img_l, 128)
    raise ValueError(
        f"Unknown dither mode '{mode}'. Choose from: {', '.join(DITHER_MODES)}"
    )


class DitherSettings:
    """Per-credit knobs read from credits.json."""

    __slots__ = ("mode", "contrast", "brightness", "threshold")

    def __init__(self, mode: str = DEFAULT_DITHER, contrast: int = 2,
                 brightness: float = 1.0, threshold: int = 128) -> None:
        self.mode = mode
        # PIL autocontrast `cutoff` is a 0..49 percentage clamp.
        self.contrast = max(0, min(49, int(contrast)))
        # Multiplied into pixel values before dither; >1 brightens.
        self.brightness = max(0.1, float(brightness))
        # Used by `threshold` mode (and ignored by FS/Atkinson/Bayer).
        self.threshold = max(0, min(255, int(threshold)))

    def label(self) -> str:
        bits = [self.mode]
        if self.contrast != 2:
            bits.append(f"c{self.contrast}")
        if abs(self.brightness - 1.0) > 0.001:
            bits.append(f"b{self.brightness:.2f}")
        if self.mode == "threshold" and self.threshold != 128:
            bits.append(f"t{self.threshold}")
        return ":".join(bits)


class RosterEntry:
    """One line of the explicit credits roster.

    Each entry is one of three flavors, distinguished by the `prebinned`
    field:

      * Source image, no prebin → `image_path` set, `prebin_paths` empty.
        The generator dithers `image_path` into 64×64 + 48×48 XBMs using
        `dither` settings.

      * Single prebinned bin → `prebin_paths == [path]`, length-1
        animation. The 64×64 XBM is read straight from that .bin file,
        the 48×48 is downscaled from it.

      * Animated prebin → `prebin_paths` is the (sorted) list of frame
        files inside a prebinned directory; `frame_durations_ms` carries
        the per-frame display time parsed from each filename.

    `image_path` may still be set on prebinned entries — it's used as a
    fallback for the 48×48 view (resized + redithered from the source)
    so the Python companion app keeps a sane downscale instead of
    inheriting a coarse pixel-decimation of the user's hand-tuned bin.
    """

    __slots__ = (
        "name", "bio", "position", "company", "website",
        "image_path", "prebin_paths", "frame_durations_ms",
        "dither", "speed", "loop_blank", "random_transform",
        "group", "weight",
    )

    def __init__(self, name: str, bio: str,
                 image_path: Path | None,
                 prebin_paths: list[Path],
                 frame_durations_ms: list[int],
                 dither: DitherSettings,
                 speed: float = 1.0,
                 loop_blank: bool = False,
                 random_transform: bool = False,
                 group: str | None = None,
                 position: str = "",
                 company: str = "",
                 website: str = "",
                 weight: int | None = None) -> None:
        self.name = name
        self.bio = bio
        self.position = position
        self.company = company
        self.website = website
        self.image_path = image_path
        self.prebin_paths = prebin_paths
        self.frame_durations_ms = frame_durations_ms
        self.dither = dither
        # Group key — entries that share a key are pooled into one card
        # slot on the screen, and a fresh variant is randomly picked
        # every time that slot scrolls into view. Default = display name,
        # so credits with identical `name` strings auto-pool. Set
        # explicitly when you want to pool entries whose display names
        # differ (e.g. WIFOS uses "Kevin\nSanto\nCappuccio" but should
        # share a pool with the plain "Kevin" headshots).
        self.group = group if group else name
        # Weighted-random pool selection. The weighted variant pick uses
        # `weight` as the relative likelihood of being chosen on each
        # off→on transition. Default for static credits is 1; animated
        # credits default to 3 because the flipbook is the more
        # interesting variant to land on, but any per-entry override in
        # credits.json wins.
        if weight is None:
            weight = 3 if (prebin_paths and len(prebin_paths) > 1) else 1
        self.weight = max(1, min(255, int(weight)))
        # Playback speed multiplier; 1.0 = use the parsed delays as-is,
        # 0.5 = play at half speed (each frame held twice as long), 2.0
        # = double speed.
        self.speed = max(0.05, float(speed))
        # When True the loop-end pause renders an empty cell instead of
        # holding the last frame. Only meaningful for animated credits.
        self.loop_blank = bool(loop_blank)
        # When True the screen picks a random rotation+flip combo every
        # time the animation cycle restarts, so each loop appears in a
        # different orientation. Only meaningful for animated credits.
        self.random_transform = bool(random_transform)

    def flags_byte(self) -> int:
        """Pack the per-credit animation flags into a single byte."""
        f = 0
        if self.loop_blank:       f |= ANIM_FLAG_LOOP_BLANK
        if self.random_transform: f |= ANIM_FLAG_RANDOM_TRANSFORM
        return f

    @property
    def is_animated(self) -> bool:
        return len(self.prebin_paths) > 1

    @property
    def is_prebinned(self) -> bool:
        return bool(self.prebin_paths)

    @property
    def display_source(self) -> str:
        """Human-readable label for the source(s) used by this entry."""
        if self.prebin_paths:
            if self.is_animated:
                return f"prebin/{self.prebin_paths[0].parent.name}/ ({len(self.prebin_paths)}f)"
            return f"prebin/{self.prebin_paths[0].name}"
        if self.image_path:
            return self.image_path.name
        return "(no source)"


def parse_frame_delay_ms(path: Path) -> int:
    """Extract the per-frame delay encoded in a kevinWIFOS-style filename.

    Filenames look like `frame_07_delay-0.1s.bin` — the `delay-Xs`
    fragment carries the per-frame display time in seconds (allowing
    fractional values). Anything we can't parse falls back to
    DEFAULT_FRAME_MS so the generator doesn't choke on a re-named frame.
    """
    import re
    match = re.search(r"delay-(\d+(?:\.\d+)?)s", path.stem)
    if not match:
        return DEFAULT_FRAME_MS
    seconds = float(match.group(1))
    ms = int(round(seconds * 1000.0))
    # Clamp into a sane range — at 0ms we'd burn CPU; at >65s a single
    # frame would tie up the screen forever.
    return max(20, min(60000, ms))


# Per-byte bit reversal lookup. The .bin tool packs each row's bytes
# MSB-first (bit 7 = leftmost pixel) but oled::drawXBM expects LSB-first
# XBM (bit 0 = leftmost), so we flip each byte at read time and the rest
# of the pipeline stays in standard XBM coordinates.
_BIT_REVERSE = bytes(
    int(f"{i:08b}"[::-1], 2) for i in range(256)
)


def read_bin_xbm(path: Path) -> bytes:
    """Read a hand-authored .bin file and return a row-major XBM body.

    The .bin format the user is producing in assets/credits/prebinned/ is
    `width(LE16) height(LE16) | row_major_msb_first_packed_bits`. That's
    standard PC/AT-style monochrome packing (bit 7 = leftmost pixel of
    each 8-px run) — *not* the LSB-first ordering that `oled::drawXBM`
    expects. Symptom of skipping the conversion: every 8-pixel horizontal
    span in the rendered image is mirrored, which reads as "reversed
    8-pixel vertical strips" once the eye averages adjacent rows.

    We bit-reverse each byte here so every downstream consumer (drawXBM,
    the preview tooling, the Python companion) sees the standard XBM
    layout.

    Validates that width/height are 64×64 (the only thing the screen
    knows how to render) and that the body length matches.
    """
    raw = path.read_bytes()
    if len(raw) < BIN_HEADER_BYTES + 1:
        sys.exit(f"{path}: too small to be a valid .bin (got {len(raw)}B)")
    width  = int.from_bytes(raw[0:2], "little")
    height = int.from_bytes(raw[2:4], "little")
    if (width, height) != (ICON_SIZE, ICON_SIZE):
        sys.exit(
            f"{path}: header says {width}×{height}, expected "
            f"{ICON_SIZE}×{ICON_SIZE}"
        )
    expected = ((width + 7) // 8) * height
    body = raw[BIN_HEADER_BYTES:]
    if len(body) != expected:
        sys.exit(
            f"{path}: body is {len(body)}B, expected {expected}B "
            f"for a {width}×{height} packed bitmap"
        )
    return bytes(_BIT_REVERSE[b] for b in body)


def resolve_prebin(spec, ctx: str) -> tuple[list[Path], list[int]]:
    """Turn a `prebinned` value from credits.json into frame paths + delays.

    Accepts a filename ("kev.bin"), a directory name ("kevinWIFOS"), or
    a path-prefixed string. Filenames give a single static frame;
    directories are sorted alphabetically and treated as animation
    sequences with per-frame delays parsed from the filenames. The
    user-friendly auto-detection path (no `prebinned` field at all) is
    handled in the caller.
    """
    candidate = (PREBINNED_DIR / spec).resolve()
    if candidate.is_file():
        return [candidate], [DEFAULT_FRAME_MS]
    if candidate.is_dir():
        frames = sorted(
            p for p in candidate.iterdir()
            if p.is_file() and p.suffix.lower() == ".bin"
        )
        if not frames:
            sys.exit(f"{ctx}: prebinned dir '{spec}' has no .bin frames")
        durations = [parse_frame_delay_ms(p) for p in frames]
        return frames, durations
    sys.exit(
        f"{ctx}: prebinned source '{spec}' not found under "
        f"{PREBINNED_DIR.relative_to(REPO)}/"
    )


def autodetect_prebin(stem: str) -> list[Path]:
    """Auto-pick a prebinned .bin for an entry whose `file` matches a stem.

    Skips animated directory matches — those need an explicit roster
    entry so we don't silently turn a static credit into a 17-frame loop
    just because somebody dropped a same-stemmed directory next to the
    image. Returns [] when nothing matches.
    """
    candidate = PREBINNED_DIR / f"{stem}.bin"
    if candidate.is_file():
        return [candidate]
    return []


def _dither_from_dict(item: dict, ctx: str) -> DitherSettings:
    """Pull dither/contrast/brightness/threshold from a roster entry.

    `item` is the raw JSON dict; `ctx` is a human-readable location
    used in error messages so a typo in credits.json points at the
    right line. Both inline (`"dither": "atkinson"`) and nested
    (`"dither": {"mode": "atkinson", "contrast": 8}`) shapes work.
    """
    raw = item.get("dither", DEFAULT_DITHER)
    contrast = item.get("contrast", 2)
    brightness = item.get("brightness", 1.0)
    threshold = item.get("threshold", 128)
    if isinstance(raw, dict):
        mode = raw.get("mode", DEFAULT_DITHER)
        contrast = raw.get("contrast", contrast)
        brightness = raw.get("brightness", brightness)
        threshold = raw.get("threshold", threshold)
    else:
        mode = raw
    if mode not in DITHER_MODES:
        sys.exit(
            f"{ctx}: dither '{mode}' is not one of "
            f"{', '.join(DITHER_MODES)}"
        )
    return DitherSettings(mode=mode, contrast=contrast,
                          brightness=brightness, threshold=threshold)


def load_roster(image_files: list[Path]) -> list[RosterEntry]:
    """Read credits.json and produce a flat list of RosterEntry variants.

    Schema is **person-centric**: the file lists `people` and each person
    declares the metadata that's shared by all of their headshots
    (`name`, `position`, `company`, `website`, `bio`) plus an `images`
    array of one or more variants. Per-image fields override only the
    image-side knobs (`speed`, `loop_blank`, `random_transform`,
    `dither`, `contrast`, `brightness`, `threshold`, `weight`).

    Person-level metadata is duplicated onto every variant's RosterEntry
    so the runtime Credit struct stays flat — but because every variant
    of one person comes from the same source, the displayed name /
    company / etc. never change as the random pool rotates picks.

    No orphan auto-append, no implicit autodetect by stem — the JSON is
    the only source of truth, so dropping a new image into the directory
    without listing it is a no-op (logged at end of bake). This is the
    fix for the previous "every photo became its own card with the
    auto-derived stem name" surprise.
    """
    file_map    = {p.name: p for p in image_files}
    used_images: set[str] = set()
    used_bins:   set[str] = set()
    entries:     list[RosterEntry] = []

    if not ROSTER_FILE.exists():
        sys.exit(f"{ROSTER_FILE.relative_to(REPO)} not found — required")

    try:
        doc = json.loads(ROSTER_FILE.read_text())
    except json.JSONDecodeError as exc:
        sys.exit(f"Failed to parse {ROSTER_FILE}: {exc}")

    people = doc.get("people")
    if people is None:
        sys.exit(
            f"{ROSTER_FILE}: top-level 'people' list is required "
            "(see the file's _comment block for schema details)"
        )
    if not isinstance(people, list):
        sys.exit(f"{ROSTER_FILE}: 'people' must be a list")

    # Field names that belong to a *person* (and propagate to every one
    # of that person's variants). Anything else nested in `images` is
    # an image-only override, so we sanity-check the keys for typos.
    PERSON_FIELDS = {
        "name", "position", "company", "website", "bio",
        "group", "images",
    }
    IMAGE_FIELDS = {
        "file", "prebinned",
        "dither", "contrast", "brightness", "threshold",
        "speed", "loop_blank", "random_transform", "weight",
    }

    for pi, person in enumerate(people):
        if not isinstance(person, dict):
            sys.exit(f"{ROSTER_FILE}.people[{pi}]: must be an object")
        unknown = set(person.keys()) - PERSON_FIELDS - {"_comment"}
        if unknown:
            sys.exit(
                f"{ROSTER_FILE}.people[{pi}]: unknown field(s) "
                f"{sorted(unknown)} — known person fields: "
                f"{sorted(PERSON_FIELDS - {'images'})}"
            )

        person_ctx = f"{ROSTER_FILE}.people[{pi}]"
        name = person.get("name")
        if not name:
            sys.exit(f"{person_ctx}: 'name' is required")
        position = str(person.get("position", "") or "")
        company  = str(person.get("company", "") or "")
        website  = str(person.get("website", "") or "")
        bio      = str(person.get("bio", "") or "")
        # Default group key = name, so a person's variants always pool
        # under the same slot regardless of how many images they have.
        group = person.get("group") or name

        images = person.get("images", [])
        if not isinstance(images, list) or not images:
            sys.exit(
                f"{person_ctx}: 'images' must be a non-empty list "
                "of image specs"
            )

        for ii, img in enumerate(images):
            if not isinstance(img, dict):
                sys.exit(f"{person_ctx}.images[{ii}]: must be an object")
            unknown = set(img.keys()) - IMAGE_FIELDS - {"_comment"}
            if unknown:
                sys.exit(
                    f"{person_ctx}.images[{ii}]: unknown field(s) "
                    f"{sorted(unknown)} — known image fields: "
                    f"{sorted(IMAGE_FIELDS)}"
                )

            ctx = f"{person_ctx}.images[{ii}]"
            entries.append(_build_variant(
                ctx, img, file_map, used_images, used_bins,
                name=str(name), bio=bio, position=position,
                company=company, website=website, group=group,
            ))

    # Surface unused source files so the user notices headshots they
    # may have meant to wire up. Strictly informational — never auto-
    # appended; the JSON is authoritative.
    unused_imgs = sorted(p.name for p in image_files
                         if p.name not in used_images)
    if PREBINNED_DIR.is_dir():
        all_bins = sorted(
            f.relative_to(PREBINNED_DIR).as_posix()
            for f in PREBINNED_DIR.rglob("*.bin")
        )
        unused_bins = [b for b in all_bins if b not in used_bins]
    else:
        unused_bins = []
    if unused_imgs:
        print(f"  · unused image(s) in assets/credits/: "
              f"{', '.join(unused_imgs)}", file=sys.stderr)
    if unused_bins:
        print(f"  · unused bin(s) in assets/credits/prebinned/: "
              f"{', '.join(unused_bins)}", file=sys.stderr)

    return entries


def _build_variant(ctx: str, img: dict,
                   file_map: dict[str, Path],
                   used_images: set[str], used_bins: set[str],
                   *,
                   name: str, bio: str, position: str, company: str,
                   website: str, group: str) -> "RosterEntry":
    """Resolve one image-spec into a RosterEntry, inheriting person metadata.

    Image-spec must supply `file` and/or `prebinned`. `prebinned` may
    be a `.bin` filename (single static frame), a directory name
    (animation), or `true` (auto-detect a same-stemmed `.bin` next to
    the source image). Per-image fields override the dither / animation
    knobs; person fields supply name + bio + company + … for every
    variant in the pool.
    """
    fname = img.get("file")
    image_path: Path | None = None
    if fname:
        image_path = file_map.get(fname)
        if image_path is None:
            sys.exit(
                f"{ctx}: 'file' '{fname}' not found in "
                f"{CREDITS_DIR.relative_to(REPO)}/"
            )
        if fname in used_images:
            sys.exit(f"{ctx}: '{fname}' listed more than once")
        used_images.add(fname)

    prebin_paths: list[Path] = []
    durations: list[int] = []
    prebin_spec = img.get("prebinned")
    if prebin_spec is True and image_path is not None:
        prebin_paths = autodetect_prebin(image_path.stem)
        durations = [DEFAULT_FRAME_MS] * len(prebin_paths)
    elif isinstance(prebin_spec, str):
        prebin_paths, durations = resolve_prebin(prebin_spec, ctx)

    if not prebin_paths and image_path is None:
        sys.exit(
            f"{ctx}: image spec needs at least one of 'file' or "
            f"'prebinned'"
        )

    for p in prebin_paths:
        rel = p.relative_to(PREBINNED_DIR).as_posix()
        if rel in used_bins:
            sys.exit(f"{ctx}: prebin '{rel}' listed more than once")
        used_bins.add(rel)

    speed = float(img.get("speed", 1.0))
    if speed <= 0:
        sys.exit(f"{ctx}: 'speed' must be positive (got {speed})")
    if abs(speed - 1.0) > 0.001 and durations:
        durations = [max(1, int(round(d / speed))) for d in durations]

    loop_blank = bool(img.get("loop_blank", False))
    random_transform = bool(img.get("random_transform", False))
    weight = img.get("weight", None)
    if weight is not None:
        try:
            weight = int(weight)
        except (TypeError, ValueError):
            sys.exit(f"{ctx}: 'weight' must be an integer")

    dither = _dither_from_dict(img, ctx)

    return RosterEntry(
        name=name,
        bio=bio,
        image_path=image_path,
        prebin_paths=prebin_paths,
        frame_durations_ms=durations,
        dither=dither,
        speed=speed,
        loop_blank=loop_blank,
        random_transform=random_transform,
        group=group,
        position=position,
        company=company,
        website=website,
        weight=weight,
    )


def display_name_from_stem(stem: str) -> str:
    """Convert filename stem to a nicer default display label.

    "kev"           -> "Kev"
    "alex_lynd"     -> "Alex Lynd"
    "alex-lynd"     -> "Alex Lynd"
    "aaron-kalair2" -> "Aaron Kalair"  (trailing digits stripped)
    """
    cleaned = "".join(c if c.isalpha() else " " for c in stem)
    parts = [p for p in cleaned.split() if p]
    return " ".join(p.capitalize() for p in parts) or stem


def prepare_grayscale(path: Path, size: int,
                      settings: DitherSettings) -> Image.Image:
    """Load, square-crop, resize, contrast-stretch, and brightness-scale
    the source image. Returns an `L`-mode PIL image at `size`x`size`,
    pre-dither — useful both for the on-target XBM path and for the
    preview comparison grid.
    """
    img = Image.open(path)
    img = ImageOps.exif_transpose(img)
    img = img.convert("L")
    w, h = img.size
    side = min(w, h)
    left = (w - side) // 2
    top  = (h - side) // 2
    img = img.crop((left, top, left + side, top + side))
    img = img.resize((size, size), Image.LANCZOS)
    img = ImageOps.autocontrast(img, cutoff=settings.contrast)
    if abs(settings.brightness - 1.0) > 0.001:
        # Pixel-level brightness scale, clamped at 255 so highlights
        # don't wrap around. Multiplier > 1 brightens, < 1 dims.
        px = img.load()
        for y in range(size):
            for x in range(size):
                v = int(px[x, y] * settings.brightness)
                px[x, y] = 255 if v > 255 else max(0, v)
    return img


def image_to_xbm(path: Path, size: int,
                 settings: DitherSettings = None) -> bytes:
    """Square-crop, dither, and pack a face image into a `size`x`size` XBM.

    Produces packed XBM bytes (LSB-first within each byte; bit 0 of byte 0
    is the leftmost pixel of row 0). Total length = ceil(size/8) * size.
    """
    if settings is None:
        settings = DitherSettings()
    gray = prepare_grayscale(path, size, settings)
    bw = apply_dither(gray, settings.mode, settings.threshold)

    pixels = bw.load()
    stride = (size + 7) // 8
    out = bytearray(stride * size)
    for y in range(size):
        for x in range(size):
            if pixels[x, y]:
                out[y * stride + (x >> 3)] |= 1 << (x & 7)
    return bytes(out)


def format_byte_table(data: bytes, indent: str = "    ", per_row: int = 16) -> str:
    lines = []
    for i in range(0, len(data), per_row):
        chunk = data[i:i + per_row]
        lines.append(f"{indent}{', '.join(f'0x{b:02X}' for b in chunk)},")
    return "\n".join(lines)


def _c_str(value: str) -> str:
    """Escape a Python string into a C string literal body (no surrounding quotes)."""
    out = []
    for ch in value:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif 0x20 <= ord(ch) < 0x7F:
            out.append(ch)
        else:
            # Non-ASCII goes through as raw UTF-8 bytes inside the
            # literal; u8g2's `_tr` fonts only render ASCII, so emit a
            # warning and let the caller decide how to clean it up.
            for b in ch.encode("utf-8"):
                out.append(f"\\x{b:02x}")
    return "".join(out)


def group_roster(entries: list[RosterEntry]) -> list[RosterEntry]:
    """Reorder roster entries so same-`group` entries are contiguous.

    The screen treats each group as one card slot and randomly picks a
    variant whenever the slot scrolls into view, so the runtime data
    layout needs each group's variants to live in adjacent kCredits[]
    slots — `kGroups[i]` then just stores the first credit's index +
    the number of variants. We preserve first-appearance order across
    groups (matches how `credits.json` already reads top-to-bottom),
    and within each group we keep the original order of the listed
    variants — predictable for the human author.
    """
    order: list[str] = []
    bucket: dict[str, list[RosterEntry]] = {}
    for e in entries:
        if e.group not in bucket:
            order.append(e.group)
            bucket[e.group] = []
        bucket[e.group].append(e)
    out: list[RosterEntry] = []
    for key in order:
        out.extend(bucket[key])
    return out


def emit_header(blob: bytes, durations: list[int],
                entries: list[dict],
                groups: list[dict]) -> str:
    """Emit AboutCredits.h.

    `entries` is a list of dicts with keys:
      name, bio, offset, frame_count, dur_offset, loop_blank
    `durations` is the global concatenated table of per-frame display
    times (ms) — `dur_offset` indexes into it; entries with frame_count
    == 1 get dur_offset == 0 by convention (the field is unused).
    """
    bytes_per = ((ICON_SIZE + 7) // 8) * ICON_SIZE

    credit_rows = "\n".join(
        f'    {{"{_c_str(e["name"])}", "{_c_str(e["bio"])}",'
        f' "{_c_str(e["position"])}", "{_c_str(e["company"])}",'
        f' "{_c_str(e["website"])}",'
        f' {e["offset"]}, {e["frame_count"]},'
        f' {e["dur_offset"]}, 0x{e["flags"]:02X}, {e["weight"]}}},'
        for e in entries
    )
    info_rows = "\n".join(
        f'    {{"{_c_str(e["name"])}", &kCreditBits[{e["offset"]}], '
        f'nullptr, {e["frame_count"]}, kStride, 0, nullptr}},'
        for e in entries
    )
    group_rows = "\n".join(
        f'    {{{g["first"]}, {g["count"]}, {g["bag_size"]}}},'
        f'   // {_c_str(g["label"])}'
        for g in groups
    ) if groups else "    {0, 0, 0},"
    durations_body = ", ".join(str(d) for d in durations) if durations else "0"
    durations_len = len(durations) if durations else 1
    group_count = len(groups) if groups else 1
    # The bag = each variant index repeated by its `weight`, shuffled
    # at the start of every cycle through the pool. The screen needs a
    # static buffer big enough for the largest group's bag.
    max_bag = max((g["bag_size"] for g in groups), default=1)
    if max_bag < 1:
        max_bag = 1

    return textwrap.dedent("""\
        // Generated by scripts/gen_credit_xbms.py — do not edit by hand.
        // Order, names, and bios are sourced from
        // assets/credits/credits.json; edit that file and re-run the
        // generator instead of patching this header.

        #pragma once

        #include <Arduino.h>

        #include "../ui/Images.h"  // ImageInfo for ImageScaler integration

        namespace AboutCredits {{

        constexpr uint8_t  kIconSize     = {icon};
        constexpr uint16_t kBytesPerFrame = {bpp};   // (W/8) * H per single frame
        constexpr uint16_t kStride       = kBytesPerFrame;
        constexpr uint8_t  kCount        = {count};
        constexpr uint8_t  kGroupCount   = {group_count};
        constexpr uint8_t  kMaxBagSize   = {max_bag};   // largest group's variant bag
        constexpr uint16_t kBitsLen      = {bits_len};

        constexpr uint16_t kAnimLoopPauseMs   = {anim_pause_ms};
        constexpr uint16_t kFrameDurationsLen = {durations_len};

        // Per-credit animation flags (Credit::flags bit positions). Keep
        // in sync with ANIM_FLAG_* in scripts/gen_credit_xbms.py.
        constexpr uint8_t kFlagLoopBlank       = 1 << 0;
        constexpr uint8_t kFlagRandomTransform = 1 << 1;

        struct Credit {{
          const char* name;        // display name (large font; may contain '\\n')
          const char* bio;         // free-form text (small font, wrapped)
          const char* position;    // job title (small font, single line)
          const char* company;     // affiliation (small font, single line)
          const char* website;     // url / handle (small font, single line)
          uint16_t    offset;      // first frame of this credit in kCreditBits
          uint8_t     frameCount;  // 1 = static; >1 = animated, frames stored back-to-back
          uint16_t    durOffset;   // index into kFrameDurationsMs[] (unused when frameCount == 1)
          uint8_t     flags;       // bit-packed kFlag* (loop-blank, random-transform, ...)
          uint8_t     weight;      // weighted-random pool selection (default 1; animated default 3)
        }};

        // Packed XBM, LSB-first within each byte (oled::drawXBM order).
        // Animated credits stack every frame at offset, offset+kBytesPerFrame,
        // offset+2*kBytesPerFrame, and so on.
        static const uint8_t kCreditBits[kBitsLen] PROGMEM = {{
        {bits_body}
        }};

        // Concatenated per-frame durations in milliseconds. Each animated
        // credit's run starts at its `durOffset` and spans `frameCount`
        // entries. After cycling through every frame the screen pauses
        // for kAnimLoopPauseMs (holding the last frame, or rendering a
        // blank cell when kFlagLoopBlank is set) before restarting.
        // When kFlagRandomTransform is set the screen also picks a new
        // random rotation+flip for each loop iteration so successive
        // cycles never look identical.
        static const uint16_t kFrameDurationsMs[kFrameDurationsLen] PROGMEM = {{
            {durations_body}
        }};

        static const Credit kCredits[kCount] = {{
        {credits}
        }};

        // Card-slot groupings. Credits whose JSON `group` keys match
        // (default: their `name`) are pooled into a single on-screen
        // slot — every time the slot scrolls into view (or once per
        // ~6 s when a static is being held), the screen advances to
        // the next entry in a shuffled bag containing each variant
        // index repeated by its `weight`. So `weight: 2` means a
        // variant appears twice per pass through the pool, and the
        // bag guarantees no repeats inside a single pass.
        // `bagSize` = sum of weights, sized for the bag buffer.
        struct CreditGroup {{
          uint8_t firstCredit;
          uint8_t count;
          uint8_t bagSize;
        }};

        static const CreditGroup kGroups[kGroupCount] = {{
        {groups}
        }};

        // ImageInfo wrappers — drop into any UI that already speaks the
        // zigmoji ImageScaler vocabulary (StickerPicker, etc.). Single
        // source (no 48×48 sibling), so data48 is nullptr; ImageScaler::
        // getFrame falls through to data64 for any target dimension.
        static const ImageInfo kImageInfos[kCount] = {{
        {infos}
        }};

        }}  // namespace AboutCredits
        """).format(
            icon=ICON_SIZE,
            bpp=bytes_per,
            count=len(entries),
            group_count=group_count,
            max_bag=max_bag,
            bits_len=len(blob),
            bits_body=format_byte_table(blob),
            anim_pause_ms=ANIM_LOOP_PAUSE_MS,
            durations_len=durations_len,
            durations_body=durations_body,
            credits=credit_rows,
            groups=group_rows,
            infos=info_rows,
        )


def emit_python_app(blob: bytes,
                    entries: list[tuple[str, str, int]]) -> str:
    """Render the MicroPython companion app for the badge filesystem.

    Uses the same 64×64 blob the native screen reads. The set_pixel walk
    is ~4096 calls per redraw, which is slower than the native drawXBM
    path but only runs on credit changes (not every frame), so it stays
    snappy enough for a viewing app. For animated credits, the Python
    app shows only the first frame — animation playback is native-only.
    """
    credits_rows = ",\n".join(
        f"    ({name!r}, {bio!r}, {off})" for name, bio, off in entries
    )
    data_lines = []
    for i in range(0, len(blob), 16):
        chunk = blob[i:i + 16]
        data_lines.append(
            '    b"' + ''.join(f'\\x{b:02x}' for b in chunk) + '"'
        )
    data_block = "\n".join(data_lines)

    return textwrap.dedent('''\
        """credits.py — Scroll through the Temporal badge crew, one headshot at a time.

        Each credit is a 64x64 dithered XBM headshot. Joystick / left+
        right buttons cycle credits, BACK exits.

        Generated by ``scripts/gen_credit_xbms.py`` from the same source
        bytes as the native ``AboutCreditsScreen`` (same blob, same
        offsets). For animated credits this app shows only the first
        frame — animation playback is native-only.
        """

        import time

        ICON_SIZE = {icon}
        ICON_STRIDE = {stride}  # bytes per row in one icon (LSB-first XBM)
        ICON_BYTES = ICON_STRIDE * ICON_SIZE

        # (display_name, bio, byte_offset_into_BITS)
        CREDITS = (
        {credits},
        )

        # Concatenated 64x64 XBM bitmaps, one per credit (first frame
        # only for animated credits). LSB-first within each byte: bit 0
        # of byte 0 is the leftmost pixel of row 0. Machine-generated
        # blob — edit assets/credits/ + re-run scripts/gen_credit_xbms.py
        # instead of patching this in place.
        BITS = (
        {data}
        )

        ICON_X = 0
        ICON_Y = 0
        TEXT_X = 68   # right of the 64-wide image + 4 px breather
        NAME_Y = 18
        ROLE_Y = 32

        JOY_LOW = 1100
        JOY_HIGH = 3000
        NAV_MS = 220


        def draw_icon(offset, x0, y0):
            # Walk the 64 rows of 8 bytes each and light pixels where the
            # XBM bit is set. ~4096 set_pixel calls — only runs on credit
            # change, so no per-frame budget concern.
            for row in range(ICON_SIZE):
                base = offset + row * ICON_STRIDE
                for col_byte in range(ICON_STRIDE):
                    byte = BITS[base + col_byte]
                    if byte == 0:
                        continue
                    x_base = x0 + (col_byte << 3)
                    for bit in range(8):
                        col = (col_byte << 3) + bit
                        if col >= ICON_SIZE:
                            break
                        if byte & (1 << bit):
                            oled_set_pixel(x_base + bit, y0 + row, 1)


        def render(idx):
            name, role, offset = CREDITS[idx]
            try:
                import badge_ui as ui
                ui.chrome("Credits",
                          str(idx + 1) + "/" + str(len(CREDITS)),
                          "<>", "browse", "BACK", "exit")
            except Exception:
                oled_clear()
                oled_set_cursor(0, 0)
                oled_print("Credits " + str(idx + 1) + "/" +
                           str(len(CREDITS)))
            draw_icon(offset, ICON_X, ICON_Y)
            oled_set_cursor(TEXT_X, NAME_Y)
            oled_print(name)
            if role:
                oled_set_cursor(TEXT_X, ROLE_Y)
                oled_print(role)
            oled_show()


        def step(idx, direction):
            return (idx + direction) % len(CREDITS)


        idx = 0
        render(idx)
        last_nav = 0

        while True:
            if button_pressed(BTN_BACK):
                break

            direction = 0
            if button_pressed(BTN_RIGHT) or button_pressed(BTN_CONFIRM):
                direction = 1
            elif button_pressed(BTN_LEFT):
                direction = -1
            else:
                now = time.ticks_ms()
                if time.ticks_diff(now, last_nav) >= NAV_MS:
                    jx = joy_x()
                    if jx > JOY_HIGH:
                        direction = 1
                    elif jx < JOY_LOW:
                        direction = -1
                    if direction:
                        last_nav = now

            if direction:
                idx = step(idx, direction)
                render(idx)

            time.sleep_ms(30)

        oled_clear(True)
        exit()
        ''').format(
            icon=ICON_SIZE,
            stride=(ICON_SIZE + 7) // 8,
            credits=credits_rows,
            data=data_block,
        )


def render_preview_sheet(entry: RosterEntry, size: int,
                         scale: int = 6) -> Image.Image:
    """Build a side-by-side comparison sheet for one headshot.

    Renders the source image through every preset in DITHER_MODES at
    the requested target size, upscales each tile by `scale` so dot
    patterns are legible on a desktop monitor, labels each tile, and
    pastes them in a row. Used by `--preview` to help pick a per-credit
    dither setting without having to flash and squint.
    """
    tiles: list[Image.Image] = []
    labels: list[str] = []
    base_settings = entry.dither

    # The preview always exercises the dither pipeline, so we need a
    # source image even when the on-target render would come from a
    # prebinned .bin. Skip credits that have only a .bin (e.g. the
    # animation-only entries) — there's nothing to A/B for those.
    if entry.image_path is None:
        return None

    for mode in DITHER_MODES:
        settings = DitherSettings(
            mode=mode,
            contrast=base_settings.contrast,
            brightness=base_settings.brightness,
            threshold=base_settings.threshold,
        )
        gray = prepare_grayscale(entry.image_path, size, settings)
        bw = apply_dither(gray, mode, settings.threshold)
        tile = bw.convert("L").resize(
            (size * scale, size * scale), Image.NEAREST
        )
        tiles.append(tile)
        marker = " ★" if mode == base_settings.mode else ""
        labels.append(f"{mode}{marker}")

    pad = 8
    label_h = 14
    tile_w = size * scale
    tile_h = size * scale
    sheet_w = pad + (tile_w + pad) * len(tiles)
    sheet_h = pad + label_h + tile_h + pad

    sheet = Image.new("L", (sheet_w, sheet_h), color=255)
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype(
            "/System/Library/Fonts/Supplemental/Menlo.ttc", 11
        )
    except OSError:
        font = ImageFont.load_default()

    src_label = entry.image_path.name if entry.image_path else "(prebin only)"
    title = f"{entry.name}  ({src_label}, {size}x{size})"
    draw.text((pad, 0), title, fill=0, font=font)

    x = pad
    for tile, label in zip(tiles, labels):
        draw.text((x, label_h - 12), label, fill=0, font=font)
        sheet.paste(tile, (x, label_h))
        x += tile_w + pad
    return sheet


def cmd_preview(roster: list[RosterEntry], size: int) -> None:
    """Write one comparison PNG per credit to assets/credits/preview/.

    The PNGs are deliberately desktop-sized (default ~6× upscale) so a
    human can A/B between Floyd-Steinberg's noise pattern, Atkinson's
    softer midtones, and Bayer's regular grid before committing a mode
    in credits.json.
    """
    PREVIEW_DIR.mkdir(parents=True, exist_ok=True)
    for entry in roster:
        sheet = render_preview_sheet(entry, size)
        if sheet is None:
            print(f"  · {entry.name:<24} skipped (no source image, "
                  f"prebin-only entry)")
            continue
        stem = entry.image_path.stem
        out = PREVIEW_DIR / f"{stem}_{size}.png"
        sheet.save(out)
        print(f"  ✓ {entry.name:<24} {size}x{size}  → "
              f"{out.relative_to(REPO)}")
    print()
    print(f"Open {PREVIEW_DIR.relative_to(REPO)}/ to compare. "
          "Once you pick a winner, set it in assets/credits/credits.json:")
    print('   { "file": "...", "name": "...", "dither": "atkinson" }')
    print("then re-run this script (without --preview) to bake the result.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--preview", action="store_true",
        help="Render side-by-side comparison PNGs to "
             f"{PREVIEW_DIR.relative_to(REPO)}/ instead of writing the "
             "C++ header / Python app. Use to A/B dither modes before "
             "committing a choice in credits.json.",
    )
    parser.add_argument(
        "--preview-size", type=int, default=ICON_SIZE,
        help="Target dimension for preview tiles (default: 64).",
    )
    args = parser.parse_args()

    if not CREDITS_DIR.is_dir():
        sys.exit(f"Credits dir not found: {CREDITS_DIR}")

    sources = sorted(
        p for p in CREDITS_DIR.iterdir()
        if p.is_file() and p.suffix.lower() in SUPPORTED_EXTS
    )
    # No-source-images is fine when every entry is prebin-only —
    # `load_roster` will catch a credits.json that references a
    # missing `file:` and abort with a useful message there.

    roster = load_roster(sources)

    if args.preview:
        cmd_preview(roster, args.preview_size)
        return

    # Reshuffle so same-group entries are contiguous in kCredits[]; the
    # screen relies on this layout for its on-enter random variant pick.
    roster = group_roster(roster)

    blob = bytearray()
    durations: list[int] = []
    header_entries: list[dict] = []
    py_entries: list[tuple[str, str, int]] = []
    # Track group boundaries while the credit array fills out so we can
    # emit a parallel kGroups[] table without a second pass.
    groups: list[dict] = []
    current_group_key: str | None = None
    for entry in roster:
        # Resolve frames — either straight out of one or more .bin
        # files, or by dithering the source image.
        if entry.is_prebinned:
            frames = [read_bin_xbm(p) for p in entry.prebin_paths]
            source_label = "prebin"
        else:
            frames = [image_to_xbm(entry.image_path, ICON_SIZE,
                                   entry.dither)]
            source_label = entry.dither.label()

        offset = len(blob)
        for f in frames:
            blob.extend(f)

        # Per-frame durations are only meaningful for animated credits.
        # Static credits get durOffset=0 and a frameCount of 1; the
        # screen never reads kFrameDurationsMs[] for them.
        if entry.is_animated:
            dur_offset = len(durations)
            durations.extend(entry.frame_durations_ms)
        else:
            dur_offset = 0

        header_entries.append({
            "name": entry.name,
            "bio": entry.bio,
            "position": entry.position,
            "company": entry.company,
            "website": entry.website,
            "offset": offset,
            "frame_count": len(frames),
            "dur_offset": dur_offset,
            "flags": entry.flags_byte(),
            "weight": entry.weight,
        })
        # Open a new group when the key changes (and close the previous
        # one). `roster` is already ordered so every variant of a group
        # appears consecutively, so a simple "did the key just change?"
        # check is enough to bookend each group.
        if entry.group != current_group_key:
            groups.append({
                "label": entry.group,
                "first": len(header_entries) - 1,
                "count": 1,
                "bag_size": entry.weight,
            })
            current_group_key = entry.group
        else:
            groups[-1]["count"] += 1
            groups[-1]["bag_size"] += entry.weight
        # The Python companion app shows the first frame for any credit
        # (animated or not) — drop just that frame's offset into the
        # tuple we hand it.
        py_entries.append((entry.name, entry.bio, offset))

        bio_suffix = f"  — {entry.bio}" if entry.bio else ""
        anim_suffix = ""
        if entry.is_animated:
            total_ms = sum(entry.frame_durations_ms)
            speed_suffix = (f" @{entry.speed:.2f}×"
                            if abs(entry.speed - 1.0) > 0.001 else "")
            extras = []
            if entry.loop_blank: extras.append("blank-pause")
            if entry.random_transform: extras.append("random-xform")
            extras_suffix = (", " + ", ".join(extras)) if extras else ""
            anim_suffix = (f"  [anim {len(frames)}f, {total_ms}ms loop"
                           f"{speed_suffix}{extras_suffix}]")
        print(
            f"  ✓ {entry.name:<24} {source_label:<14}  "
            f"({len(frames) * len(frames[0])}b)  "
            f"← {entry.display_source}{bio_suffix}{anim_suffix}"
        )

    OUT_HEADER.parent.mkdir(parents=True, exist_ok=True)
    OUT_HEADER.write_text(emit_header(
        bytes(blob), durations, header_entries, groups
    ))

    OUT_APP.parent.mkdir(parents=True, exist_ok=True)
    OUT_APP.write_text(emit_python_app(bytes(blob), py_entries))

    print()
    print(f"Header: {OUT_HEADER.relative_to(REPO)}  "
          f"({len(blob):,}b, {len(header_entries)} variants in "
          f"{len(groups)} group{'s' if len(groups) != 1 else ''})")
    pooled = [g for g in groups if g["count"] > 1]
    if pooled:
        details = ", ".join(f"{g['label']}×{g['count']}" for g in pooled)
        print(f"        pooled: {details}")
    print(f"App:    {OUT_APP.relative_to(REPO)}")


if __name__ == "__main__":
    main()
