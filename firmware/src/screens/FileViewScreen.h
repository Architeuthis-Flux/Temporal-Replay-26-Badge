#pragma once
#include "Screen.h"

// ─── File view screen (scrollable text viewer) ─────────────────────────────

class FileViewScreen : public Screen {
 public:
  void loadFile(const char* name);

  void onEnter(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenFileView; }
  bool showCursor() const override { return false; }

 private:
  static constexpr uint16_t kViewBufSize = 600;
  static constexpr uint8_t kVisibleRows = 5;
  static constexpr uint8_t kMaxNameLen = 28;
  static constexpr uint16_t kJoyDeadband = 400;

  char viewBuf_[kViewBufSize];
  uint16_t viewLen_ = 0;
  uint8_t viewTotalLines_ = 0;
  char viewTitle_[kMaxNameLen];
  uint8_t scrollLine_ = 0;
  uint32_t lastJoyNavMs_ = 0;
};
