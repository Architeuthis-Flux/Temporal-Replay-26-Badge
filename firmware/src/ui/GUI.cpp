#include "GUI.h"
#include "screens/AboutSponsorsScreen.h"
#include "screens/AnimTestScreen.h"
#include "screens/AppsScreen.h"
#include "screens/BadgeInfoViewScreen.h"
#include "screens/BootScreen.h"
#include "screens/BoopScreen.h"
#include "screens/ContactDetailScreen.h"
#include "screens/ContactsScreen.h"
#include "screens/DiagnosticsScreen.h"
#include "screens/FileViewScreen.h"
#include "screens/FilesScreen.h"
#include "screens/GridMenuScreen.h"
#include "screens/HapticsTestScreen.h"
#include "screens/InputTestScreen.h"
#include "screens/LEDScreen.h"
#include "screens/MapScreens.h"
#include "screens/ScheduleScreen.h"
#include "screens/SettingsScreen.h"
#include "screens/TextInputScreen.h"
#include "screens/draw/DrawIcons.h"
#include "screens/draw/DrawPickerScreen.h"
#include "screens/draw/DrawScreen.h"
#include "screens/draw/ScalePickerScreen.h"
#include "screens/draw/StickerPickerScreen.h"
#include "screens/draw/AnimDoc.h"

#ifdef BADGE_HAS_DOOM
#include "doom/DoomScreen.h"
#endif

#include <cstdio>
#include <cstring>

#include "../BadgeGlobals.h"
#include "../apps/AppRegistry.h"
#ifdef BADGE_ENABLE_BLE_PROXIMITY
#include "../ble/BadgeBeaconAdv.h"
#include "../ble/BleBeaconScanner.h"
#endif
#include "../infra/BadgeConfig.h"
#include "../ir/BadgeIR.h"
#include "../hardware/PanicReset.h"
#include "AppIcons.h"
#include "BadgeDisplay.h"
#include "hardware/Haptics.h"
#include "Images.h"
#include "hardware/Inputs.h"
#include "hardware/LEDmatrix.h"
#include "hardware/Power.h"
#include "hardware/oled.h"
#include <esp_heap_caps.h>

extern "C" {
#include "matrix_app_api.h"
void mpy_gui_exec_file(const char* path);
void mpy_collect(void) __attribute__((weak));
}

extern Scheduler scheduler;
extern SleepService sleepService;
extern oled badgeDisplay;
extern Inputs inputs;
extern LEDmatrix badgeMatrix;
extern BadgeState badgeState;
extern volatile bool mpy_oled_hold;
extern volatile bool mpy_led_hold;

bool gGuiActive = false;

// ═══════════════════════════════════════════════════════════════════════════════
//  Main menu definition
// ═══════════════════════════════════════════════════════════════════════════════
//
// Menu vocabulary — keep labels unambiguous:
//   Boop → local IR contact exchange.

extern char badgeName[];
extern char badgeTitle[];
extern char badgeCompany[];
extern uint8_t*       badgeBits;
extern int            badgeByteCount;

static void launchPythonApp(GUIManager& gui, const char* path,
                            const char* displayName) {
#ifdef BADGE_ENABLE_BLE_PROXIMITY
  BadgeBeaconAdv::setPausedForForeground(true);
  BadgeBeaconAdv::setPausedForIr(false);
#endif
  pythonIrListening = false;
  irDrainPythonRx();
#ifdef BADGE_ENABLE_BLE_PROXIMITY
  BleBeaconScanner::stopScan();
  BleBeaconScanner::clearScanCache();
  BadgeBeaconAdv::setPausedForIr(false);
#endif
  pythonIrListening = false;
  irDrainPythonRx();
  if (mpy_collect != nullptr) mpy_collect();
  Serial.printf("[appmem] enter %s largest=%u free=%u psram=%u\n",
                displayName,
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  oled& d = gui.oledDisplay();
  d.clearBuffer();
  d.setDrawColor(1);
  OLEDLayout::drawStatusHeader(d, displayName);
  OLEDLayout::drawStatusBox(d, 12, 22, 104, 24, "Launching", displayName, true);
  OLEDLayout::drawGameFooter(d);
  OLEDLayout::drawFooterActions(d, nullptr, nullptr, "exit", nullptr);
  d.sendBuffer();

  Serial.printf("GUI: Running app %s\n", path);
  mpy_gui_exec_file(path);

#ifdef BADGE_ENABLE_BLE_PROXIMITY
  BleBeaconScanner::clearScanCache();
#endif
  if (mpy_collect != nullptr) mpy_collect();
  Serial.printf("[appmem] exit %s largest=%u free=%u psram=%u\n",
                displayName,
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#ifdef BADGE_ENABLE_BLE_PROXIMITY
  BadgeBeaconAdv::setPausedForForeground(false);
#endif

  mpy_oled_hold = false;
  if (mpy_led_hold) {
    if (!matrix_app_is_active()) {
      mpy_led_hold = false;
      badgeMatrix.setMicropythonMode(false);
    }
  }

  for (int i = 0; i < 200; i++) {
    inputs.service();
    const Inputs::ButtonStates& btns = inputs.buttons();
    if (!btns.up || !btns.down || !btns.left || !btns.right) break;
    delay(10);
  }
  inputs.clearEdges();

  Serial.println("GUI: App finished, returning to menu");
  gui.requestRender();
}

static void launchSynth(GUIManager& gui) {
  launchPythonApp(gui, "/apps/synth/main.py", "Synth");
}

static void launchFlappyAsteroids(GUIManager& gui) {
  launchPythonApp(gui, "/apps/flappy_asteroids/main.py", "Flappy Asteroids");
}

#ifdef BADGE_DEV_MENU
static void launchCrashLog(GUIManager& gui) {
  launchPythonApp(gui, "/apps/crash_log/main.py", "Crash Log");
}
#endif

static void launchIRBlockBattle(GUIManager& gui) {
  launchPythonApp(gui, "/apps/ir_block_battle/main.py", "IR Block Battle");
}

static void launchBreakSnake(GUIManager& gui) {
  launchPythonApp(gui, "/apps/breaksnake/main.py", "BreakSnake");
}

// ── Curated C++ menu items ────────────────────────────────────────────────
// These are the always-on items the firmware ships with. Dynamic Python
// apps discovered by AppRegistry are appended after this list when the
// menu is rebuilt.
static const GridMenuItem kCuratedMenuItems[] = {
    {"BOOP",     "Exchange contact info over IR",
     AppIcons::booper,    kScreenBoop,     nullptr, nullptr, nullptr},
    {"CONTACTS", "Browse people you've booped",
     AppIcons::profile,   kScreenContacts, nullptr, nullptr, nullptr},
    {"BADGE INFO", "Edit your name, title, company, and contact info",
     AppIcons::profile,   kScreenBadgeInfo, nullptr, nullptr, nullptr},
    {"MAP",      "Find your way around the venue",
     AppIcons::map,       kScreenMap,      nullptr, nullptr, nullptr},
    {"SCHEDULE", "See conference sessions, workshops, and what's next",
     AppIcons::schedule, kScreenSchedule, nullptr, nullptr, nullptr},
    {"LED", "Choose LED matrix modes and animations",
     AppIcons::apps,      kScreenMatrixApps, nullptr, nullptr, nullptr},
    {"DRAW", "Draw frames and animations with stickers and pixels",
     DrawIcons::menuDraw, kScreenDrawPicker, nullptr, nullptr, nullptr},
#ifndef BADGE_DEV_MENU
    {"IR BLOCK", "Clear lines and send garbage over IR",
     AppIcons::irBlockBattle, kScreenNone, launchIRBlockBattle, nullptr, nullptr},
    {"BREAKSNAKE",  "Play Breakout and Snake together",
     AppIcons::breaksnake, kScreenNone,       launchBreakSnake, nullptr, nullptr},
    {"FLAPPY", "Play Asteroids and Flappy Bird together",
     AppIcons::flappyAsteroids, kScreenNone,  launchFlappyAsteroids, nullptr, nullptr},
#endif
    {"SYNTH", "Play joystick tones, loops, and loadable sounds",
     AppIcons::synth,     kScreenNone,       launchSynth, nullptr, nullptr},
#ifdef BADGE_HAS_DOOM
    {"DOOM",        "Play DOOM on the badge",
     AppIcons::doom,      kScreenDoom,        nullptr, nullptr, nullptr},
#endif
    {"SPONSORS", "Thank you to our sponsors!",
     AppIcons::about,     kScreenAboutSponsors, nullptr, nullptr, nullptr},
#ifdef BADGE_DEV_MENU
    {"INPUT TEST",  "Check joystick, buttons, tilt, and raw input values",
     AppIcons::apps,      kScreenInputTest,   nullptr, nullptr, nullptr},
    {"APPS",        "Run MicroPython apps stored on the badge",
     AppIcons::apps,      kScreenApps,        nullptr, nullptr, nullptr},
    {"CRASH LOG",   "View the last MicroPython app crash",
     AppIcons::about,     kScreenNone,        launchCrashLog, nullptr, nullptr},
    {"HAPTICS",     "Preview vibration strength, frequency, and duration",
     AppIcons::workflow,  kScreenHaptics,     nullptr, nullptr, nullptr},
    {"FILES",       "Browse files on the badge filesystem",
     AppIcons::directory, kScreenFiles,       nullptr, nullptr, nullptr},
    {"ANIM TEST",   "Preview OLED and LED matrix animations",
     AppIcons::apps,      kScreenAnimTest,    nullptr, nullptr, nullptr},
    {"DIAGNOSTICS", "Inspect runtime state, tasks, battery, and memory",
     AppIcons::about,     kScreenDiagnostics, nullptr, nullptr, nullptr},
#endif
    {"SETTINGS", "Adjust display, input, power, and haptics",
     AppIcons::settings,  kScreenSettings, nullptr, nullptr, nullptr},
};

static constexpr size_t kCuratedMenuItemCount =
    sizeof(kCuratedMenuItems) / sizeof(kCuratedMenuItems[0]);

// ── Dynamic-app menu storage + trampolines ────────────────────────────────
// AppRegistry discovers self-describing /apps/<slug>/main.py files and we
// surface them as additional grid items. GridMenuItem.action takes a
// `void(*)(GUIManager&)`, with no per-item closure, so we need a trampoline
// per slot that knows which AppRegistry index it points at.

static constexpr size_t kMaxMenuItems =
    kCuratedMenuItemCount + AppRegistry::kMaxDynamicApps;

static GridMenuItem sMenuItems[kMaxMenuItems];
static uint8_t sMenuItemCount = 0;

static char sDynamicLabel[AppRegistry::kMaxDynamicApps]
                         [AppRegistry::kTitleCap];
static char sDynamicDescription[AppRegistry::kMaxDynamicApps]
                               [AppRegistry::kDescriptionCap];
static uint8_t sDynamicIcon[AppRegistry::kMaxDynamicApps]
                           [AppRegistry::kIconBytes];
static char sDynamicEntry[AppRegistry::kMaxDynamicApps]
                         [AppRegistry::kEntryPathCap];

// Dispatch a dynamic-app launch via AppRegistry index. Looked up at call
// time — AppRegistry::count() may shrink between menu rebuild and click.
static void launchDynamicAppByIndex(GUIManager& gui, size_t index) {
  if (index >= AppRegistry::count()) return;
  launchPythonApp(gui, sDynamicEntry[index], sDynamicLabel[index]);
}

// Trampoline pool — one function per slot. Generated by macro to keep the
// table dense and avoid templates that the embedded compiler may not
// like.
#define DYN_APP_TRAMPOLINE(N) \
  static void launchDynamicApp##N(GUIManager& gui) { \
    launchDynamicAppByIndex(gui, N); \
  }

DYN_APP_TRAMPOLINE(0)
DYN_APP_TRAMPOLINE(1)
DYN_APP_TRAMPOLINE(2)
DYN_APP_TRAMPOLINE(3)
DYN_APP_TRAMPOLINE(4)
DYN_APP_TRAMPOLINE(5)
DYN_APP_TRAMPOLINE(6)
DYN_APP_TRAMPOLINE(7)
DYN_APP_TRAMPOLINE(8)
DYN_APP_TRAMPOLINE(9)
DYN_APP_TRAMPOLINE(10)
DYN_APP_TRAMPOLINE(11)
DYN_APP_TRAMPOLINE(12)
DYN_APP_TRAMPOLINE(13)
DYN_APP_TRAMPOLINE(14)
DYN_APP_TRAMPOLINE(15)
DYN_APP_TRAMPOLINE(16)
DYN_APP_TRAMPOLINE(17)
DYN_APP_TRAMPOLINE(18)
DYN_APP_TRAMPOLINE(19)
DYN_APP_TRAMPOLINE(20)
DYN_APP_TRAMPOLINE(21)
DYN_APP_TRAMPOLINE(22)
DYN_APP_TRAMPOLINE(23)
DYN_APP_TRAMPOLINE(24)
DYN_APP_TRAMPOLINE(25)
DYN_APP_TRAMPOLINE(26)
DYN_APP_TRAMPOLINE(27)
DYN_APP_TRAMPOLINE(28)
DYN_APP_TRAMPOLINE(29)
DYN_APP_TRAMPOLINE(30)
DYN_APP_TRAMPOLINE(31)
#undef DYN_APP_TRAMPOLINE

static GridMenuAction kDynamicLaunchTable[AppRegistry::kMaxDynamicApps] = {
    launchDynamicApp0,  launchDynamicApp1,  launchDynamicApp2,
    launchDynamicApp3,  launchDynamicApp4,  launchDynamicApp5,
    launchDynamicApp6,  launchDynamicApp7,  launchDynamicApp8,
    launchDynamicApp9,  launchDynamicApp10, launchDynamicApp11,
    launchDynamicApp12, launchDynamicApp13, launchDynamicApp14,
    launchDynamicApp15, launchDynamicApp16, launchDynamicApp17,
    launchDynamicApp18, launchDynamicApp19, launchDynamicApp20,
    launchDynamicApp21, launchDynamicApp22, launchDynamicApp23,
    launchDynamicApp24, launchDynamicApp25, launchDynamicApp26,
    launchDynamicApp27, launchDynamicApp28, launchDynamicApp29,
    launchDynamicApp30, launchDynamicApp31,
};

static_assert(sizeof(kDynamicLaunchTable) /
                      sizeof(kDynamicLaunchTable[0]) ==
                  AppRegistry::kMaxDynamicApps,
              "Dynamic app trampoline pool out of sync with "
              "AppRegistry::kMaxDynamicApps — extend the DYN_APP_TRAMPOLINE "
              "block above to match.");

// Forward decl so badge.rescan_apps() can drive a refresh.
extern "C" void rebuildMainMenuFromRegistry(void);

// ═══════════════════════════════════════════════════════════════════════════════
//  Screen instances
// ═══════════════════════════════════════════════════════════════════════════════

#ifdef BADGE_DEV_MENU
static DiagnosticsScreen sDiagnostics;
static AnimTestScreen sAnimTest;
#endif
static BootScreen sBoot;
// Built initially from kCuratedMenuItems; rebuildMainMenuFromRegistry()
// merges in dynamic apps before the menu first renders.
static GridMenuScreen sMainMenu(kScreenMainMenu, "MENU",
                                sMenuItems, 0);
static ScheduleScreen sSchedule;
static MapScreen sMap;
static MapSectionScreen sMapSection;
static MapFloorScreen sMapFloor;
static SectionModalScreen sSectionModal;
static ScheduleDetailModalScreen sScheduleDetailModal;
static SettingsScreen sSettings(kScreenSettings, &badgeConfig);
#ifdef BADGE_DEV_MENU
static HapticsTestScreen sHaptics(kScreenHaptics);
static FilesScreen sFiles(kScreenFiles);
static AppsScreen sApps(kScreenApps, "/apps", "APPS",
                        kScreenTests, "tests/");
static AppsScreen sTests(kScreenTests, "/tests", "TESTS");
#endif
static LEDScreen sMatrixApps;
FileViewScreen sFileView;
static BoopScreen sBoop;
#ifdef BADGE_DEV_MENU
static InputTestScreen sInputTest;
#endif
TextInputScreen sTextInput;
static BadgeInfoViewScreen sBadgeInfoView;
static AboutSponsorsScreen sAboutSponsors;
#ifdef BADGE_HAS_DOOM
static DoomScreen sDoom;
#endif

// Rebuild sMenuItems from curated items + AppRegistry. Safe to call on
// boot and from badge.rescan_apps() at runtime. Re-points sMainMenu at
// the freshly-populated buffer so the next render walks the new items.
extern "C" void rebuildMainMenuFromRegistry(void) {
  size_t cursor = 0;
  for (size_t i = 0; i < kCuratedMenuItemCount && cursor < kMaxMenuItems;
       i++) {
    sMenuItems[cursor++] = kCuratedMenuItems[i];
  }

  AppRegistry::rescan();
  size_t dynCount = AppRegistry::count();
  if (dynCount > AppRegistry::kMaxDynamicApps) {
    dynCount = AppRegistry::kMaxDynamicApps;
  }

  for (size_t i = 0; i < dynCount && cursor < kMaxMenuItems; i++) {
    const AppRegistry::DynamicApp* app = AppRegistry::at(i);
    if (!app) break;

    strncpy(sDynamicLabel[i], app->title, AppRegistry::kTitleCap - 1);
    sDynamicLabel[i][AppRegistry::kTitleCap - 1] = '\0';
    strncpy(sDynamicDescription[i], app->description,
            AppRegistry::kDescriptionCap - 1);
    sDynamicDescription[i][AppRegistry::kDescriptionCap - 1] = '\0';
    strncpy(sDynamicEntry[i], app->entryPath,
            AppRegistry::kEntryPathCap - 1);
    sDynamicEntry[i][AppRegistry::kEntryPathCap - 1] = '\0';
    memcpy(sDynamicIcon[i], app->icon, AppRegistry::kIconBytes);

    GridMenuItem& slot = sMenuItems[cursor++];
    slot = GridMenuItem{};
    slot.label = sDynamicLabel[i];
    slot.description = sDynamicDescription[i];
    slot.icon = app->hasCustomIcon ? sDynamicIcon[i] : AppIcons::apps;
    slot.target = kScreenNone;
    slot.action = kDynamicLaunchTable[i];
    slot.iconW = AppRegistry::kIconWidth;
    slot.iconH = AppRegistry::kIconHeight;
  }

  sMenuItemCount = static_cast<uint8_t>(cursor);
  sMainMenu.setItems(sMenuItems, sMenuItemCount);
  Serial.printf(
      "[apps] main menu rebuilt: %u curated + %u dynamic = %u total\n",
      static_cast<unsigned>(kCuratedMenuItemCount),
      static_cast<unsigned>(dynCount),
      static_cast<unsigned>(sMenuItemCount));
}

// Used by GridMenuScreen for the "Hi <name>" main-menu title.
void firstNameFromBadgeName(char* out, size_t outCap) {
  if (!out || outCap == 0) return;
  out[0] = '\0';
  const char* src = badgeName;
  while (*src == ' ') src++;
  if (!*src || strcmp(src, "YOUR_NAME") == 0) return;

  size_t i = 0;
  while (src[i] && src[i] != ' ' && i < outCap - 1) {
    out[i] = src[i];
    i++;
  }
  out[i] = '\0';
}

// ═══════════════════════════════════════════════════════════════════════════════
//  GUIManager
// ═══════════════════════════════════════════════════════════════════════════════

void GUIManager::begin(oled* display, Inputs* inputs) {
  oled_ = display;
  inputs_ = inputs;

#ifdef BADGE_DEV_MENU
  registerScreen(&sDiagnostics);
#endif
  registerScreen(&sBoot);
  registerScreen(&sMainMenu);
  registerScreen(&sSchedule);
  registerScreen(&sMap);
  registerScreen(&sMapSection);
  registerScreen(&sMapFloor);
  registerScreen(&sSectionModal);
  registerScreen(&sScheduleDetailModal);
  registerScreen(&sSettings);
#ifdef BADGE_DEV_MENU
  registerScreen(&sHaptics);
  registerScreen(&sFiles);
  registerScreen(&sTests);
  registerScreen(&sFileView);
  registerScreen(&sApps);
#endif
  registerScreen(&sMatrixApps);
  registerScreen(&sBoop);
#ifdef BADGE_DEV_MENU
  registerScreen(&sInputTest);
  registerScreen(&sAnimTest);
#endif
  registerScreen(&sTextInput);
  registerScreen(&sBadgeInfoView);
  registerScreen(&sContacts);
  registerScreen(&sContactDetail);
  registerScreen(&sDrawPicker);
  registerScreen(&sDrawScreen);
  registerScreen(&sStickerPicker);
  registerScreen(&sScalePicker);
  registerScreen(&sAboutSponsors);
#ifdef BADGE_HAS_DOOM
  registerScreen(&sDoom);
#endif

  // Populate the main-menu grid with curated items + AppRegistry-discovered
  // dynamic Python apps before the first render. Safe to call again later
  // via badge.rescan_apps() to hot-refresh the menu from JumperIDE.
  rebuildMainMenuFromRegistry();

  stackDepth_ = 0;
  routedBadgeState_ = static_cast<uint8_t>(badgeState);
  pushScreen(kScreenBoot);
  active_ = true;
  gGuiActive = true;

  scheduler.setServiceState("OLED", false);
  Serial.printf("GUI: initialized (screens=%u cap=%u)\n",
                (unsigned)screenCount_, (unsigned)kMaxScreens);
}

void GUIManager::handleInputIfActive() {
  if (!active_ || !oled_ || !inputs_) return;

  const Inputs::ButtonStates& b = inputs_->buttons();
  if (b.up && b.down && b.left && b.right) {
    return;
  }

  updateCursor();
  const Inputs::ButtonEdges& e = inputs_->edges();
  const bool anyButtonEdge = e.yPressed || e.aPressed ||
                             e.xPressed || e.bPressed;

  // Nametag overlay (badge flipped upside-down): any button edge bumps
  // the idle timer, and when the overlay is currently showing the edge
  // is absorbed — dismissing the overlay without leaking the press
  // into the screen underneath (otherwise B would silently pop the
  // current screen on the very first press).
  if (nametagMode_ != NametagMode::kDisabled) {
    if (anyButtonEdge) {
      nametagLastInputMs_ = millis();
      if (nametagMode_ == NametagMode::kShown) {
        nametagMode_ = NametagMode::kHidden;
        unloadNametagAnimationDoc();
        requestRender();
        return;
      } else if (nametagMode_ == NametagMode::kPendingEnter) {
        // A press during the inverted entry delay means "keep interacting";
        // don't immediately pop into the overlay when the delay elapses.
        nametagMode_ = NametagMode::kHidden;
      }
    }
  }

  Screen* screen = currentScreen();
  if (!screen) return;

  int16_t cx = static_cast<int16_t>(cursorX_);
  int16_t cy = static_cast<int16_t>(cursorY_);
  screen->handleInput(*inputs_, cx, cy, *this);
  if (anyButtonEdge) requestRender();
}

void GUIManager::service() {
  if (!oled_ || !inputs_) return;
  if (PanicReset::rebootPending()) return;

  if (!active_) {
    applyDisplayPolicy(DisplayPolicy::kNormal);
    checkActivation(millis());
    return;
  }

  uint32_t now = millis();

  if (mpy_oled_hold) {
    const Inputs::ButtonEdges& e = inputs_->edges();
    if (e.yPressed || e.aPressed || e.xPressed || e.bPressed) {
      mpy_oled_hold = false;
      if (mpy_led_hold) {
        mpy_led_hold = false;
        badgeMatrix.setMicropythonMode( false );
        matrix_app_end_override();
      }
    } else {
      return;
    }
  }

  syncRouteForBadgeState();

  const bool forcedRender = renderRequested_;
  if (!forcedRender && now - lastRenderMs_ < kRenderIntervalMs) return;
  lastRenderMs_ = now;

  // Nametag overlay idle re-entry: while the badge is still inverted
  // and the user has been quiet for kNametagIdleMs, bring the overlay
  // back. Evaluated in service() rather than handleInputIfActive so the
  // timer advances even if no input tick ran this frame. QR/unpaired mode
  // keeps the pairing code stable and scannable; Boop suppresses only the
  // nametag overlay. Input stays in the normal interactive orientation.
  if (nametagInverted_ && nametagSuppressedForCurrentScreen()) {
    unloadNametagAnimationDoc();
    nametagMode_ = NametagMode::kDisabled;
  } else if (nametagInverted_ && nametagMode_ == NametagMode::kDisabled) {
    nametagMode_ = NametagMode::kPendingEnter;
    nametagPendingSinceMs_ = now;
  }

  if (nametagMode_ == NametagMode::kPendingEnter &&
      (now - nametagPendingSinceMs_) >= kNametagEnterDelayMs) {
    nametagMode_ = NametagMode::kShown;
    nametagLastInputMs_ = now;
  }

  if (nametagMode_ == NametagMode::kHidden &&
      (now - nametagLastInputMs_) >= kNametagIdleMs) {
    nametagMode_ = NametagMode::kShown;
  }

  if (nametagMode_ == NametagMode::kShown) {
    applyDisplayPolicy(DisplayPolicy::kNametag);
    oled_->clearBuffer();
    oled_->setDrawColor(1);
    renderNametagOverlay(*oled_);
    oled_->sendBuffer();
    renderRequested_ = false;
    return;
  }

  // kPendingEnter keeps the display in normal orientation — the OLED
  // doesn't flip until we actually render the nametag (kShown). This avoids
  // the artifact of the display inverting before the nametag doc is ready.
  applyDisplayPolicy(DisplayPolicy::kNormal);

  Screen* screen = currentScreen();
  if (!screen || !active_) return;

  // Per-screen frame-skip opt-out. Default Screen::needsRender is true.
  if (!forcedRender && !screen->needsRender()) return;

  oled_->clearBuffer();
  oled_->setFontPreset(FONT_TINY);
  oled_->setDrawColor(1);

  screen->render(*oled_, *this);

  if (screen->showCursor()) {
    drawXorCursor();
  }

  oled_->sendBuffer();
  renderRequested_ = false;
}

// ─── Cursor ──────────────────────────────────────────────────────────────────

void GUIManager::updateCursor() {
  cursorX_ = static_cast<float>(inputs_->joyX()) * (127.0f / 4095.0f);
  cursorY_ = static_cast<float>(inputs_->joyY()) * (63.0f / 4095.0f);
}

void GUIManager::drawXorCursor() {
  oled_->setDrawColor(2);
  oled_->drawBitmap(static_cast<int>(cursorX_), static_cast<int>(cursorY_),
                    GUIImages::kCursorBitmap, GUIImages::kCursorW,
                    GUIImages::kCursorH);
  oled_->setDrawColor(1);
}

// ─── Screen stack ────────────────────────────────────────────────────────────

void GUIManager::pushScreen(ScreenId sid) {
  sid = resolveScreenForBadgeState(sid);
  if (stackDepth_ >= kMaxStack) return;
  Screen* scr = findScreen(sid);
  if (!scr) {
    // Keep this warning — it's the only way to catch future silent-drop
    // regressions if the registration list outgrows kMaxScreens again.
    Serial.printf("[GUI] pushScreen(id=%u) FAILED: screen not registered "
                  "(screenCount=%u)\n",
                  (unsigned)sid, (unsigned)screenCount_);
    return;
  }
  enterTransitionOut();
  stack_[stackDepth_++] = sid;
  scr->onEnter(*this);
  enterTransitionIn();
  requestRender();
}

void GUIManager::resetStackTo(ScreenId sid) {
  sid = resolveScreenForBadgeState(sid);
  Screen* scr = findScreen(sid);
  if (!scr) {
    Serial.printf("[GUI] resetStackTo(id=%u) FAILED: screen not registered "
                  "(screenCount=%u)\n",
                  (unsigned)sid, (unsigned)screenCount_);
    return;
  }

  enterTransitionOut();

  while (stackDepth_ > 0) {
    Screen* old = findScreen(stack_[stackDepth_ - 1]);
    if (old) old->onExit(*this);
    stackDepth_--;
  }

  routedBadgeState_ = static_cast<uint8_t>(badgeState);
  stack_[stackDepth_++] = sid;
  scr->onEnter(*this);
  enterTransitionIn();
  requestRender();
}

void GUIManager::popScreen() {
  if (stackDepth_ <= 1) return;
  enterTransitionOut();
  Screen* old = findScreen(stack_[stackDepth_ - 1]);
  if (old) old->onExit(*this);
  stackDepth_--;
  Screen* now = currentScreen();
  if (now) now->onResume(*this);
  enterTransitionIn();
  requestRender();
}

void GUIManager::popScreens(uint8_t n) {
  Serial.printf("[GUI] popScreens(%u) entry  depth=%u  top=%u\n",
                (unsigned)n, (unsigned)stackDepth_,
                stackDepth_ > 0 ? (unsigned)stack_[stackDepth_ - 1] : 0u);
  if (n == 0) return;
  if (stackDepth_ <= 1) return;
  // Clamp so we always leave at least one screen on the stack.
  if (n > stackDepth_ - 1) n = stackDepth_ - 1;
  enterTransitionOut();
  for (uint8_t i = 0; i < n; i++) {
    ScreenId popping = stack_[stackDepth_ - 1];
    Screen* old = findScreen(popping);
    if (old) old->onExit(*this);
    stackDepth_--;
    Serial.printf("[GUI] popScreens  popped id=%u  newDepth=%u\n",
                  (unsigned)popping, (unsigned)stackDepth_);
  }
  enterTransitionIn();
  requestRender();
}

void GUIManager::replaceScreen(ScreenId sid) {
  sid = resolveScreenForBadgeState(sid);
  Screen* scr = findScreen(sid);
  if (!scr) {
    Serial.printf("[GUI] replaceScreen(id=%u) FAILED: screen not registered "
                  "(screenCount=%u)\n",
                  (unsigned)sid, (unsigned)screenCount_);
    return;
  }
  enterTransitionOut();
  if (stackDepth_ > 0) {
    Screen* old = findScreen(stack_[stackDepth_ - 1]);
    if (old) old->onExit(*this);
    stack_[stackDepth_ - 1] = sid;
  } else {
    stack_[stackDepth_++] = sid;
  }
  scr->onEnter(*this);
  enterTransitionIn();
  requestRender();
}

Screen* GUIManager::currentScreen() {
  if (stackDepth_ == 0) return nullptr;
  return findScreen(stack_[stackDepth_ - 1]);
}

// ─── Hardware fade transitions ───────────────────────────────────────────────
//
// Wrap stack mutations with a contrast fade so screen swaps don't pop. The
// initial home-screen push during begin() is skipped (active_ is still false
// there); after that every push/pop/replace fades the panel down, swaps the
// content under the dark, then ramps back up to the user's brightness.

void GUIManager::renderCurrentToPanel() {
  if (!oled_) return;
  Screen* screen = currentScreen();
  if (!screen) return;
  oled_->clearBuffer();
  oled_->setFontPreset(FONT_TINY);
  oled_->setDrawColor(1);
  screen->render(*oled_, *this);
  if (screen->showCursor()) {
    drawXorCursor();
  }
  oled_->sendBuffer();
}

void GUIManager::enterTransitionOut() {
  if (!active_ || !oled_) return;
  oled_->transitionOut();
}

void GUIManager::enterTransitionIn() {
  if (!active_ || !oled_) return;
  // Render the new content into the dark buffer first so the fade-up
  // reveals the destination screen, not whatever was there before.
  renderCurrentToPanel();
  oled_->transitionIn();
}

void GUIManager::registerScreen(Screen* screen) {
  if (screenCount_ < kMaxScreens) {
    screens_[screenCount_++] = screen;
    return;
  }
  // Loud warning on overflow — the historical silent-drop behavior is how
  // the Contacts menu broke when the registration list outgrew kMaxScreens.
  // If this ever fires again, bump kMaxScreens in GUI.h.
  Serial.printf("[GUI] registerScreen: dropped screen id=%u (cap=%u)\n",
                screen ? (unsigned)screen->id() : 0u,
                (unsigned)kMaxScreens);
}

Screen* GUIManager::findScreen(ScreenId sid) {
  for (uint8_t i = 0; i < screenCount_; i++) {
    if (screens_[i]->id() == sid) return screens_[i];
  }
  return nullptr;
}

ScreenId GUIManager::homeScreenForBadgeState() const {
  return kScreenMainMenu;
}

bool GUIManager::screenAllowedForBadgeState(const Screen* screen) const {
  if (!screen) return false;
  switch (screen->access()) {
    case ScreenAccess::kAny:
      return true;
    case ScreenAccess::kPairedOnly:
      return true;
    case ScreenAccess::kUnpairedOnly:
      return false;
  }
  return false;
}

ScreenId GUIManager::resolveScreenForBadgeState(ScreenId sid) {
  Screen* screen = findScreen(sid);
  if (!screen) return sid;
  if (screenAllowedForBadgeState(screen)) return sid;

  const ScreenId home = homeScreenForBadgeState();
  if (sid != home) {
    Serial.printf("[GUI] screen id=%u blocked while %s; routing to id=%u\n",
                  (unsigned)sid,
                  badgeStateName(badgeState),
                  (unsigned)home);
  }
  return home;
}

void GUIManager::syncRouteForBadgeState() {
  const uint8_t currentState = static_cast<uint8_t>(badgeState);
  Screen* current = currentScreen();
  if (routedBadgeState_ == currentState &&
      screenAllowedForBadgeState(current)) {
    return;
  }

  routedBadgeState_ = currentState;
  const ScreenId home = homeScreenForBadgeState();
  if (current && current->id() == home && screenAllowedForBadgeState(current)) {
    requestRender();
    return;
  }

  while (stackDepth_ > 0) {
    Screen* old = findScreen(stack_[stackDepth_ - 1]);
    if (old) old->onExit(*this);
    stackDepth_--;
  }
  pushScreen(home);
}

void GUIManager::applyDisplayPolicy(DisplayPolicy policy) {
  if (!oled_) return;
  if (!displayPolicyApplied_ || displayPolicy_ != policy) {
    oled_->setFlipped(policy == DisplayPolicy::kNametag);
    displayPolicy_ = policy;
    displayPolicyApplied_ = true;
  }
}

void GUIManager::requestRender() {
  renderRequested_ = true;
}

// ─── Activation / deactivation ───────────────────────────────────────────────

void GUIManager::activate() {
  active_ = true;
  gGuiActive = true;
  scheduler.setServiceState("OLED", false);
  stackDepth_ = 0;
  routedBadgeState_ = static_cast<uint8_t>(badgeState);
  pushScreen(homeScreenForBadgeState());
  cursorX_ = 64.0f;
  cursorY_ = 32.0f;
  sleepService.caffeine = true;
  requestRender();
  Serial.println("GUI: activated");
}

void GUIManager::activateKeepStack() {
  active_ = true;
  gGuiActive = true;
  scheduler.setServiceState("OLED", false);
  requestRender();
  Serial.println("GUI: activated (keep stack)");
}

void GUIManager::deactivate() {
  active_ = false;
  gGuiActive = false;
  scheduler.setServiceState("OLED", true);
  scheduler.setExecutionDivisors(Power::Policy::schedulerHighDivisor,
                                 Power::Policy::schedulerNormalDivisor,
                                 Power::Policy::schedulerLowDivisor);
  applyDisplayPolicy(DisplayPolicy::kNormal);
  Serial.println("GUI: deactivated");
}

void GUIManager::checkActivation(uint32_t nowMs) {
  const Inputs::ButtonStates& b = inputs_->buttons();
  if (b.a && b.b && !b.y && !b.x) {
    if (activateHoldStartMs_ == 0) {
      activateHoldStartMs_ = nowMs;
    } else if (nowMs - activateHoldStartMs_ >= kActivateHoldMs) {
      activate();
      activateHoldStartMs_ = 0;
    }
  } else {
    activateHoldStartMs_ = 0;
  }
}

// ─── Nametag overlay ────────────────────────────────────────────────────────
//
// The main loop's IMU flip handler drives this. On every orientation
// change it calls setNametagMode(orient == kInverted); that's the only
// external entry point. Everything else — dismissing on input, 30 s idle
// re-entry, full-screen render — is handled inside the GUIManager render
// loop (see handleInputIfActive and service above).

bool GUIManager::nametagSuppressedForCurrentScreen() const {
  if (stackDepth_ == 0) return false;
  const ScreenId sid = stack_[stackDepth_ - 1];
  return sid == kScreenBoop;
}

void GUIManager::setNametagMode(bool inverted) {
  nametagInverted_ = inverted;
  if (inverted && !nametagSuppressedForCurrentScreen()) {
    nametagMode_ = NametagMode::kPendingEnter;
    nametagPendingSinceMs_ = millis();
  } else {
    nametagMode_ = NametagMode::kDisabled;
    unloadNametagAnimationDoc();
    // Leaving nametag: unflip immediately so the next render is upright.
    if (oled_) oled_->setFlipped(false);
    displayPolicyApplied_ = false;
  }
  requestRender();
}

void GUIManager::renderNametagOverlay(oled& d) {
  d.setTextWrap(false);
  d.setDrawColor(1);

  if (gCustomNametagEnabled && ensureNametagAnimationDocLoaded() &&
      gNametagDoc) {
    sDrawScreen.renderDocComposition(d, *gNametagDoc, 0);
    return;
  }

  if (gCustomNametagEnabled && badgeBits && badgeByteCount >= 1024) {
    d.drawXBM(0, 0, 128, 64, badgeBits);
    return;
  }

  const char* name     = badgeName[0]    ? badgeName    : "(set name)";
  const char* subtitle = badgeCompany[0] ? badgeCompany : nullptr;
  const char* tertiary = badgeTitle[0]   ? badgeTitle   : nullptr;
  d.drawNametag(name, subtitle, tertiary,
                /*x=*/0, /*y=*/0, /*maxW=*/128, /*maxH=*/64);
}
