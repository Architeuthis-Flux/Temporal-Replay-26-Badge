#include "FileViewScreen.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../infra/Filesystem.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"
#include "../hardware/Inputs.h"
#include "../hardware/oled.h"

extern "C" {
#include "lib/oofatfs/ff.h"
FATFS* replay_get_fatfs(void);
}

void FileViewScreen::loadFile(const char* name) {
  strncpy(viewTitle_, name, kMaxNameLen - 1);
  viewTitle_[kMaxNameLen - 1] = '\0';
  viewLen_ = 0;
  viewTotalLines_ = 1;
  memset(viewBuf_, 0, kViewBufSize);

  Filesystem::IOLock fsLock;  // serialise vs other journal writers
  FATFS* fs = replay_get_fatfs();
  if (fs == nullptr) return;

  FIL fil;
  if (f_open(fs, &fil, name, FA_READ) == FR_OK) {
    UINT bytesRead = 0;
    f_read(&fil, viewBuf_, kViewBufSize - 1, &bytesRead);
    viewLen_ = static_cast<uint16_t>(bytesRead);
    viewBuf_[viewLen_] = '\0';
    f_close(&fil);

    viewTotalLines_ = 1;
    for (uint16_t i = 0; i < viewLen_; i++) {
      if (viewBuf_[i] == '\n') viewTotalLines_++;
    }
  }
}

void FileViewScreen::onEnter(GUIManager& /*gui*/) {
  scrollLine_ = 0;
  lastJoyNavMs_ = 0;
}

void FileViewScreen::render(oled& d, GUIManager& /*gui*/) {
  d.setFontPreset(FONT_TINY);
  d.setTextWrap(false);
  d.setDrawColor(1);

  d.setCursor(0, 0);
  d.print(viewTitle_);
  OLEDLayout::drawHeaderRule(d, 9);

  const char* p = viewBuf_;
  uint8_t lineNum = 0;
  while (lineNum < scrollLine_ && p < viewBuf_ + viewLen_) {
    if (*p == '\n') lineNum++;
    p++;
  }

  for (uint8_t row = 0; row < kVisibleRows && p < viewBuf_ + viewLen_;
       row++) {
    d.setCursor(0, 10 + row * 8);
    uint8_t col = 0;
    while (p < viewBuf_ + viewLen_ && *p != '\n' && col < 21) {
      d.print(*p);
      p++;
      col++;
    }
    while (p < viewBuf_ + viewLen_ && *p != '\n') p++;
    if (p < viewBuf_ + viewLen_ && *p == '\n') p++;
  }

  OLEDLayout::drawGameFooter(d);
  OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", nullptr);
  char page[12];
  std::snprintf(page, sizeof(page), "%u/%u",
                (unsigned)(scrollLine_ + 1), (unsigned)viewTotalLines_);
  const int pageW = d.getStrWidth(page);
  d.drawStr(128 - pageW, OLEDLayout::kFooterBaseY, page);
}

void FileViewScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                 int16_t /*cy*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();

  const uint8_t maxScroll =
      viewTotalLines_ > kVisibleRows ? viewTotalLines_ - kVisibleRows : 0;

  if (e.cancelPressed) {
    gui.popScreen();
    return;
  }

  auto scroll = [&](int8_t delta) {
    int16_t next = static_cast<int16_t>(scrollLine_) + delta;
    if (next < 0) next = 0;
    if (next > maxScroll) next = maxScroll;
    scrollLine_ = static_cast<uint8_t>(next);
  };

  uint32_t nowMs = millis();
  int16_t joyDeltaY = static_cast<int16_t>(inputs.joyY()) - 2047;
  if (abs(joyDeltaY) > static_cast<int16_t>(kJoyDeadband)) {
    int16_t absY = abs(joyDeltaY);
    uint32_t repeatMs = absY > 1500 ? 260 : (absY > 900 ? 140 : 380);
    if (lastJoyNavMs_ == 0 || nowMs - lastJoyNavMs_ >= repeatMs) {
      lastJoyNavMs_ = nowMs;
      scroll(joyDeltaY > 0 ? 1 : -1);
    }
  } else {
    lastJoyNavMs_ = 0;
  }
}
