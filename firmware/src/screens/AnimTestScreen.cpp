#include "AnimTestScreen.h"

#include <cstdio>
#include <cstdlib>
#include <Arduino.h>

#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"
#include "../ui/Images.h"
#include "../hardware/Inputs.h"
#include "../hardware/oled.h"

void AnimTestScreen::onEnter(GUIManager& /*gui*/) {
  frameIdx_ = 0;
  scaleIdx_ = 0;
  frameDelayMs_ = 200;
  lastFrameMs_ = millis();
  lastJoyNavMs_ = 0;
  updateScaleOptions();
}

void AnimTestScreen::updateScaleOptions() {
  const ImageInfo& img = kImageCatalog[imageIdx_];
  scaleCount_ = ImageScaler::availableScales(img, scaleDims_, 8);
  if (scaleIdx_ >= scaleCount_) scaleIdx_ = 0;
  frameIdx_ = 0;
}

void AnimTestScreen::render(oled& d, GUIManager& /*gui*/) {
  const ImageInfo& img = kImageCatalog[imageIdx_];
  const uint8_t dispW = scaleDims_[scaleIdx_];
  const uint8_t dispH = dispW;  // all sources are square
  const bool use48 = img.data48 && (dispW == 48 || dispW == 24 || dispW == 12);
  const uint8_t srcDim = use48 ? 48 : 64;

  const uint8_t* frameData = ImageScaler::getFrame(img, frameIdx_, dispW);

  d.setDrawColor(1);

  // Image: bottom-right justified
  int imgX = 128 - dispW;
  int imgY = 64 - dispH;
  d.drawXBM(imgX, imgY, srcDim, srcDim, frameData, dispW, dispH);

  // Info panel: left 64px column
  d.setFontPreset(FONT_TINY);
  char buf[16];

  d.drawStr(0, 6, img.name);
  d.drawHLine(0, 8, 64);

  std::snprintf(buf, sizeof(buf), "%dx%d", dispW, dispH);
  d.drawStr(0, 16, buf);

  uint16_t curDelay = (img.frameTimes && frameIdx_ < img.frameCount)
                      ? img.frameTimes[frameIdx_] : frameDelayMs_;
  std::snprintf(buf, sizeof(buf), "F%d/%d %dms",
                frameIdx_ + 1, img.frameCount, curDelay);
  d.drawStr(0, 24, buf);

  std::snprintf(buf, sizeof(buf), "%d/%d", imageIdx_ + 1, kImageCatalogCount);
  d.drawStr(0, 32, buf);

  OLEDLayout::drawGameFooter(d);
  OLEDLayout::drawUpperFooterActions(d, "slow", "fast", nullptr, nullptr);
  OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", "next");

  // Advance frame
  uint32_t now = millis();
  uint16_t delay = (img.frameTimes && frameIdx_ < img.frameCount)
                   ? img.frameTimes[frameIdx_] : frameDelayMs_;
  if (img.frameCount > 1 && now - lastFrameMs_ >= delay) {
    frameIdx_ = (frameIdx_ + 1) % img.frameCount;
    lastFrameMs_ = now;
  }
}

void AnimTestScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                 int16_t /*cy*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();

  if (e.cancelPressed) {
    gui.popScreen();
    return;
  }

  if (e.confirmPressed) {
    imageIdx_ = (imageIdx_ + 1) % kImageCatalogCount;
    updateScaleOptions();
  }

  if (e.xPressed) {
    if (frameDelayMs_ > kMinDelay + kDelayStep)
      frameDelayMs_ -= kDelayStep;
    else
      frameDelayMs_ = kMinDelay;
  }

  if (e.yPressed) {
    if (frameDelayMs_ < kMaxDelay - kDelayStep)
      frameDelayMs_ += kDelayStep;
    else
      frameDelayMs_ = kMaxDelay;
  }

  uint32_t nowMs = millis();
  int16_t joyDeltaY = static_cast<int16_t>(inputs.joyY()) - 2047;
  if (abs(joyDeltaY) > static_cast<int16_t>(kJoyDeadband)) {
    uint32_t repeatMs = 300;
    if (lastJoyNavMs_ == 0 || nowMs - lastJoyNavMs_ >= repeatMs) {
      lastJoyNavMs_ = nowMs;
      if (joyDeltaY > 0) {
        scaleIdx_ = (scaleIdx_ + 1) % scaleCount_;
      } else {
        scaleIdx_ = (scaleIdx_ == 0) ? scaleCount_ - 1 : scaleIdx_ - 1;
      }
    }
  } else {
    lastJoyNavMs_ = 0;
  }
}
