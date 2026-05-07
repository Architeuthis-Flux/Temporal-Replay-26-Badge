#include "SettingsScreen.h"

#include <cstdio>
#include <cstdlib>

#include "../hardware/Inputs.h"
#include "../hardware/LEDmatrix.h"
#include "../hardware/oled.h"
#include "../infra/BadgeConfig.h"
#include "../led/LEDAppRuntime.h"
#include "../ui/ButtonGlyphs.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"
#include "../ui/UIFonts.h"

extern LEDmatrix badgeMatrix;

namespace {

constexpr uint8_t kRowHeight = 11;
constexpr int     kValueRightX = 119;  // pulled in 5 px so right-aligned values clear the scrollbar (122..127)

// ── Compile-time scroll cadence (matches GridMenuScreen / home menu) ─────
constexpr uint32_t kSettingsRampStartMs = 350;
constexpr uint32_t kSettingsRampMinMs   = 100;
constexpr uint32_t kSettingsRampStepMs  = 50;

// ── Chevron bitmap (downward triangle) ──────────────────────────────────
// Right-arrow geometry rotated 90° clockwise about the X axis so the
// dropdown indicator points down regardless of expansion state. 5 wide ×
// 3 tall.
constexpr uint8_t kChevronW = 5;
constexpr uint8_t kChevronH = 5;
// Down-arrow chevron — drawn when the group is expanded (active).
static const uint8_t kChevronDownBits[] PROGMEM = {
    0x00, 0x1F, 0x0E, 0x04, 0x00,
};
// Right-arrow chevron — drawn when the group is collapsed.
static const uint8_t kChevronRightBits[] PROGMEM = {
    0x01, 0x03, 0x07, 0x03, 0x01,
};
constexpr int kSettingIndent = 10;  // dropdown items indent under the chevron (+5 deeper than before)

// Tiny adjustability arrows shown on the right side of each setting
// row, flanking the value. Right-pointing 3×5 arrow + its left-
// pointing mirror (mirror generated at draw time so we ship one
// bitmap). Indicates the value is joystick-L/R adjustable.
constexpr uint8_t kSettingArrowW = 3;
constexpr uint8_t kSettingArrowH = 5;
static const uint8_t kSettingArrowRightBits[] PROGMEM = {
    0x01, 0x03, 0x07, 0x03, 0x01,
};
static const uint8_t kSettingArrowLeftBits[] PROGMEM = {
    0x04, 0x06, 0x07, 0x06, 0x04,
};

// ── Setting groups ───────────────────────────────────────────────────────
//
// User-facing categories surface only the values most users would want
// to tweak. Every other ("nuanced") setting goes under the Admin
// dropdown, which is compiled in only when BADGE_DEV_MENU is defined —
// the underlying values still load and apply in production builds, the
// dropdown just isn't reachable from the UI.

static const SettingIndex kDisplayItems[] = {
    kOledContrast,
};
static const SettingIndex kHapticItems[] = {
    kHapticEnabled, kHapticStrength, kHapticFreqHz, kHapticPulseMs,
};
static const SettingIndex kLedItems[] = {
    kLedBrightness,
};
static const SettingIndex kInputItems[] = {
    kSwapConfirmCancel,
};
static const SettingIndex kSleepItems[] = {
    kLightSleepSec, kDeepSleepSec,
};

#ifdef BADGE_DEV_MENU
static const SettingIndex kAdminItems[] = {
    // Display tuning
    kOledOsc, kOledDiv, kOledMux,
    kOledPrecharge1, kOledPrecharge2, kOledRefreshMs,
    // LED tuning
    kLedServiceMs,
    // Joystick raw
    kJoySensitivity, kJoyDeadzone, kJoyPollMs,
    // Button repeat
    kBtnDebounceMs, kRptInitialDelayMs, kRptFirstIntervalMs,
    kRptSecondDelayMs, kRptSecondIntervalMs,
    // Flip / IMU
    kAutoFlipEnable, kFlipUpThreshold, kFlipDownThreshold, kFlipDelayMs,
    kImuSmoothing, kImuInt1Threshold, kImuInt1Duration,
    // Scheduler / CPU / network
    kSchHighDiv, kSchNormDiv, kSchLowDiv, kLoopDelayMs,
    kCpuIdleMhz, kCpuActiveMhz, kWifiCheckMs,
};
#endif

struct SettingGroup {
  const char* label;
  const SettingIndex* items;
  uint8_t itemCount;
};

#define GROUP(NAME, ARR) \
  {NAME, ARR, sizeof(ARR) / sizeof(ARR[0])}

static const SettingGroup kGroups[] = {
    GROUP("Display", kDisplayItems),
    GROUP("Haptics", kHapticItems),
    GROUP("LED",     kLedItems),
    GROUP("Input",   kInputItems),
    GROUP("Sleep",   kSleepItems),
#ifdef BADGE_DEV_MENU
    GROUP("Admin",   kAdminItems),
#endif
};
constexpr uint8_t kGroupCount = sizeof(kGroups) / sizeof(kGroups[0]);

// ── One-line description per setting, displayed in the footer when the
//    item is the current cursor row. Keep these short — the footer
//    renders in u8g2_font_4x6_tf and clips around 124 px (~30 chars).
struct SettingDesc {
  uint8_t idx;
  const char* desc;
};

static const SettingDesc kSettingDescs[] = {
    // Display
    {kFontFamily,         "UI typeface family"},
    {kFontSize,           "UI text size preset"},
    {kOledContrast,       "OLED panel brightness"},
    // Haptics
    {kHapticEnabled,      "Vibrate on button presses"},
    {kHapticStrength,     "Vibration intensity 0-255"},
    {kHapticFreqHz,       "Motor drive frequency (Hz)"},
    {kHapticPulseMs,      "Default pulse length (ms)"},
    // LED
    {kLedBrightness,      "LED matrix global brightness"},
    {kLedServiceMs,       "LED service tick interval"},
    // Input
    {kSwapConfirmCancel,  "Swap confirm/cancel buttons"},
    {kJoySensitivity,     "Joystick deflection scaling"},
    {kJoyDeadzone,        "Joystick neutral threshold"},
    {kJoyPollMs,          "Joystick poll interval"},
    {kBtnDebounceMs,      "Button debounce window"},
    {kRptInitialDelayMs,  "Hold ms before auto-repeat"},
    {kRptFirstIntervalMs, "Initial auto-repeat rate"},
    {kRptSecondDelayMs,   "Hold ms before fast repeat"},
    {kRptSecondIntervalMs,"Fast auto-repeat rate"},
    // Sleep
    {kLightSleepSec,      "Idle s before light sleep"},
    {kDeepSleepSec,       "Idle s before deep sleep"},
    // Display tuning
    {kOledOsc,            "OLED oscillator divider"},
    {kOledDiv,            "OLED display clock divider"},
    {kOledMux,            "OLED multiplex ratio"},
    {kOledPrecharge1,     "OLED precharge phase 1"},
    {kOledPrecharge2,     "OLED precharge phase 2"},
    {kOledRefreshMs,      "OLED refresh interval (ms)"},
    // Flip / IMU
    {kAutoFlipEnable,     "Auto-detect orientation"},
    {kFlipUpThreshold,    "IMU upright threshold"},
    {kFlipDownThreshold,  "IMU inverted threshold"},
    {kFlipDelayMs,        "Tilt-confirm delay (ms)"},
    {kImuSmoothing,       "IMU sample smoothing"},
    {kImuInt1Threshold,   "IMU INT1 motion threshold"},
    {kImuInt1Duration,    "IMU INT1 motion duration"},
    // Power / scheduler
    {kSchHighDiv,         "High-prio scheduler divider"},
    {kSchNormDiv,         "Normal scheduler divider"},
    {kSchLowDiv,          "Low-prio scheduler divider"},
    {kLoopDelayMs,        "Main loop sleep (ms)"},
    {kCpuIdleMhz,         "CPU MHz when idle"},
    {kCpuActiveMhz,       "CPU MHz when active"},
    {kWifiCheckMs,        "WiFi check interval (ms)"},
};

const char* descFor(uint8_t idx) {
  for (const auto& sd : kSettingDescs) {
    if (sd.idx == idx) return sd.desc;
  }
  return nullptr;
}

}  // namespace

static const uint8_t kUserSettings[] = {
    kLedBrightness,
    kOledContrast,
    kHapticEnabled,
    kHapticStrength,
    kSwapConfirmCancel,
    kAutoFlipEnable,
    kJoySensitivity,
    kJoyDeadzone,
    kRptInitialDelayMs,
    kRptFirstIntervalMs,
    kBtnDebounceMs,
    kLightSleepSec,
    kDeepSleepSec,
};
static constexpr uint8_t kUserSettingCount =
    sizeof(kUserSettings) / sizeof(kUserSettings[0]);

static uint8_t configIndex(uint8_t row) {
  return (row < kUserSettingCount) ? kUserSettings[row] : 0;
}

static const char* settingLabel(uint8_t ci) {
  switch (ci) {
    case kLedBrightness:     return "LED";
    case kOledContrast:      return "Contrast";
    case kHapticEnabled:     return "Haptics";
    case kHapticStrength:    return "Strength";
    case kSwapConfirmCancel: return "Confirm";
    case kAutoFlipEnable:    return "Auto Flip";
    case kJoySensitivity:    return "Joy Sens";
    case kJoyDeadzone:       return "Joy Dead";
    case kRptInitialDelayMs: return "Rpt Delay";
    case kRptFirstIntervalMs:return "Rpt Speed";
    case kBtnDebounceMs:     return "Debounce";
    case kLightSleepSec:     return "Light Slp";
    case kDeepSleepSec:      return "Deep Slp";
    default:                 return Config::kDefs[ci].label;
  }
}

static void settingValue(uint8_t ci, const Config* cfg, char* out, uint8_t cap) {
  const int32_t v = cfg->get(ci);
  switch (ci) {
    case kHapticEnabled:
    case kAutoFlipEnable:
      std::snprintf(out, cap, "%s", v ? "On" : "Off");
      break;
    case kLightSleepSec:
    case kDeepSleepSec:
      std::snprintf(out, cap, "%lds", (long)v);
      break;
    case kRptInitialDelayMs:
    case kRptFirstIntervalMs:
    case kBtnDebounceMs:
      std::snprintf(out, cap, "%ldms", (long)v);
      break;
    case kJoySensitivity:
    case kJoyDeadzone:
      std::snprintf(out, cap, "%ld%%", (long)v);
      break;
    default:
      std::snprintf(out, cap, "%ld", (long)v);
      break;
  }
}

SettingsScreen::SettingsScreen(ScreenId sid, Config* config)
    : ListMenuScreen(sid, "SETTINGS"), config_(config) {}

uint8_t SettingsScreen::itemCount() const {
  uint8_t total = kGroupCount;
  if (activeGroup_ >= 0 && activeGroup_ < (int8_t)kGroupCount) {
    total += kGroups[activeGroup_].itemCount;
  }
  return total;
}

SettingsScreen::RowRef SettingsScreen::resolveRow(uint8_t cursor) const {
  uint8_t pos = 0;
  for (int8_t g = 0; g < (int8_t)kGroupCount; ++g) {
    if (cursor == pos) return {g, -1};   // group header
    pos++;
    if (g == activeGroup_) {
      const uint8_t k = kGroups[g].itemCount;
      if (cursor < pos + k) {
        return {g, static_cast<int8_t>(kGroups[g].items[cursor - pos])};
      }
      pos += k;
    }
  }
  return {-1, -1};   // out of range (e.g. synthetic back row)
}

void SettingsScreen::formatItem(uint8_t index, char* buf,
                                uint8_t bufSize) const {
  // Used only by the base render's "Back" fallback path.
  (void)index;
  std::snprintf(buf, bufSize, "%s", "");
}

void SettingsScreen::formatSettingValue(uint8_t settingIdx,
                                        char* label, uint8_t labelSize,
                                        char* value, uint8_t valueSize) const {
  if (settingIdx == kHapticEnabled) {
    std::snprintf(label, labelSize, "Haptics");
    std::snprintf(value, valueSize, "%s",
                  config_->get(settingIdx) ? "On" : "Off");
  } else if (settingIdx == kSwapConfirmCancel) {
    std::snprintf(label, labelSize, "Confirm");
    std::snprintf(value, valueSize, "%s",
                  config_->get(settingIdx) ? "B" : "A");
  } else {
    std::snprintf(label, labelSize, "%s", Config::kDefs[settingIdx].label);
    std::snprintf(value, valueSize, "%ld", (long)config_->get(settingIdx));
  }
}

uint8_t SettingsScreen::computeRowHeight(oled& d) const {
  (void)d;
  return kRowHeight;
}

void SettingsScreen::drawItem(oled& d, uint8_t index, uint8_t y,
                              uint8_t rowHeight, bool selected) const {
  const RowRef row = resolveRow(index);
  if (row.groupIdx < 0) return;

  const bool isHeader  = (row.settingIdx < 0);
  const bool isEditing = (selected && editing_ && !isHeader);

  // Erase whatever the base ListMenuScreen painted, then draw the
  // rounded selection outline. Width is pulled all the way to the
  // scrollbar (4 px wide at x=124..127) — leaves a single 1-px gap
  // between outline and bar so they don't visually merge but doesn't
  // leave a fat white strip the way a smaller box did.
  constexpr int kSelW = 123;            // 0..122 inclusive — outline ends at x=121, scrollbar starts at x=124
  if (selected) {
    d.setDrawColor(0);
    d.drawBox(0, y, kSelW, rowHeight);
    d.setDrawColor(1);
    if (!isEditing) {
      d.drawRFrame(1, y, kSelW - 2, rowHeight - 1, 2);
    }
  }

  // +1 px text size — bumps from 4×6 to 5×7 so the values are more
  // readable than the body lists elsewhere.
  d.setFont(u8g2_font_5x7_tf);
  const int baseline = y + d.getAscent() + 2;

  if (isHeader) {
    // Chevron points right when the group is collapsed, down when
    // expanded — same affordance as a disclosure triangle.
    const bool open = (activeGroup_ == row.groupIdx);
    const uint8_t* bits = open ? kChevronDownBits : kChevronRightBits;
    const int cx = 5;
    // The down-pointing chevron's bottom row visually anchors against
    // the row baseline; the right-pointing chevron has its visual
    // mass centered slightly lower than the down version once they're
    // both centered geometrically, so nudge it up 1 px to match.
    const int cy_base = y + (rowHeight - kChevronH) / 2;
    const int cy = open ? cy_base : cy_base - 1;
    d.setDrawColor(1);
    d.drawXBM(cx, cy, kChevronW, kChevronH, bits);
    d.drawStr(cx + kChevronW + 4, baseline, kGroups[row.groupIdx].label);
    return;
  }

  // ── Setting row ─────────────────────────────────────────────────────────
  char label[24] = {};
  char value[24] = {};
  formatSettingValue(static_cast<uint8_t>(row.settingIdx),
                     label, sizeof(label), value, sizeof(value));

  // Settings items inside an expanded dropdown sit indented under the
  // group header's chevron, visually nested (like a tree view).
  const int labelX = 6 + kSettingIndent;

  if (row.settingIdx == kSwapConfirmCancel) {
    const bool swapped = config_ && config_->get(kSwapConfirmCancel) != 0;
    const ButtonGlyphs::Button confirm = swapped ? ButtonGlyphs::Button::B
                                                  : ButtonGlyphs::Button::A;
    const ButtonGlyphs::Button back    = swapped ? ButtonGlyphs::Button::A
                                                  : ButtonGlyphs::Button::B;
    d.drawStr(labelX, baseline, label);
    const int confirmW = ButtonGlyphs::measureHint(d, confirm, "ok");
    const int backW = ButtonGlyphs::measureHint(d, back, "back");
    const int clusterW = confirmW + 4 + backW;
    int x = kValueRightX - clusterW;
    if (isEditing) {
      d.setDrawColor(1);
      d.drawRBox(x - 2, y + 1, clusterW + 4, rowHeight - 2, 2);
      d.setDrawColor(0);
    }
    x += ButtonGlyphs::drawHint(d, x, baseline, confirm, "ok") + 4;
    ButtonGlyphs::drawHint(d, x, baseline, back, "back");
    d.setDrawColor(1);
    return;
  }

  d.drawStr(labelX, baseline, label);

  // Single ▶ adjust arrow on the far-right inside the selector
  // outline. Selector outline's right edge is at x = kSelW - 2 = 121
  // (for kSelW=123); arrow sits 2 px inside that. Value text gets
  // pushed left to make room for the arrow when the row is selected.
  constexpr int kArrowInsetFromFrame = 2;
  constexpr int kArrowGap            = 2;          // px between arrow and value text
  const int arrowRightX = (kSelW - 2) - kArrowInsetFromFrame;  // inclusive
  const int arrowX      = arrowRightX - kSettingArrowW + 1;
  const int arrowY      = y + (rowHeight - kSettingArrowH) / 2;

  const int valueW = d.getStrWidth(value);
  const int valueX = selected
                         ? (arrowX - kArrowGap - valueW)
                         : (kValueRightX - valueW);

  if (isEditing) {
    // Filled rounded chip behind the value while editing.
    d.setDrawColor(1);
    d.drawRBox(valueX - 2, y + 1, valueW + 4, rowHeight - 2, 2);
    d.setDrawColor(0);
    d.drawStr(valueX, baseline, value);
    d.setDrawColor(1);
  } else {
    d.drawStr(valueX, baseline, value);
  }

  if (selected) {
    d.setDrawColor(1);
    d.drawXBM(arrowX, arrowY, kSettingArrowW, kSettingArrowH,
              kSettingArrowRightBits);
  }
}

void SettingsScreen::onItemAdjust(uint8_t index, int8_t dir,
                                  GUIManager& /*gui*/) {
  const RowRef row = resolveRow(index);
  if (row.settingIdx < 0) return;
  const uint8_t s = static_cast<uint8_t>(row.settingIdx);
  int32_t v = Config::nextValue(s, config_->get(s), dir);
  config_->set(s, v);
  config_->apply(s);
}

void SettingsScreen::enterEditMode() {
  const RowRef row = resolveRow(cursor_);
  if (row.settingIdx < 0) return;
  editing_    = true;
  editingLed_ = (row.settingIdx == kLedBrightness);
  joyAdjustRamp_.reset();
  if (editingLed_) {
    ledAppRuntime.beginOverride();
    badgeMatrix.fill(static_cast<uint8_t>(config_->get(kLedBrightness)));
  }
}

void SettingsScreen::exitEditMode() {
  if (editingLed_) {
    ledAppRuntime.endOverride();
  }
  editing_    = false;
  editingLed_ = false;
  joyAdjustRamp_.reset();
  if (config_) config_->save();
}

void SettingsScreen::cycleValue(int8_t dir, GUIManager& gui) {
  onItemAdjust(cursor_, dir, gui);
  if (editingLed_) {
    badgeMatrix.fill(static_cast<uint8_t>(config_->get(kLedBrightness)));
  }
}

void SettingsScreen::maybeAutoCollapse() {
  if (activeGroup_ < 0) return;
  const RowRef row = resolveRow(cursor_);
  if (row.groupIdx == activeGroup_) return;

  const int8_t  prevGroup    = activeGroup_;
  const uint8_t childCount   = kGroups[prevGroup].itemCount;
  const uint8_t groupHeader  = static_cast<uint8_t>(prevGroup);
  const uint8_t groupEndPos  = groupHeader + 1 + childCount;
  activeGroup_ = -1;

  if (cursor_ >= groupEndPos) {
    cursor_ -= childCount;
  }
  if (scroll_ >= groupEndPos) {
    scroll_ -= childCount;
  } else if (scroll_ > groupHeader) {
    scroll_ = groupHeader;
  }
  const uint8_t total = itemCount();
  if (total > 0 && cursor_ >= total) cursor_ = total - 1;
  if (scroll_ > cursor_) scroll_ = cursor_;
}

void SettingsScreen::onExit(GUIManager& gui) {
  if (editing_) exitEditMode();
  (void)gui;
  activeGroup_ = -1;
  joyRamp_.reset();
  joyAdjustRamp_.reset();
}

const char* SettingsScreen::hintText() const {
  // Surface the description of the focused setting in the footer hint
  // bar. Group headers and the synthetic back row fall through to the
  // base "back/set" footer actions.
  const RowRef row = resolveRow(cursor_);
  if (row.groupIdx < 0 || row.settingIdx < 0) return nullptr;
  return descFor(static_cast<uint8_t>(row.settingIdx));
}

void SettingsScreen::render(oled& d, GUIManager& gui) {
  onUpdate(gui);

  d.setTextWrap(false);
  d.setDrawColor(1);
  OLEDLayout::drawHeader(d, titleText());
  d.setFont(UIFonts::kText);

  const uint8_t rowHeight = computeRowHeight(d);
  const uint8_t visibleRows = computeVisibleRows(d);
  const uint8_t realCount = itemCount();
  // No synthetic "Back" row — settings is exited via the cancel
  // (B) button, surfaced in the footer hint. Keeping the back row
  // wasted a slot and split the cursor's logical end.
  const uint8_t totalCount = realCount;

  if (cursor_ >= totalCount) cursor_ = totalCount - 1;
  if (scroll_ > cursor_) scroll_ = cursor_;

  // Scrollbar geometry mirrors the base ListMenuScreen — vertical
  // rounded-rect track + filled rounded-rect thumb on the right edge.
  constexpr uint8_t kScrollbarW          = 4;
  constexpr uint8_t kScrollbarRightX     = 127;
  constexpr uint8_t kScrollbarLeftX      = kScrollbarRightX - kScrollbarW + 1;
  constexpr uint8_t kRowGapBeforeBar     = 1;
  constexpr uint8_t kScrollbarFooterPad  = 2;
  const uint8_t selRowW = kScrollbarLeftX - kRowGapBeforeBar;

  for (uint8_t i = 0; i < visibleRows && (scroll_ + i) < totalCount; i++) {
    const uint8_t idx = scroll_ + i;
    const uint8_t y = kContentY + i * rowHeight;
    const bool selected = (idx == cursor_);

    if (selected) {
      OLEDLayout::drawSelectedRow(d, y, rowHeight, /*x=*/0, /*w=*/selRowW);
    }
    // Invert text color on the highlighted row so the default
    // drawItem doesn't paint white on the white highlight box.
    d.setDrawColor(selected ? 0 : 1);

    if (idx < realCount) {
      drawItem(d, idx, y, rowHeight, selected);
    } else {
      d.drawStr(3, y + d.getAscent() + 1, "Back");
    }
    d.setDrawColor(1);
  }

  // Scrollbar track + thumb. Track height clamped so the bottom
  // edge sits kScrollbarFooterPad px above the footer.
  {
    const int rawTrackH = visibleRows * rowHeight;
    const int maxTrackH = OLEDLayout::kFooterTopY - kContentY - kScrollbarFooterPad;
    const int trackH    = (rawTrackH > maxTrackH ? maxTrackH : rawTrackH);
    if (trackH > 4 && totalCount > 0) {
      d.setDrawColor(1);
      d.drawRFrame(kScrollbarLeftX, kContentY, kScrollbarW, trackH, 1);

      const int thumbAvailH = trackH - 4;  // 2-px inset top + bottom for a shorter thumb
      int thumbH = (thumbAvailH * visibleRows + totalCount - 1) / totalCount;
      if (thumbH < 3)            thumbH = 3;
      if (thumbH > thumbAvailH)  thumbH = thumbAvailH;
      const int maxScrollTop = thumbAvailH - thumbH;
      const int scrollSpan   = (totalCount > visibleRows)
                                   ? (totalCount - visibleRows)
                                   : 1;
      int thumbY = kContentY + 2 + (maxScrollTop * scroll_) / scrollSpan;
      const int thumbYMax = kContentY + 2 + maxScrollTop;
      if (thumbY > thumbYMax) thumbY = thumbYMax;
      d.drawRBox(kScrollbarLeftX + 1, thumbY,
                 kScrollbarW - 2, thumbH, 1);
    }
  }

  const RowRef row = resolveRow(cursor_);
  const char* footer = nullptr;
  const char* action = "back";
  if (cursor_ >= realCount) {
    footer = "Back";
  } else if (editing_ && row.settingIdx >= 0) {
    footer = "Joystick left/right changes value";
    action = "done";
  } else if (row.settingIdx >= 0) {
    footer = descFor(static_cast<uint8_t>(row.settingIdx));
    action = "edit";
  } else if (row.groupIdx >= 0) {
    footer = (activeGroup_ == row.groupIdx) ? "Close group"
                                            : "Open group";
    action = (activeGroup_ == row.groupIdx) ? "close" : "open";
  }
  OLEDLayout::drawActionFooter(d, footer ? footer : "Back", action);
}

void SettingsScreen::handleInput(const Inputs& inputs,
                                 int16_t /*cx*/, int16_t /*cy*/,
                                 GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();
  const int16_t joyDX = static_cast<int16_t>(inputs.joyX()) - 2047;
  const int16_t joyDY = static_cast<int16_t>(inputs.joyY()) - 2047;

  if (editing_) {
    if (e.cancelPressed || e.confirmPressed) {
      exitEditMode();
      return;
    }
    const int8_t dir = (std::abs(joyDX) > static_cast<int16_t>(kJoyDeadband))
                           ? (joyDX > 0 ? 1 : -1)
                           : 0;
    if (joyAdjustRamp_.tick(dir, millis(),
                            kSettingsRampStartMs,
                            kSettingsRampMinMs,
                            kSettingsRampStepMs)) {
      cycleValue(dir, gui);
      return;
    }
    return;
  }

  if (e.cancelPressed) {
    gui.popScreen();
    return;
  }

  // Down button toggles dropdown for current group header.
  if (e.downPressed) {
    const RowRef row = resolveRow(cursor_);
    if (row.groupIdx >= 0 && row.settingIdx < 0) {
      activeGroup_ = (activeGroup_ == row.groupIdx) ? -1 : row.groupIdx;
      return;
    }
  }

  if (e.confirmPressed) {
    const RowRef row = resolveRow(cursor_);
    if (row.groupIdx < 0) {
      gui.popScreen();
      return;
    }
    if (row.settingIdx < 0) {
      activeGroup_ = (activeGroup_ == row.groupIdx) ? -1 : row.groupIdx;
      return;
    }
    enterEditMode();
    return;
  }

  // Joystick Y scrolls cursor with home-menu's ramped cadence.
  const int8_t dir = (std::abs(joyDY) > static_cast<int16_t>(kJoyDeadband))
                         ? (joyDY > 0 ? 1 : -1)
                         : 0;
  if (joyRamp_.tick(dir, millis(),
                    kSettingsRampStartMs,
                    kSettingsRampMinMs,
                    kSettingsRampStepMs)) {
    const uint8_t total = itemCount();
    int16_t next = static_cast<int16_t>(cursor_) + dir;
    if (next < 0) next = 0;
    if (total > 0 && next >= total) next = total - 1;
    cursor_ = static_cast<uint8_t>(next);
    maybeAutoCollapse();

    oled& d = gui.oledDisplay();
    d.setFont(u8g2_font_5x7_tf);
    const uint8_t visibleRows = computeVisibleRows(d);
    const uint8_t total2 = itemCount();
    if (total2 > 0 && cursor_ >= total2) cursor_ = total2 - 1;
    if (cursor_ == 0)               scroll_ = 0;
    else if (cursor_ < scroll_ + 1) scroll_ = cursor_ - 1;
    if (cursor_ > scroll_ + visibleRows - 2)
      scroll_ = cursor_ - visibleRows + 2;
    const uint8_t maxScroll =
        total2 > visibleRows ? total2 - visibleRows + 1 : 0;
    if (scroll_ > maxScroll) scroll_ = maxScroll;
  }
}
