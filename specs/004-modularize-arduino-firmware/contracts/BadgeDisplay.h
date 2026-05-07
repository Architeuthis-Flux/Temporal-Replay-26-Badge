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

// ─── Display instance (accessible for advanced use; prefer render functions) ──
extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
extern SemaphoreHandle_t displayMutex;

#define DISPLAY_TAKE()  xSemaphoreTake(displayMutex, portMAX_DELAY)
#define DISPLAY_GIVE()  xSemaphoreGive(displayMutex)

// ─── Screen state (shared with main sketch and BadgeIR) ──────────────────────
extern String screenLine1;
extern String screenLine2;
extern bool   screenDirty;
extern bool   modalActive;

enum RenderMode { MODE_BOOT, MODE_QR, MODE_MAIN };
extern RenderMode renderMode;

// ─── Display control ─────────────────────────────────────────────────────────

// Initialize display mutex. Call once in setup() before xTaskCreatePinnedToCore.
void displayInit();

// Switch display 180° rotation. Must be called with mutex held or before irTask.
void setDisplayFlip(bool flip);

void setScreenText(const String& line1, const String& line2);

// ─── Render functions (Core 1 only, hold mutex) ───────────────────────────────

// Route to active render mode; acquires + releases displayMutex.
void renderScreen();

// Individual mode renderers (called via renderScreen; do NOT call directly from other cores).
void renderBoot();
void renderQR();
void renderMain();

// Called during setup() before irTask; no mutex needed.
void bootPrint(const String& msg);

// ─── Modal system (called from Core 0 / irTask) ──────────────────────────────

// Blocking modal — returns 0 for left button, 1 for right button.
// Sets modalActive=true for duration; Core 1 loop() skips renderScreen while true.
int showModal(const String& message, const String& leftLabel, const String& rightLabel);

// ─── Drawing helpers ─────────────────────────────────────────────────────────

void drawXBM(int x, int y, int w, int h, const uint8_t* bits);
void drawStringCharWrap(int x, int y, int maxWidth, int lineHeight, const String& str);
