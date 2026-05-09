"""BreakSnake app icon.

The bytes match AppIcons::breaksnake in firmware/src/ui/AppIcons.h.

Layout: 12x12 packed XBM. Two bytes per row — the low byte covers cols
0..7 and the high byte covers cols 8..11 (the top 4 bits of the high
byte are unused). XBM order is LSB-first within each byte, so the binary
literal reads mirrored relative to the rendered icon. The pattern is
still visible by eye; the bytes are right-aligned visually below.
"""

WIDTH = 12
HEIGHT = 12
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
