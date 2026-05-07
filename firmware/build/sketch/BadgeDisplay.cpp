#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeDisplay.cpp"
// BadgeDisplay.cpp — U8G2 display, render functions, modal system

#include "BadgeDisplay.h"
#include "BadgeConfig.h"
#include "BadgeIR.h"
#include "BadgeInput.h"
#include "BadgeMenu.h"
#include "BadgePairing.h"
#include "graphics.h"

// ─── Globals (declared extern in BadgeDisplay.h) ──────────────────────────────
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R2, /* reset= */ U8X8_PIN_NONE);
SemaphoreHandle_t displayMutex = nullptr;

String screenLine1 = "";
String screenLine2 = "";
bool screenDirty = false;
RenderMode renderMode = MODE_BOOT;
unsigned long inputTestLastActivity = 0;
unsigned long boopResultShownAt = 0;

// ─── Display control ─────────────────────────────────────────────────────────

void displayInit()
{
  displayMutex = xSemaphoreCreateMutex();
}

static bool displayFlipped = false;

void setDisplayFlip(bool flip)
{
  if (flip == displayFlipped)
    return;
  displayFlipped = flip;
  u8g2.setDisplayRotation(flip ? U8G2_R0 : U8G2_R2);
}

void setScreenText(const char* line1, const char* line2)
{
  screenLine1 = line1;
  screenLine2 = line2;
  screenDirty = true;
}

// ─── Drawing helpers ─────────────────────────────────────────────────────────

void drawXBM(int x, int y, int w, int h, const uint8_t *bits)
{
  u8g2.drawXBMP(x, y, w, h, bits);
}

void drawStringCharWrap(int x, int y, int maxWidth, int lineHeight, const char* str)
{
  char line[128];
  int lineLen = 0;
  line[0] = '\0';
  u8g2.setFont(u8g2_font_6x10_tf);
  for (int i = 0; str[i] != '\0'; i++)
  {
    char test[129];
    memcpy(test, line, lineLen);
    test[lineLen]     = str[i];
    test[lineLen + 1] = '\0';
    if ((int)u8g2.getStrWidth(test) > maxWidth)
    {
      u8g2.drawStr(x, y + lineHeight, line);
      y += lineHeight;
      line[0] = str[i];
      line[1] = '\0';
      lineLen  = 1;
    }
    else
    {
      line[lineLen]     = str[i];
      line[++lineLen]   = '\0';
    }
  }
  if (lineLen > 0)
    u8g2.drawStr(x, y + lineHeight, line);
}

// ─── Boot render ──────────────────────────────────────────────────────────────

void bootPrint(const char* msg) {
  Serial.println(msg);
  screenLine1 = msg;
  u8g2.clearBuffer();
  drawXBM(0, 3, Temporal_Logo_width, Temporal_Logo_height, Temporal_Logo_bits);
  u8g2.setFont(u8g2_font_4x6_tf);
  u8g2.drawStr(0, 62, msg);
  u8g2.sendBuffer();
}

static void renderBoot() {
  u8g2.clearBuffer();
  drawXBM(0, 3, Temporal_Logo_width, Temporal_Logo_height, Temporal_Logo_bits);
  u8g2.setFont(u8g2_font_4x6_tf);
  u8g2.drawStr(0, 62, screenLine1.c_str());
  u8g2.sendBuffer();
}

// ─── QR render ───────────────────────────────────────────────────────────────

// QR globals owned by the main sketch; declared extern here.
extern uint8_t *qrBits;
extern int      qrByteCount;
extern int      qrWidth;
extern int      qrHeight;
extern char uid_hex[];

static void renderQR()
{
  u8g2.clearBuffer();
  if (qrBits && qrWidth > 0 && qrHeight > 0) {
    int x = ((128 - qrWidth) / 2) & ~7;
    int y = (64 - qrHeight) / 2;
    if (y < 0) y = 0;
    u8g2.drawXBMP(x, y, qrWidth, qrHeight, qrBits);
  } else {
    u8g2.setFont(u8g2_font_4x6_tf);
    int statusX = 128 - (int)u8g2.getStrWidth(qrPollStatus);
    u8g2.drawStr(statusX, 6, qrPollStatus);
  }
  u8g2.sendBuffer();
}

// ─── Main render ─────────────────────────────────────────────────────────────

// Badge identity globals owned by the main sketch
extern BadgeState badgeState;
extern char badgeName[];
extern char badgeTitle[];
extern char badgeCompany[];
extern char badgeAtType[];

// ─── Nametag render ───────────────────────────────────────────────────────────
static void renderNametag() {
  setDisplayFlip(true);
  u8g2.clearBuffer();

  // Attendee type label (top, small)
  if (badgeAtType[0]) {
    u8g2.setFont(u8g2_font_4x6_tf);
    int w = u8g2.getStrWidth(badgeAtType);
    u8g2.drawStr((128 - w) / 2, 7, badgeAtType);
  }

  // Name (large, center)
  if (badgeName[0]) {
    u8g2.setFont(u8g2_font_logisoso16_tf);
    int w = u8g2.getStrWidth(badgeName);
    // Scale down to 6x10 if name is too wide
    if (w > 124) {
      u8g2.setFont(u8g2_font_6x10_tf);
      w = u8g2.getStrWidth(badgeName);
    }
    u8g2.drawStr((128 - w) / 2, 32, badgeName);
  }

  // Title
  if (badgeTitle[0]) {
    u8g2.setFont(u8g2_font_6x10_tf);
    int w = u8g2.getStrWidth(badgeTitle);
    u8g2.drawStr((128 - w) / 2, 46, badgeTitle);
  }

  // Company
  if (badgeCompany[0]) {
    u8g2.setFont(u8g2_font_4x6_tf);
    int w = u8g2.getStrWidth(badgeCompany);
    u8g2.drawStr((128 - w) / 2, 58, badgeCompany);
  }

  u8g2.sendBuffer();
}

// ─── IR arrow helper ──────────────────────────────────────────────────────────
// Draws one IR arrow. up=TX (tip at top), down=RX (tip at bottom).
// filled=active state, outline=idle. Shaft 6×14 + head 18×10 = 24px tall total.
static void drawIrArrow(int cx, int cy, bool up, bool filled)
{
  const int SW = 6;   // shaft width
  const int SH = 14;  // shaft height
  const int HW = 18;  // arrowhead base width
  const int HH = 10;  // arrowhead height
  const int H  = SH + HH;
  int top = cy - H / 2;

  if (up) {
    // Tip at top, shaft at bottom
    if (filled) {
      for (int i = 0; i < HH; i++) {
        int w = 2 + (HW - 2) * i / (HH - 1);
        u8g2.drawHLine(cx - w / 2, top + i, w);
      }
      u8g2.drawBox(cx - SW / 2, top + HH, SW, SH);
    } else {
      u8g2.drawLine(cx, top, cx - HW / 2, top + HH - 1);
      u8g2.drawLine(cx, top, cx + HW / 2, top + HH - 1);
      u8g2.drawVLine(cx - SW / 2, top + HH, SH);
      u8g2.drawVLine(cx + SW / 2, top + HH, SH);
      u8g2.drawHLine(cx - SW / 2, top + H - 1, SW + 1);
    }
  } else {
    // Shaft at top, tip at bottom
    if (filled) {
      u8g2.drawBox(cx - SW / 2, top, SW, SH);
      for (int i = 0; i < HH; i++) {
        int w = HW - (HW - 2) * i / (HH - 1);
        u8g2.drawHLine(cx - w / 2, top + SH + i, w);
      }
    } else {
      u8g2.drawVLine(cx - SW / 2, top, SH);
      u8g2.drawVLine(cx + SW / 2, top, SH);
      u8g2.drawHLine(cx - SW / 2, top, SW + 1);
      u8g2.drawLine(cx - HW / 2, top + SH, cx, top + H - 1);
      u8g2.drawLine(cx + HW / 2, top + SH, cx, top + H - 1);
    }
  }
}

static void renderBoop()
{
  setDisplayFlip(false);
  u8g2.clearBuffer();

  IrPhase phase = irStatus.phase;
  bool arrowRX  = false;
  bool arrowTX  = false;
  const char* statusA = nullptr;
  const char* statusB = nullptr;

  switch (phase)
  {
  case IR_IDLE:
    statusA = "Press UP to boop";
    break;
  case IR_SYN_SENT:
    arrowTX = true;
    statusA = "Seeking...";
    statusB = irStatus.statusMsg[0] ? irStatus.statusMsg : nullptr;
    break;
  case IR_SYN_RECEIVED:
    arrowRX = true;
    statusA = "Connecting...";
    statusB = irStatus.statusMsg[0] ? irStatus.statusMsg : nullptr;
    break;
  case IR_ESTABLISHED:
    arrowRX = true;
    arrowTX = true;
    statusA = "Connected";
    statusB = (irStatus.role == IR_INITIATOR) ? "INIT" : "RESP";
    break;
  case IR_TX_UID:
    arrowTX = true;
    statusA = "Sending UID...";
    statusB = irStatus.statusMsg[0] ? irStatus.statusMsg : nullptr;
    break;
  case IR_RX_UID:
    arrowRX = true;
    statusA = "Receiving...";
    statusB = irStatus.peerUidHex[0] ? irStatus.peerUidHex : nullptr;
    break;
  case IR_PAIR_CONSENT:
    statusA = "Booping...";
    statusB = irStatus.peerUID[0] ? irStatus.peerUID : nullptr;
    break;
  case IR_PAIRED_OK:
  case IR_PAIR_FAILED:
  case IR_PAIR_CANCELLED:
    // Terminal — loop() will switch to MODE_BOOP_RESULT on next iteration;
    // show nothing here to avoid a one-frame flash of stale status.
    break;
  }

  // ── Arrows: RX (down) left, TX (up) right — horizontally centered pair ──
  // cy=24 → arrows span y 12–36, leaving room for two text rows below.
  drawIrArrow(46, 24, false, arrowRX);
  drawIrArrow(82, 24, true,  arrowTX);

  // ── Status text: centered below arrows ──────────────────────────────────
  if (statusA)
  {
    u8g2.setFont(u8g2_font_6x10_tf);
    int w = (int)u8g2.getStrWidth(statusA);
    u8g2.drawStr((128 - w) / 2, 46, statusA);
    if (statusB)
    {
      w = (int)u8g2.getStrWidth(statusB);
      u8g2.drawStr((128 - w) / 2, 58, statusB);
    }
  }

  u8g2.sendBuffer();
}

// ─── Input test render ───────────────────────────────────────────────────────

static void renderInputTest()
{
  if (inputTestLastActivity == 0)
    inputTestLastActivity = millis();

  // Reset countdown on deliberate input (buttons or tilt only — joystick excluded to avoid drift)
  static uint8_t prevBtns = 0;
  static bool prevTilt = HIGH;

  uint8_t curBtns = 0;
  for (int i = 0; i < NUM_BUTTONS; i++)
    if (buttons[i].state == LOW)
      curBtns |= (1 << i);
  if (curBtns != prevBtns || tiltState != prevTilt)
  {
    prevBtns = curBtns;
    prevTilt = tiltState;
    inputTestLastActivity = millis();
  }

  // 5-second inactivity timeout → back to menu
  unsigned long elapsed = millis() - inputTestLastActivity;
  if (elapsed >= 5000)
  {
    inputTestLastActivity = 0;
    renderMode = MODE_MENU;
    screenDirty = true;
    return;
  }
  int secsLeft = (int)((5000 - elapsed + 999) / 1000);

  setDisplayFlip(false);
  u8g2.clearBuffer();
  drawXBM(0, 0, Graphics_Base_width, Graphics_Base_height, Graphics_Base_bits);

  // Original joystick dot and tilt indicator
  u8g2.drawBox(joySquareX - 1, joySquareY - 1, 3, 3);
  if (tiltState == HIGH)
    u8g2.drawBox(84, 48, 4, 5);
  else
    u8g2.drawBox(84, 54, 4, 5);

  // Button indicators — 3×3 dot at each button's XBM indicator position
  for (int i = 0; i < NUM_BUTTONS; i++)
  {
    if (buttons[i].state == LOW)
      u8g2.drawBox(buttons[i].indicatorX - 1, buttons[i].indicatorY - 1, 3, 3);
  }

  // Countdown — large digit centered on screen
  char buf[4];
  snprintf(buf, sizeof(buf), "%d", secsLeft);
  u8g2.setFont(u8g2_font_logisoso32_tf);
  int tw = (int)u8g2.getStrWidth(buf);
  u8g2.setDrawColor(0);
  u8g2.drawBox(64 - tw / 2 - 2, 16, tw + 4, 34);
  u8g2.setDrawColor(1);
  u8g2.drawStr(64 - tw / 2, 48, buf);

  u8g2.sendBuffer();
}

// ─── Boop result render ───────────────────────────────────────────────────────

// Declared extern in BadgeIR.h (use forward reference here to avoid circular include)
extern IrStatus irStatus;

static void renderBoopResult()
{
  if (boopResultShownAt == 0) boopResultShownAt = millis();

  // 10-second auto-return to menu
  if (millis() - boopResultShownAt >= 10000) {
    boopResultShownAt = 0;
    renderMode = MODE_MENU;
    screenDirty = true;
    return;
  }

  setDisplayFlip(false);
  u8g2.clearBuffer();

  IrPhase phase = irStatus.phase;

  if (phase == IR_PAIRED_OK) {
    // Success: "Booped with <name>"
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "Booped with");

    const char* name = irStatus.peerName[0] ? irStatus.peerName : irStatus.peerUID;
    u8g2.setFont(u8g2_font_logisoso16_tf);
    int w = (int)u8g2.getStrWidth(name);
    if (w > 124) {
      u8g2.setFont(u8g2_font_6x10_tf);
      w = (int)u8g2.getStrWidth(name);
    }
    u8g2.drawStr((128 - w) / 2, 32, name);
  } else {
    // Failure / cancelled / unexpected — header only
    const char* header =
      (phase == IR_PAIR_FAILED)    ? "Boop failed" :
      (phase == IR_PAIR_CANCELLED) ? "Boop cancelled" :
                                     "Boop error";
    u8g2.setFont(u8g2_font_6x10_tf);
    int hw = (int)u8g2.getStrWidth(header);
    u8g2.drawStr((128 - hw) / 2, 28, header);
  }

  u8g2.drawHLine(0, 38, 128);

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(4, 52, "< New Boop");
  const char* menuLabel = "Menu >";
  int mw = (int)u8g2.getStrWidth(menuLabel);
  u8g2.drawStr(128 - mw - 4, 52, menuLabel);

  u8g2.sendBuffer();
}

// ─── renderScreen ────────────────────────────────────────────────────────────

void renderScreen()
{
  DISPLAY_TAKE();
  screenDirty = false;
  if (TILT_SHOWS_BADGE && tiltNametagActive && badgeName[0] != '\0') {
    renderNametag();
    DISPLAY_GIVE();
    return;
  }
  switch (renderMode)
  {
  case MODE_BOOT:
    renderBoot();
    break;
  case MODE_QR:
    renderQR();
    break;
  case MODE_BOOP:
    renderBoop();
    break;
  case MODE_MENU:
    renderMenu();
    break;
  case MODE_INPUT_TEST:
    renderInputTest();
    break;
  case MODE_BOOP_RESULT:
    renderBoopResult();
    break;
  }
  DISPLAY_GIVE();
}

