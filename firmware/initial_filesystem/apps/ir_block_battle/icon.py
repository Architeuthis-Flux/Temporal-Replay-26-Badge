"""IR Block Battle app icon.

The bytes match AppIcons::irBlockBattle in firmware/src/ui/AppIcons.h.
"""

# 12x12 packed XBM, two bytes per row (low = cols 0..7, high = cols
# 8..11). XBM is LSB-first; the binary literal reads mirrored.
WIDTH = 12
HEIGHT = 12
DATA = (
    0b00001111, 0b00000001,
    0b00001000, 0b00000011,
    0b00001000, 0b00000010,
    0b00000000, 0b00000100,
    0b00110011, 0b00000000,
    0b00110011, 0b00000111,
    0b00110000, 0b00000100,
    0b00111000, 0b00000000,
    0b00000000, 0b00000100,
    0b11001110, 0b00000001,
    0b11001110, 0b00000001,
    0b00000000, 0b00000000,
)
