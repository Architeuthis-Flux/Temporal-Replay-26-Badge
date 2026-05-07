#!/usr/bin/env python3
"""
Render sponsor SVG logos to combined XBM byte blobs at TWO uniform heights
and emit one C++ header per height. The About screen uses the larger
32-px set; the SponsorBooth modal marquee uses the smaller 26-px set
so logos fit inside the modal chrome with room to scroll vertically.

Usage:
  python3 scripts/gen_sponsor_xbms.py

Inputs:
  /home/alex/Projects/Clients/Temporal/Badge/assets/Sponsor Logos/*.svg

Outputs:
  firmware/src/screens/AboutSponsors.h   (height=32, namespace AboutSponsors)
  firmware/src/screens/BoothSponsors.h   (height=26, namespace BoothSponsors)

Each header exposes:
    <ns>::kHeight       constexpr uint8_t (the rendered height)
    <ns>::kCount        constexpr uint8_t
    <ns>::kSponsorBits  uint8_t[] PROGMEM — concatenated XBM bytes
                        (row-major, LSB-first, sponsor bytes contiguous).
    <ns>::kSponsors[]   array of Sponsor{name, width, byteOffset, stride}.

Renders SVGs through whichever of these is available, in order:
  1. rsvg-convert (apt install librsvg2-bin)
  2. cairosvg     (pip install cairosvg)
  3. inkscape     (system package)
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import textwrap
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

REPO = Path(__file__).resolve().parent.parent
SPONSOR_DIR = Path(
    "/home/alex/Projects/Clients/Temporal/Badge/assets/Sponsor Logos"
)
SCREENS_DIR = REPO / "firmware" / "src" / "screens"

# (height_px, output_header, namespace) — order matters only for log readability.
# About screen keeps the original 32-px set; the sponsor-booth modal
# marquee uses a tighter 26-px set so multiple logos can scroll inside
# the modal body without truncation.
RENDER_TARGETS = [
    (32, SCREENS_DIR / "AboutSponsors.h", "AboutSponsors"),
    (26, SCREENS_DIR / "BoothSponsors.h", "BoothSponsors"),
]

LIT_THRESHOLD = 96  # pixels brighter than this become lit bits

# (svg-stem, display name) — display name is what the AboutScreen credits.
SPONSORS = [
    ("apartment-304-white 1",          "Apartment 304"),
    ("augment-code-2026-logo-white 1", "Augment Code"),
    ("aws",                            "AWS"),
    ("bitovi-logo-wh 1",               "Bitovi"),
    ("braintrust-logo-light",          "Braintrust"),
    ("Google-for-startups-white",      "Google for Startups"),
    ("grid-dynamics-logo-wh",          "Grid Dynamics"),
    ("Liatro-logo-wh",                 "Liatro"),
    ("Tailscale_logo_wht_",            "Tailscale"),
    ("technoidentity-wh-logo 1",       "TechnoIdentity"),
]


# ── SVG → PNG renderers ─────────────────────────────────────────────────────

def _have(cmd: str) -> bool:
    return shutil.which(cmd) is not None


def _render_rsvg(svg: Path, png: Path, height: int) -> bool:
    if not _have("rsvg-convert"):
        return False
    subprocess.run(
        ["rsvg-convert", "-h", str(height), "--keep-aspect-ratio",
         "-o", str(png), str(svg)],
        check=True, capture_output=True,
    )
    return True


def _render_cairosvg(svg: Path, png: Path, height: int) -> bool:
    try:
        import cairosvg  # type: ignore
    except ImportError:
        return False
    cairosvg.svg2png(url=str(svg), write_to=str(png), output_height=height)
    return True


def _render_inkscape(svg: Path, png: Path, height: int) -> bool:
    if not _have("inkscape"):
        return False
    subprocess.run(
        ["inkscape", str(svg),
         "--export-type=png",
         f"--export-filename={png}",
         f"--export-height={height}",
         "--export-background-opacity=0"],
        check=True, capture_output=True,
    )
    return True


def render_svg(svg: Path, png: Path, height: int) -> str:
    for name, fn in (("rsvg-convert", _render_rsvg),
                     ("cairosvg",      _render_cairosvg),
                     ("inkscape",      _render_inkscape)):
        try:
            if fn(svg, png, height):
                return name
        except subprocess.CalledProcessError as e:
            stderr = (e.stderr or b"").decode("utf-8", "replace")[:200]
            print(f"  ! {name} failed for {svg.name}: {stderr}",
                  file=sys.stderr)
    sys.exit(
        "No SVG renderer available. Install one of:\n"
        "  apt install librsvg2-bin   # rsvg-convert\n"
        "  pip install cairosvg\n"
        "  apt install inkscape"
    )


# ── PIL → XBM byte-row encoder ──────────────────────────────────────────────

def image_to_xbm(img: Image.Image, target_height: int) -> tuple[bytes, int, int]:
    """Convert an image to XBM byte rows at the requested height.

    XBM byte layout: row-major; within each byte, bit 0 (LSB) is leftmost
    pixel of that 8-pixel column. This matches U8G2 drawXBM() expectations.
    Returns (bytes, width, stride).
    """
    if img.mode in ("RGBA", "LA"):
        bg = Image.new("RGBA", img.size, (0, 0, 0, 255))
        bg.paste(img, mask=img.split()[-1])
        img = bg.convert("L")
    elif img.mode != "L":
        img = img.convert("L")

    w, h = img.size
    if h != target_height:
        new_w = max(1, round(w * target_height / h))
        img = img.resize((new_w, target_height), Image.LANCZOS)
        w = new_w
        h = target_height

    pixels = img.load()
    stride = (w + 7) // 8
    out = bytearray(stride * h)
    for y in range(h):
        for x in range(w):
            if pixels[x, y] > LIT_THRESHOLD:
                out[y * stride + (x >> 3)] |= 1 << (x & 7)
    return bytes(out), w, stride


# ── Header emission ─────────────────────────────────────────────────────────

def _format_byte_table(name: str, data: bytes, indent: str = "    ") -> str:
    """Format a uint8_t array body with 16 bytes per line."""
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        hex_str = ", ".join(f"0x{b:02X}" for b in chunk)
        lines.append(f"{indent}{hex_str},")
    return "\n".join(lines)


def emit_header(combined: bytes,
                entries: list[tuple[str, int, int, int]],
                height: int,
                namespace: str) -> str:
    """entries: (display_name, width, byte_offset, stride)"""
    body_table = _format_byte_table("kSponsorBits", combined)

    sponsor_rows = []
    for name, w, off, stride in entries:
        sponsor_rows.append(
            f'    {{"{name}", {w}, {off}, {stride}}},'
        )

    return textwrap.dedent("""\
        // Generated by scripts/gen_sponsor_xbms.py — do not edit by hand.
        //
        // Sponsor logos rendered to XBM at uniform {height}px height. Each
        // sponsor's bytes are contiguous inside kSponsorBits — pass
        // (kSponsorBits + kSponsors[i].byteOffset) to U8G2 drawXBM with
        // width = kSponsors[i].width and height = kHeight.

        #pragma once

        #include <Arduino.h>

        namespace {ns} {{

        constexpr uint8_t  kHeight = {height};
        constexpr uint8_t  kCount  = {count};
        constexpr uint16_t kBitsLen = {bits_len};

        struct Sponsor {{
          const char* name;
          uint16_t    width;       // pixel width
          uint16_t    byteOffset;  // start of this sponsor's rows in kSponsorBits
          uint8_t     stride;      // bytes per row ((width + 7) / 8)
        }};

        static const uint8_t kSponsorBits[kBitsLen] PROGMEM = {{
        {bits_body}
        }};

        static const Sponsor kSponsors[kCount] = {{
        {sponsors}
        }};

        }}  // namespace {ns}
        """).format(
            ns=namespace,
            height=height,
            count=len(entries),
            bits_len=len(combined),
            bits_body=body_table,
            sponsors="\n".join(sponsor_rows),
        )


# ── Main ────────────────────────────────────────────────────────────────────

def render_set(height: int,
               out_header: Path,
               namespace: str) -> str | None:
    """Render every sponsor at `height`, write `out_header`. Returns the
    SVG renderer name actually used (or None if nothing was rendered)."""
    out_header.parent.mkdir(parents=True, exist_ok=True)
    combined = bytearray()
    entries: list[tuple[str, int, int, int]] = []
    renderer_used: str | None = None
    print(f"== {namespace} (h={height}) ==")
    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = Path(tmp)
        for stem, name in SPONSORS:
            svg = SPONSOR_DIR / f"{stem}.svg"
            if not svg.exists():
                print(f"  - {name}: SVG not found ({svg.name}) — skipping")
                continue
            png = tmp_dir / f"{stem}.png"
            renderer_used = render_svg(svg, png, height)
            xbm, real_w, stride = image_to_xbm(Image.open(png), height)
            offset = len(combined)
            entries.append((name, real_w, offset, stride))
            combined.extend(xbm)
            print(f"  ✓ {name:<22} {real_w}x{height}, "
                  f"{len(xbm)} bytes  @offset {offset}")
    if not entries:
        print(f"  (no sponsors converted for {namespace})")
        return renderer_used
    out_header.write_text(emit_header(bytes(combined), entries, height,
                                      namespace))
    print(f"  → {out_header.relative_to(REPO)} "
          f"({len(combined):,} bytes, {len(entries)} sponsors)")
    return renderer_used


def main() -> None:
    if not SPONSOR_DIR.is_dir():
        sys.exit(f"Sponsor logo dir not found: {SPONSOR_DIR}")
    last_renderer: str | None = None
    for height, out_header, namespace in RENDER_TARGETS:
        last_renderer = render_set(height, out_header, namespace) or last_renderer
        print()
    print(f"Renderer: {last_renderer}")


if __name__ == "__main__":
    main()
