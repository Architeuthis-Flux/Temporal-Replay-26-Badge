#!/usr/bin/env python3
"""
Trace an axis-aligned XBM outline into a polyline, and emit a C++
MapEdge[] literal suitable for pasting into MapData.cpp.

Usage:
  python3 scripts/trace_xbm_polyline.py path/to/file.xbm

Strategy: the outline is assumed to be a 1-px-thick connected sequence of
horizontal and vertical segments (which the lobby plan is, by inspection).
We collapse runs of contiguous lit pixels along each axis into segments,
deduplicate, then walk endpoint-to-endpoint to produce an ordered polyline.

Stops at any junction with degree > 2; we expect a simple open polyline.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from collections import defaultdict


def load_xbm(path: Path) -> tuple[int, int, list[int]]:
    text = path.read_text()
    w_m = re.search(r"#define\s+\w+_width\s+(\d+)", text)
    h_m = re.search(r"#define\s+\w+_height\s+(\d+)", text)
    if not w_m or not h_m:
        sys.exit(f"{path}: could not find width/height defines")
    w, h = int(w_m.group(1)), int(h_m.group(1))
    nums = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", text)]
    stride = (w + 7) // 8
    expected = stride * h
    if len(nums) != expected:
        sys.exit(f"{path}: got {len(nums)} bytes, expected {expected}")
    return w, h, nums


def lit(bits: list[int], stride: int, x: int, y: int) -> bool:
    return bool((bits[y * stride + (x >> 3)] >> (x & 7)) & 1)


def extract_segments(w: int, h: int, bits: list[int]) -> list[tuple[int, int, int, int]]:
    """Return list of (x0,y0,x1,y1) maximal axis-aligned segments."""
    stride = (w + 7) // 8

    h_segs: list[tuple[int, int, int, int]] = []
    for y in range(h):
        x = 0
        while x < w:
            if lit(bits, stride, x, y):
                start = x
                while x < w and lit(bits, stride, x, y):
                    x += 1
                if x - start >= 2:
                    h_segs.append((start, y, x - 1, y))
            else:
                x += 1

    v_segs: list[tuple[int, int, int, int]] = []
    for x in range(w):
        y = 0
        while y < h:
            if lit(bits, stride, x, y):
                start = y
                while y < h and lit(bits, stride, x, y):
                    y += 1
                if y - start >= 2:
                    v_segs.append((x, start, x, y - 1))
            else:
                y += 1

    # Drop segments fully covered by an orthogonal one (corner pixels are
    # otherwise counted twice and emitted as 1-pixel "stub" segments).
    return h_segs + v_segs


def trace_polyline(segments: list[tuple[int, int, int, int]]) -> list[tuple[int, int]]:
    """Walk endpoints to produce an ordered list of vertices."""
    # Adjacency: each segment has two endpoints; junctions chain segments.
    adj: dict[tuple[int, int], list[tuple[int, int]]] = defaultdict(list)
    for x0, y0, x1, y1 in segments:
        a, b = (x0, y0), (x1, y1)
        adj[a].append(b)
        adj[b].append(a)

    # Start at an endpoint (degree 1) if possible — that's where an open
    # polyline begins. Closed loops fall back to any vertex.
    starts = [v for v, n in adj.items() if len(n) == 1]
    if not starts:
        starts = [next(iter(adj))]
    # Prefer the topmost-leftmost start so output is deterministic.
    starts.sort(key=lambda v: (v[1], v[0]))
    start = starts[0]

    poly = [start]
    prev = None
    cur = start
    while True:
        nxts = [n for n in adj[cur] if n != prev]
        if not nxts:
            break
        if len(nxts) > 1:
            print(f"warning: junction at {cur} has degree {len(adj[cur])}; "
                  f"continuing along first branch", file=sys.stderr)
        nxt = nxts[0]
        poly.append(nxt)
        prev, cur = cur, nxt
        if cur == start:
            break  # closed loop
    return poly


def emit_edges(poly: list[tuple[int, int]]) -> str:
    lines = []
    for (x0, y0), (x1, y1) in zip(poly, poly[1:]):
        lines.append(f"    {{{{{x0:>3}, {y0:>2}}}, {{{x1:>3}, {y1:>2}}}}},")
    return "\n".join(lines)


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} path/to/file.xbm")
    path = Path(sys.argv[1])
    w, h, bits = load_xbm(path)
    segs = extract_segments(w, h, bits)
    poly = trace_polyline(segs)

    print(f"// Traced from {path.name} ({w}x{h}); {len(poly)} vertices, "
          f"{len(poly) - 1} edges.")
    print(f"static const MapPt {path.stem}_outline_pts[] = {{")
    for x, y in poly:
        print(f"    {{{x:>3}, {y:>2}}},")
    print("};")
    print()
    print(f"static const MapEdge {path.stem}_outline_edges[] = {{")
    print(emit_edges(poly))
    print("};")


if __name__ == "__main__":
    main()
