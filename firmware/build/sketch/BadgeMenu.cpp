#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeMenu.cpp"
// BadgeMenu.cpp — Menu rendering and joystick navigation

#include "BadgeMenu.h"
#include "BadgeConfig.h"
#include "BadgeDisplay.h"
#include "BadgeInput.h"

// ─── Menu state ───────────────────────────────────────────────────────────────
int menuIndex = 0;
static int  menuScrollTop  = 0;   // index of first visible item
static bool menuNavLocked  = false;

extern BadgeState badgeState;
extern uint8_t* qrBits;

// ─── renderListMenu ───────────────────────────────────────────────────────────
void renderListMenu(const char* const* items, int count,
                    int selectedIdx, int scrollTop, const char* hint) {
  setDisplayFlip(false);
  u8g2.clearBuffer();

  const int ITEM_H  = 13;
  const int START_Y = 1;

  u8g2.setFont(u8g2_font_6x10_tf);
  for (int i = 0; i < MENU_VISIBLE; i++) {
    int itemIdx = scrollTop + i;
    if (itemIdx >= count) break;
    int y = START_Y + i * ITEM_H;
    if (itemIdx == selectedIdx) {
      u8g2.drawBox(0, y, 122, ITEM_H);
      u8g2.setDrawColor(0);
    }
    u8g2.drawStr(8, y + ITEM_H - 3, items[itemIdx]);
    u8g2.setDrawColor(1);
  }

  // Scroll indicators (right edge)
  if (scrollTop > 0) {
    u8g2.drawTriangle(125, START_Y + 3, 122, START_Y + 7, 128, START_Y + 7);
  }
  if (scrollTop + MENU_VISIBLE < count) {
    int arrowY = START_Y + MENU_VISIBLE * ITEM_H - 1;
    u8g2.drawTriangle(125, arrowY, 122, arrowY - 4, 128, arrowY - 4);
  }

  u8g2.setFont(u8g2_font_4x6_tf);
  u8g2.drawStr(0, 63, hint);

  u8g2.sendBuffer();
}

// ─── renderMenu ───────────────────────────────────────────────────────────────
void renderMenu() {
  const char* qrLabel = (badgeState == BADGE_PAIRED || qrBits != nullptr) ? "QR Code" : "QR / Pair";
  const char* items[MENU_COUNT] = {
    "Boop", "Messages", qrLabel, "Input Test", "Apps"
  };
  renderListMenu(items, MENU_COUNT, menuIndex, menuScrollTop,
                 "joy:nav  v:select  >:back");
}

// ─── menuHandleJoystick ───────────────────────────────────────────────────────
void menuHandleJoystick(float ny) {
  if (ny < -MENU_NAV_THRESHOLD && !menuNavLocked) {
    menuIndex = (menuIndex - 1 + MENU_COUNT) % MENU_COUNT;
    // Scroll viewport up if cursor moved above it
    if (menuIndex < menuScrollTop)
      menuScrollTop = menuIndex;
    // Wrap from top to bottom — snap viewport to show last items
    if (menuIndex == MENU_COUNT - 1)
      menuScrollTop = MENU_COUNT - MENU_VISIBLE;
    screenDirty   = true;
    menuNavLocked = true;
  } else if (ny > MENU_NAV_THRESHOLD && !menuNavLocked) {
    menuIndex = (menuIndex + 1) % MENU_COUNT;
    // Scroll viewport down if cursor moved below it
    if (menuIndex >= menuScrollTop + MENU_VISIBLE)
      menuScrollTop = menuIndex - MENU_VISIBLE + 1;
    // Wrap from bottom to top — snap viewport to start
    if (menuIndex == 0)
      menuScrollTop = 0;
    screenDirty   = true;
    menuNavLocked = true;
  } else if (fabsf(ny) < MENU_NAV_THRESHOLD) {
    menuNavLocked = false;
  }
}

// ─── appsMenuLoop ─────────────────────────────────────────────────────────────
int appsMenuLoop(const char names[][32], int count) {
  int idx      = 0;
  bool navLock = false;

  // Debounce: wait until BTN_DOWN (which triggered this) is released
  waitButtonRelease(BTN_DOWN, 10);
  delay(50);

  // Build pointer array for renderListMenu
  const char* ptrs[16];
  for (int i = 0; i < count && i < 16; i++) ptrs[i] = names[i];

  while (true) {
    DISPLAY_TAKE();
    renderListMenu(ptrs, count, idx, viewportStart(idx, MENU_VISIBLE), "v:run  >:back");
    DISPLAY_GIVE();

    // Poll joystick for navigation
    int rawY = analogRead(JOY_Y);
    float ny = (rawY / 2047.5f) - 1.0f;
    if (fabsf(ny) < JOY_DEADBAND) ny = 0.0f;

    if (ny < -MENU_NAV_THRESHOLD && !navLock) {
      idx     = (idx - 1 + count) % count;
      navLock = true;
    } else if (ny > MENU_NAV_THRESHOLD && !navLock) {
      idx     = (idx + 1) % count;
      navLock = true;
    } else if (fabsf(ny) < MENU_NAV_THRESHOLD) {
      navLock = false;
    }

    // BTN_DOWN: select
    if (digitalRead(BTN_DOWN) == LOW) {
      waitButtonRelease(BTN_DOWN, 10);
      delay(50);
      return idx;
    }

    // BTN_RIGHT: cancel
    if (digitalRead(BTN_RIGHT) == LOW) {
      waitButtonRelease(BTN_RIGHT, 10);
      return -1;
    }

    delay(30);
  }
}
