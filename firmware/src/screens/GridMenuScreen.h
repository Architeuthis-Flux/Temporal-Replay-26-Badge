#pragma once

#include "Screen.h"
#include "JoyRamp.h"

using GridMenuAction = void (*)(GUIManager& gui);
using GridMenuVisible = bool (*)();
using GridMenuLabelFn = void (*)(char* buf, uint8_t bufSize);
using GridMenuBadgeFn = uint16_t (*)();

struct GridMenuItem {
  const char* label;
  const char* description;
  const uint8_t* icon;
  ScreenId target;
  GridMenuAction action;
  GridMenuVisible visible;
  GridMenuLabelFn labelFn;
  GridMenuBadgeFn badgeFn = nullptr;
  uint8_t iconW = 0;
  uint8_t iconH = 0;
  bool iconWhiteOnBlack = false;
};

class GridMenuScreen : public Screen {
 public:
  GridMenuScreen(ScreenId sid, const char* title,
                 const GridMenuItem* items, uint8_t count);

  // Swap the items pointer (e.g. after AppRegistry::rescan() rebuilt
  // the dynamic-app rows). Invalidates the visibility cache so the
  // next render walks the fresh array. Items must remain valid for
  // the lifetime of the screen — caller owns storage.
  void setItems(const GridMenuItem* items, uint8_t count);

  void onEnter(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX,
                   int16_t cursorY, GUIManager& gui) override;
  ScreenId id() const override { return sid_; }
  bool showCursor() const override { return false; }

 private:
  void rebuildVisibleCache() const;
  void ensureVisibleCache() const;
  const GridMenuItem* itemAtVisibleIndex(uint8_t index) const;
  uint8_t visibleCount() const;
  uint8_t rowCount() const;
  uint8_t maxTopRow() const;
  uint8_t targetTopRowForSelection() const;
  int16_t currentScrollPx() const;
  void syncScrollToSelection(bool animate);
  void finishScrollIfNeeded();
  void clampSelection();
  void moveSelection(int8_t dir);
  void selectCurrent(GUIManager& gui);
  void formatLabel(const GridMenuItem& item, char* buf, uint8_t bufSize) const;
  void drawHeader(oled& d) const;
  void drawCell(oled& d, uint8_t col, int y, uint8_t visibleIndex,
                bool selected) const;
  void drawFooter(oled& d);

  ScreenId sid_;
  const char* title_;
  const GridMenuItem* items_;
  uint8_t count_;
  uint8_t cursor_ = 0;
  uint8_t topRow_ = 0;
  bool scrollAnimating_ = false;
  uint32_t scrollAnimStartMs_ = 0;
  int16_t scrollFromPx_ = 0;
  int16_t scrollToPx_ = 0;
  JoyRamp joyRamp_;
  mutable char dynamicTitle_[32] = {};
  mutable uint8_t visibleIndices_[32] = {};
  mutable uint8_t visibleCount_ = 0;
  mutable bool visibleCacheValid_ = false;
  mutable uint16_t badgeCounts_[32] = {};
  mutable uint32_t badgePulseStartMs_[32] = {};
  mutable uint16_t badgeSeenFrame_[32] = {};
  mutable uint16_t badgeRenderFrame_ = 0;
  mutable bool badgeCountsValid_[32] = {};
};
