#pragma once

#include <Arduino.h>

class oled;

namespace ButtonGlyphs {

enum class Button : uint8_t {
  Y = 0,
  B = 1,
  A = 2,
  X = 3,
  Up = Y,
  Right = B,
  Down = A,
  Left = X,
};

constexpr uint8_t kGlyphW = 10;
constexpr uint8_t kGlyphH = 10;
constexpr uint8_t kGap = 2;

void draw(oled& d, Button button, int x, int y);

// Composite cluster glyphs (all four / left+right / up+down) are
// surfaced via the inline-hint parser only — see ButtonGlyphs.cpp.
// The recognised tokens are `ALL`, `L/R` (alias `X/B`), and `U/D`
// (aliases `Y/A`, `^v`); each draws as a single 10×10 cluster picture
// with the relevant pads filled, instead of two side-by-side glyphs
// or an OR composition at draw time.
int drawHint(oled& d, int x, int baseline, Button button, const char* label);
int measureHint(oled& d, Button button, const char* label);

int drawInlineHint(oled& d, int x, int baseline, const char* text);
int drawInlineHintRight(oled& d, int rightX, int baseline, const char* text);
int measureInlineHint(oled& d, const char* text);
int drawInlineHintCompact(oled& d, int x, int baseline, const char* text);
int measureInlineHintCompact(oled& d, const char* text);

}  // namespace ButtonGlyphs
