#!/usr/bin/env python3
"""
Convert Zig-moji and sponsor logo assets to SSD1306 framebuffer binary files.

Usage:
  python3 scripts/convert_zigmoji.py

Output:
  firmware/initial_filesystem/apps/zigmoji/*.fb

Each .fb file is exactly 1024 bytes: the SSD1306/U8G2 128×64 page-format
framebuffer, pre-rotated 180° to compensate for the firmware's rotation in
temporalbadge_runtime_oled_set_framebuffer().

SSD1306 page format:
  - 8 pages of 128 bytes (page p covers rows p*8 .. p*8+7)
  - byte at buf[page*128 + col]: bit n (0=LSB) is the pixel at row page*8+n
  - Total: 1024 bytes

Requires: Pillow (pip install Pillow)
Optional: rsvg-convert (brew install librsvg) for SVG assets
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

REPO = Path(__file__).parent.parent
ASSETS = REPO / "assets"
OUT_DIR = REPO / "firmware" / "initial_filesystem" / "apps" / "zigmoji"

DISPLAY_W = 128
DISPLAY_H = 64
FB_BYTES = DISPLAY_W * DISPLAY_H // 8  # 1024


# ── Framebuffer helpers ────────────────────────────────────────────────────────

def _reverse_bits(b: int) -> int:
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4)
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2)
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1)
    return b


def _rotate_180(buf: bytes) -> bytes:
    """Pre-rotate framebuffer 180° (same op as firmware's rotateFramebuffer180)."""
    arr = bytearray(buf)
    lo, hi = 0, len(arr) - 1
    while lo < hi:
        arr[lo], arr[hi] = _reverse_bits(arr[hi]), _reverse_bits(arr[lo])
        lo += 1
        hi -= 1
    if lo == hi:
        arr[lo] = _reverse_bits(arr[lo])
    return bytes(arr)


def _img_to_fb(img: "Image.Image", x_off: int = 0, y_off: int = 0) -> bytes:
    """
    Convert a PIL image to a 1024-byte SSD1306 framebuffer.
    White/bright pixels → lit; dark/transparent pixels → unlit.
    The image is placed at (x_off, y_off) on the 128×64 canvas.
    """
    # Flatten transparency onto black background, then to grayscale
    if img.mode in ("RGBA", "LA"):
        bg = Image.new("RGBA", img.size, (0, 0, 0, 255))
        bg.paste(img, mask=img.split()[-1])
        img = bg.convert("L")
    elif img.mode != "L":
        img = img.convert("L")

    buf = bytearray(FB_BYTES)
    w, h = img.size
    pixels = img.load()

    for y in range(h):
        yd = y + y_off
        if yd < 0 or yd >= DISPLAY_H:
            continue
        page = yd // 8
        bit = yd % 8
        for x in range(w):
            xd = x + x_off
            if xd < 0 or xd >= DISPLAY_W:
                continue
            if pixels[x, y] > 127:
                buf[page * DISPLAY_W + xd] |= (1 << bit)

    return _rotate_180(bytes(buf))


def _save_fb(data: bytes, out_path: Path) -> None:
    assert len(data) == FB_BYTES, f"Expected {FB_BYTES} bytes, got {len(data)}"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(data)
    print(f"  ✓ {out_path.name}")


# ── PNG conversion ─────────────────────────────────────────────────────────────

def convert_png(src: Path, out: Path, target: int = 64) -> None:
    img = Image.open(src).convert("RGBA")
    if img.size != (target, target):
        img = img.resize((target, target), Image.LANCZOS)
    x_off = (DISPLAY_W - target) // 2
    y_off = (DISPLAY_H - target) // 2
    _save_fb(_img_to_fb(img, x_off, y_off), out)


# ── SVG conversion ─────────────────────────────────────────────────────────────

def _has_rsvg() -> bool:
    try:
        subprocess.run(["rsvg-convert", "--version"],
                       capture_output=True, check=True)
        return True
    except (FileNotFoundError, subprocess.CalledProcessError):
        return False


def convert_svg(src: Path, out: Path,
                max_w: int = 120, max_h: int = 56) -> bool:
    """
    Convert an SVG to a .fb file via rsvg-convert.
    Returns True on success, False if rsvg-convert is unavailable.
    """
    if not _has_rsvg():
        return False

    with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
        tmp_path = tmp.name

    try:
        subprocess.run(
            ["rsvg-convert",
             "-w", str(max_w), "-h", str(max_h),
             "--keep-aspect-ratio",
             "-o", tmp_path,
             str(src)],
            check=True, capture_output=True
        )
        img = Image.open(tmp_path).convert("RGBA")
        x_off = (DISPLAY_W - img.width) // 2
        y_off = (DISPLAY_H - img.height) // 2
        _save_fb(_img_to_fb(img, x_off, y_off), out)
        return True
    except subprocess.CalledProcessError as e:
        print(f"  ✗ rsvg-convert failed for {src.name}: {e.stderr.decode()[:80]}")
        return False
    finally:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass


# ── Main ───────────────────────────────────────────────────────────────────────

def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    zmoji = ASSETS / "Zig-moji"

    # ── PNG animations ─────────────────────────────────────────────────────────
    png_animations = [
        ("blink",  zmoji / "Animated/Ziggy-blink/64x64"),
        ("fly",    zmoji / "Animated/Ziggy-fly/64x64"),
        ("heart",  zmoji / "Animated/Ziggy-heart/64x64"),
        ("sleep",  zmoji / "Animated/Ziggy-sleep/64x64"),
        ("wow",    zmoji / "Animated/Ziggy-wow/64x64"),
    ]

    for name, folder in png_animations:
        frames = sorted(folder.glob("*.png"))
        if not frames:
            print(f"\n{name}: no PNGs in {folder}")
            continue
        print(f"\n{name} ({len(frames)} frames):")
        for i, frame in enumerate(frames, 1):
            convert_png(frame, OUT_DIR / f"{name}_{i:02d}.fb")

    # ── Static PNG images ──────────────────────────────────────────────────────
    statics = [
        ("s_ziggy",   zmoji / "Static/Ziggy-full-static/Ziggy-full-64x64.png"),
        ("s_zighead", zmoji / "Static/Ziggy-zoom-static/Ziggy-head-64x64.png"),
        ("s_tlogo",   zmoji / "Static/Temporal-logo-static/logomark-white-64x64.png"),
        ("s_theart",  zmoji / "Static/Temporal-heart-static/temporal-heart-64x64.png"),
    ]

    print("\nstatic images:")
    for name, path in statics:
        if path.exists():
            convert_png(path, OUT_DIR / f"{name}.fb")
        else:
            print(f"  - {name}: not found ({path.name})")

    # ── SVG animations ─────────────────────────────────────────────────────────
    svg_animations = [
        ("theart", zmoji / "Animated/Temporal-heart-expand"),
        ("starry", zmoji / "Animated/Ziggy-starry-eyed"),
        ("vulcan", zmoji / "Animated/Vulcan-wave"),
    ]

    print("\nSVG animations (requires rsvg-convert):")
    for name, folder in svg_animations:
        frames = sorted(folder.glob("*64x64.svg"))
        if not frames:
            print(f"  - {name}: no 64×64 SVGs found")
            continue
        print(f"  {name} ({len(frames)} frames):")
        for i, frame in enumerate(frames, 1):
            ok = convert_svg(frame, OUT_DIR / f"{name}_{i:02d}.fb")
            if not ok:
                print("  (skipping SVG animations — rsvg-convert not found)")
                break

    # ── Sponsor logos (SVG) ────────────────────────────────────────────────────
    sponsor_dir = ASSETS / "Sponsor Logos"
    sponsor_map = [
        ("apartment-304-white 1",       "sp_apartment"),
        ("augment-code-2026-logo-white 1", "sp_augment"),
        ("bitovi-logo-wh 1",            "sp_bitovi"),
        ("braintrust-logo-light",       "sp_braintrust"),
        ("Google-for-startups-white",   "sp_google"),
        ("grid-dynamics-logo-wh",       "sp_grid"),
        ("Liatro-logo-wh",              "sp_liatro"),
        ("Tailscale_logo_wht_",         "sp_tailscale"),
        ("technoidentity-wh-logo 1",    "sp_techno"),
    ]

    print("\nSponsor logos (requires rsvg-convert):")
    for stem, out_name in sponsor_map:
        svg_path = sponsor_dir / f"{stem}.svg"
        if not svg_path.exists():
            print(f"  - {out_name}: SVG not found")
            continue
        ok = convert_svg(svg_path, OUT_DIR / f"{out_name}.fb",
                         max_w=120, max_h=40)
        if not ok:
            print("  (skipping sponsor logos — rsvg-convert not found)")
            break

    # ── Summary ────────────────────────────────────────────────────────────────
    fb_files = sorted(OUT_DIR.glob("*.fb"))
    total = sum(f.stat().st_size for f in fb_files)
    print(f"\nDone: {len(fb_files)} .fb files → {OUT_DIR.relative_to(REPO)}")
    print(f"Total size: {total:,} bytes ({total / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
