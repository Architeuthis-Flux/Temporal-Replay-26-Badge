#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeMenu.h"
// BadgeMenu.h — Menu rendering and joystick navigation
//
// renderMenu()          — draw menu screen via U8G2; sets display flip, sends buffer
// menuHandleJoystick()  — threshold-triggered up/down navigation with centre-unlock

#pragma once
#include <Arduino.h>

#define MENU_COUNT       5

// Named menu index constants — use these everywhere instead of magic numbers.
#define MENU_BOOP        0
#define MENU_MESSAGES    1
#define MENU_QR_PAIR     2
#define MENU_INPUT_TEST  3
#define MENU_APPS        4

// Visible items shown at once (scrolling menu)
#define MENU_VISIBLE     4

extern int menuIndex;

// Viewport helper: first visible item for a scrolling list with fixed-size pages.
// e.g. viewportStart(5, 4) == 4  (item 5 is in the second page: items 4-7)
static inline int viewportStart(int sel, int vis) { return (sel / vis) * vis; }

// Shared list renderer used by renderMenu() and appsMenuLoop().
// Draws MENU_VISIBLE items starting at scrollTop, highlights selectedIdx,
// shows scroll indicators if the list overflows, and puts hint at y=63.
// Does NOT take/release the display mutex — caller is responsible.
void renderListMenu(const char* const* items, int count,
                    int selectedIdx, int scrollTop, const char* hint);

void renderMenu();
void menuHandleJoystick(float ny);

// Blocking sub-menu for app selection. Displays names[], navigates with joystick,
// confirms with BTN_DOWN, cancels with BTN_RIGHT.
// Returns selected index (0..count-1) or -1 if user pressed back.
int appsMenuLoop(const char names[][32], int count);
