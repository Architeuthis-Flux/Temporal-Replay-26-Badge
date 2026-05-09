"""Flappy Asteroids app icon.

The bytes match AppIcons::flappyAsteroids in firmware/src/ui/AppIcons.h.
"""

# 12x12 packed XBM, two bytes per row (low = cols 0..7, high = cols
# 8..11). XBM is LSB-first; the binary literal reads mirrored relative
# to the rendered icon — that's accepted, the pattern is still visible.
WIDTH = 12
HEIGHT = 12
DATA = (
    0b00000000, 0b00001100,
    0b00001000, 0b00001100,
    0b00011100, 0b00001100,
    0b00101010, 0b00000000,
    0b10001000, 0b00000001,
    0b11000001, 0b00001101,
    0b10100010, 0b00001100,
    0b00000001, 0b00001100,
    0b00010100, 0b00001100,
    0b00100010, 0b00001100,
    0b00001000, 0b00001100,
    0b00010100, 0b00000000,
)
