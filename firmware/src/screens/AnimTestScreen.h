#pragma once
#include "Screen.h"

// ─── Animation test screen (image catalog viewer with scaling) ──────────────

class AnimTestScreen : public Screen {
 public:
  void onEnter(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenAnimTest; }
  bool showCursor() const override { return false; }

 private:
  uint8_t imageIdx_ = 0;
  uint8_t frameIdx_ = 0;
  uint8_t scaleIdx_ = 0;
  uint16_t frameDelayMs_ = 200;
  uint32_t lastFrameMs_ = 0;
  uint32_t lastJoyNavMs_ = 0;

  static constexpr uint16_t kJoyDeadband = 400;
  static constexpr uint16_t kMinDelay = 30;
  static constexpr uint16_t kMaxDelay = 2000;
  static constexpr uint16_t kDelayStep = 30;

  uint8_t scaleDims_[4];
  uint8_t scaleCount_ = 0;

  void updateScaleOptions();
};
