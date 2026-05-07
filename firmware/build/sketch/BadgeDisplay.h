#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeDisplay.h"
// BadgeDisplay.h — Public interface for the BadgeDisplay module
// See FR-004 in spec.md
//
// U8G2 instance, display mutex, all render functions, modal system, XBM drawing helpers.
// Call order constraint: u8g2.begin() must be called in setup() before any render function.
// displayMutex must be created (xSemaphoreCreateMutex) before irTask is launched.
//
// Thread safety:
//   renderScreen(), renderModal(), showModal() acquire displayMutex internally.
//   bootPrint() is called during setup() before irTask exists — no mutex needed.
//   setDisplayFlip() and setDisplayContrast() must only be called while holding the mutex.

#pragma once
#include <Arduino.h>
#include <U8g2lib.h>
#include "BadgeMenu.h"

// ─── Display instance (accessible for advanced use; prefer render functions) ──
extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
extern SemaphoreHandle_t displayMutex;

#define DISPLAY_TAKE()  xSemaphoreTake(displayMutex, portMAX_DELAY)
#define DISPLAY_GIVE()  xSemaphoreGive(displayMutex)

// ─── Badge state (shared across modules) ─────────────────────────────────────
enum BadgeState { BADGE_UNPAIRED, BADGE_PAIRED };

// ─── Screen state (shared with main sketch and BadgeIR) ──────────────────────
extern String screenLine1;
extern String screenLine2;
extern bool   screenDirty;

enum RenderMode { MODE_BOOT, MODE_QR, MODE_BOOP, MODE_MENU, MODE_INPUT_TEST, MODE_BOOP_RESULT };
extern RenderMode     renderMode;
extern unsigned long  inputTestLastActivity;
extern unsigned long  boopResultShownAt;

// ─── Display control ─────────────────────────────────────────────────────────

// Initialize display mutex. Call once in setup() before xTaskCreatePinnedToCore.
void displayInit();

// Switch display 180° rotation. Must be called with mutex held or before irTask.
void setDisplayFlip(bool flip);

void setScreenText(const char* line1, const char* line2);

// ─── Render functions (Core 1 only, hold mutex) ───────────────────────────────

// Route to active render mode; acquires + releases displayMutex.
void renderScreen();

// Called during setup() before irTask; no mutex needed.
void bootPrint(const char* msg);


// ─── Drawing helpers ─────────────────────────────────────────────────────────

void drawXBM(int x, int y, int w, int h, const uint8_t* bits);
void drawStringCharWrap(int x, int y, int maxWidth, int lineHeight, const char* str);

