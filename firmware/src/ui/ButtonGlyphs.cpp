#include "ButtonGlyphs.h"

#include <cstring>

#include "../infra/BadgeConfig.h"
#include "../hardware/oled.h"

namespace ButtonGlyphs {
namespace {

// Kevin's 23x23 button-cluster map, reduced to the footer-safe 10x10
// size. Letter interiors are removed; the requested button is filled.
constexpr uint8_t kYBits[] = {
    0x30, 0x00,
    0x78, 0x00,
    0x78, 0x00,
    0xB6, 0x01,
    0x49, 0x02,
    0x49, 0x02,
    0xB6, 0x01,
    0x48, 0x00,
    0x48, 0x00,
    0x30, 0x00,
};
constexpr uint8_t kBBits[] = {
    0x30, 0x00,
    0x48, 0x00,
    0x48, 0x00,
    0xB6, 0x01,
    0xC9, 0x03,
    0xC9, 0x03,
    0xB6, 0x01,
    0x48, 0x00,
    0x48, 0x00,
    0x30, 0x00,
};
constexpr uint8_t kABits[] = {
    0x30, 0x00,
    0x48, 0x00,
    0x48, 0x00,
    0xB6, 0x01,
    0x49, 0x02,
    0x49, 0x02,
    0xB6, 0x01,
    0x78, 0x00,
    0x78, 0x00,
    0x30, 0x00,
};
constexpr uint8_t kXBits[] = {
    0x30, 0x00,
    0x48, 0x00,
    0x48, 0x00,
    0xB6, 0x01,
    0x4F, 0x02,
    0x4F, 0x02,
    0xB6, 0x01,
    0x48, 0x00,
    0x48, 0x00,
    0x30, 0x00,
};
const uint8_t* bitsFor(Button button) {
  switch (button) {
    case Button::Y:
      return kYBits;
    case Button::B:
      return kBBits;
    case Button::A:
      return kABits;
    case Button::X:
      return kXBits;
    default:
      return kXBits;
  }
}

Button confirmButton() {
  return badgeConfig.get(kSwapConfirmCancel) != 0 ? Button::B : Button::A;
}

Button cancelButton() {
  return badgeConfig.get(kSwapConfirmCancel) != 0 ? Button::A : Button::B;
}

bool isSpace(char c) {
  return c == ' ' || c == '\t';
}

bool isBoundary(const char* text, size_t pos) {
  return pos == 0 || isSpace(text[pos - 1]);
}

bool tokenEnds(char c) {
  return c == '\0' || c == ':' || isSpace(c);
}

bool matchesWord(const char* text, const char* word, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char a = text[i];
    char b = word[i];
    if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 'a' + 'A');
    if (b >= 'a' && b <= 'z') b = static_cast<char>(b - 'a' + 'A');
    if (a != b) return false;
  }
  return true;
}

bool buttonToken(const char* text, size_t pos, Button* button,
                 size_t* consumed, bool* hasPair, Button* pairButton) {
  if (!text || !isBoundary(text, pos)) return false;
  const char* s = text + pos;
  if (matchesWord(s, "CONFIRM", 7) && tokenEnds(s[7])) {
    *button = confirmButton();
    *consumed = 7;
    *hasPair = false;
  } else if (matchesWord(s, "OK", 2) && tokenEnds(s[2])) {
    *button = confirmButton();
    *consumed = 2;
    *hasPair = false;
  } else if (matchesWord(s, "CANCEL", 6) && tokenEnds(s[6])) {
    *button = cancelButton();
    *consumed = 6;
    *hasPair = false;
  } else if (matchesWord(s, "BACK", 4) && tokenEnds(s[4])) {
    *button = cancelButton();
    *consumed = 4;
    *hasPair = false;
  } else if (matchesWord(s, "L/R", 3) && tokenEnds(s[3])) {
    *button = Button::X;
    *consumed = 3;
    *hasPair = true;
    *pairButton = Button::B;
  } else if (matchesWord(s, "X/B", 3) && tokenEnds(s[3])) {
    *button = Button::X;
    *consumed = 3;
    *hasPair = true;
    *pairButton = Button::B;
  } else if (matchesWord(s, "U/D", 3) && tokenEnds(s[3])) {
    *button = Button::Y;
    *consumed = 3;
    *hasPair = true;
    *pairButton = Button::A;
  } else if (matchesWord(s, "Y/A", 3) && tokenEnds(s[3])) {
    *button = Button::Y;
    *consumed = 3;
    *hasPair = true;
    *pairButton = Button::A;
  } else if (matchesWord(s, "^v", 2) && tokenEnds(s[2])) {
    *button = Button::Y;
    *consumed = 2;
    *hasPair = true;
    *pairButton = Button::A;
  } else if (matchesWord(s, "UP", 2) && tokenEnds(s[2])) {
    *button = Button::Y;
    *consumed = 2;
    *hasPair = false;
  } else if (matchesWord(s, "DOWN", 4) && tokenEnds(s[4])) {
    *button = Button::A;
    *consumed = 4;
    *hasPair = false;
  } else if ((s[0] == 'Y' || s[0] == 'y') && tokenEnds(s[1])) {
    *button = Button::Y;
    *consumed = 1;
    *hasPair = false;
  } else if ((s[0] == 'B' || s[0] == 'b') && tokenEnds(s[1])) {
    *button = Button::B;
    *consumed = 1;
    *hasPair = false;
  } else if ((s[0] == 'A' || s[0] == 'a') && tokenEnds(s[1])) {
    *button = Button::A;
    *consumed = 1;
    *hasPair = false;
  } else if ((s[0] == 'X' || s[0] == 'x') && tokenEnds(s[1])) {
    *button = Button::X;
    *consumed = 1;
    *hasPair = false;
  } else if ((s[0] == '^' || s[0] == 'U' || s[0] == 'u') && tokenEnds(s[1])) {
    *button = Button::Y;
    *consumed = 1;
    *hasPair = false;
  } else if ((s[0] == 'v' || s[0] == 'V' || s[0] == 'D' || s[0] == 'd') &&
             tokenEnds(s[1])) {
    *button = Button::A;
    *consumed = 1;
    *hasPair = false;
  } else if ((s[0] == '<' || s[0] == 'L' || s[0] == 'l') && tokenEnds(s[1])) {
    *button = Button::X;
    *consumed = 1;
    *hasPair = false;
  } else if ((s[0] == '>' || s[0] == 'R' || s[0] == 'r') && tokenEnds(s[1])) {
    *button = Button::B;
    *consumed = 1;
    *hasPair = false;
  } else {
    return false;
  }

  if (s[*consumed] == ':') {
    (*consumed)++;
  }
  return true;
}

int slashWidth(oled& d) {
  return d.getStrWidth("/");
}

int drawOrMeasureInline(oled& d, int x, int baseline, const char* text,
                        bool draw) {
  if (!text) return 0;
  int cursor = x;
  const size_t len = std::strlen(text);
  for (size_t i = 0; i < len;) {
    Button button = Button::X;
    size_t consumed = 0;
    bool hasPair = false;
    Button pairButton = Button::B;
    if (buttonToken(text, i, &button, &consumed, &hasPair, &pairButton)) {
      if (draw) {
        ButtonGlyphs::draw(d, button, cursor, baseline - kGlyphH + 1);
      }
      cursor += kGlyphW + kGap;
      if (hasPair) {
        if (draw) d.drawStr(cursor, baseline, "/");
        cursor += slashWidth(d) + kGap;
        if (draw) {
          ButtonGlyphs::draw(d, pairButton, cursor, baseline - kGlyphH + 1);
        }
        cursor += kGlyphW + kGap;
      }
      i += consumed;
      continue;
    }

    char one[2] = {text[i], '\0'};
    if (draw) d.drawStr(cursor, baseline, one);
    cursor += d.getStrWidth(one);
    i++;
  }
  return cursor - x;
}

}  // namespace

void draw(oled& d, Button button, int x, int y) {
  d.drawXBM(x, y, kGlyphW, kGlyphH, bitsFor(button));
}

// Footer chip labels are always painted uppercase regardless of how
// callers spell them — keeps the badge's nav chrome visually
// uniform across screens (no need to update every drawFooterActions
// callsite by hand). Stack buffer is local to each call so threading
// is fine.
namespace {
constexpr size_t kHintLabelCap = 24;
void toUpperBuf(const char* in, char* out) {
  size_t i = 0;
  if (in) {
    while (in[i] && i + 1 < kHintLabelCap) {
      const char c = in[i];
      out[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
      i++;
    }
  }
  out[i] = '\0';
}
}  // namespace

int measureHint(oled& d, Button button, const char* label) {
  (void)button;
  if (!label || !label[0]) return kGlyphW;
  char up[kHintLabelCap];
  toUpperBuf(label, up);
  return kGlyphW + kGap + d.getStrWidth(up);
}

int drawHint(oled& d, int x, int baseline, Button button, const char* label) {
  draw(d, button, x, baseline - kGlyphH + 1);
  int cursor = x + kGlyphW;
  if (label && label[0]) {
    char up[kHintLabelCap];
    toUpperBuf(label, up);
    cursor += kGap;
    d.drawStr(cursor, baseline, up);
    cursor += d.getStrWidth(up);
  }
  return cursor - x;
}

int measureInlineHint(oled& d, const char* text) {
  return drawOrMeasureInline(d, 0, 0, text, false);
}

int drawInlineHint(oled& d, int x, int baseline, const char* text) {
  return drawOrMeasureInline(d, x, baseline, text, true);
}

int drawInlineHintRight(oled& d, int rightX, int baseline, const char* text) {
  int w = measureInlineHint(d, text);
  drawInlineHint(d, rightX - w, baseline, text);
  return w;
}

int measureInlineHintCompact(oled& d, const char* text) {
  return measureInlineHint(d, text);
}

int drawInlineHintCompact(oled& d, int x, int baseline, const char* text) {
  return drawInlineHint(d, x, baseline, text);
}

}  // namespace ButtonGlyphs
