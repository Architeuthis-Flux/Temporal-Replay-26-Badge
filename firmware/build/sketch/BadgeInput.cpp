#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeInput.cpp"
// BadgeInput.cpp — Button polling, joystick, tilt switch

#include "BadgeInput.h"
#include "BadgeConfig.h"
#include "BadgeIR.h"
#include "BadgeDisplay.h"
#include "BadgeMenu.h"
#include "BadgePairing.h"

extern char badgeName[];
extern unsigned long boopResultShownAt;
extern BadgeState badgeState;

// ─── Button array (declared extern in BadgeInput.h) ───────────────────────────
Button buttons[] = {
    {BTN_UP, HIGH, HIGH, 0, 118, 48},
    {BTN_DOWN, HIGH, HIGH, 0, 118, 58},
    {BTN_LEFT, HIGH, HIGH, 0, 113, 53},
    {BTN_RIGHT, HIGH, HIGH, 0, 123, 53},
};
const int NUM_BUTTONS = 4;

// ─── Tilt state ───────────────────────────────────────────────────────────────
bool tiltState = HIGH;
bool tiltNametagActive = false;

// ─── Joystick state ───────────────────────────────────────────────────────────
int joySquareX = JOY_CIRCLE_CX;
int joySquareY = JOY_CIRCLE_CY;

// ─── Flags consumed by loop() ─────────────────────────────────────────────────
volatile bool pairingCheckRequested = false;
volatile bool pythonMenuRequested   = false;
volatile bool messagesRequested     = false; // spec-009: set when MENU_MESSAGES selected

// ─── onButtonPressed ──────────────────────────────────────────────────────────
static void onButtonPressed(int index)
{
  // Always let any button cancel an active pairing consent
  if (irStatus.phase == IR_PAIR_CONSENT)
  {
    pairingCancelRequested = true;
    Serial.println("Pairing cancel requested");
    return;
  }

  // In boop result mode: left = new boop, anything else = menu
  if (renderMode == MODE_BOOP_RESULT)
  {
    boopResultShownAt = 0;
    if (index == 2)
    { // BTN_LEFT — go back to boop screen
      renderMode = MODE_BOOP;
      irHardwareEnabled = true;
    }
    else
    {
      renderMode = MODE_MENU;
      irHardwareEnabled = false;
    }
    screenDirty = true;
    return;
  }

  // In input test mode, buttons only reset the inactivity timer
  if (renderMode == MODE_INPUT_TEST)
  {
    inputTestLastActivity = millis();
    screenDirty = true;
    return;
  }

  switch (index)
  {
  case 0: // BTN_UP — edge trigger for boop when in boop screen, otherwise go to menu
    if (renderMode == MODE_BOOP)
    {
      // Edge trigger: consumed by irTask on SYN fire; guarded by irExchangeActive
      if (!irExchangeActive) {
        boopEngaged = true;
      }
      Serial.println("BTN_UP: boop engaged");
    }
    else
    {
      renderMode = MODE_MENU;
      irHardwareEnabled = false;
      screenDirty = true;
    }
    break;

  case 1: // BTN_DOWN — QR/menu navigation only; boop is UP-only now
    if (renderMode == MODE_QR && qrPairingActive)
    {
      qrCheckRequested = true;
      Serial.println("BTN_DOWN: QR check requested");
    }
    else if (renderMode == MODE_MENU)
    {
      switch (menuIndex)
      {
      case MENU_BOOP: // 0
        if (badgeState != BADGE_PAIRED)
        {
          // Not enrolled — redirect to QR screen with explanation
          strncpy(qrPollStatus, "Enroll to boop!", 23);
          renderMode = MODE_QR;
          irHardwareEnabled = false;
          screenDirty = true;
          Serial.println("Menu: Boop blocked — not enrolled");
        }
        else
        {
          renderMode = MODE_BOOP;
          irHardwareEnabled = true;
          screenDirty = true;
          Serial.println("Menu: Boop");
        }
        break;
      case MENU_MESSAGES: // 1 — spec-009: open Messages screen
        messagesRequested = true;
        Serial.println("Menu: Messages");
        break;
      case MENU_QR_PAIR: // 2
        pairingCheckRequested = true;
        Serial.println("Menu: QR/Pair");
        break;
      case MENU_INPUT_TEST: // 3
        renderMode = MODE_INPUT_TEST;
        inputTestLastActivity = millis();
        screenDirty = true;
        Serial.println("Menu: Input Test");
        break;
      case MENU_APPS: // 4
        pythonMenuRequested = true;
        Serial.println("Menu: Apps");
        break;
      }
    }
    break;

  case 2: // BTN_LEFT — shortcut: toggle QR
    renderMode = (renderMode == MODE_QR) ? MODE_MENU : MODE_QR;
    irHardwareEnabled = false;
    screenDirty = true;
    break;

  case 3: // BTN_RIGHT — cancel / back to menu
    if (qrPairingActive)
    {
      qrPairingActive = false;
    }
    // Post-handshake exchange: ignore BTN_RIGHT until result screen (T020)
    if (irExchangeActive)
    {
      Serial.println("BTN_RIGHT: ignored (exchange active)");
      break;
    }
    pairingCancelRequested = true;
    // Pre-handshake (SYN_SENT/SYN_RECEIVED): let irTask send RST and signal boopTaskDone;
    // loop() will then switch to MODE_BOOP_RESULT. Don't switch mode here.
    if (renderMode == MODE_BOOP &&
        (irStatus.phase == IR_SYN_SENT || irStatus.phase == IR_SYN_RECEIVED))
    {
      Serial.println("BTN_RIGHT: pre-handshake cancel requested");
      break;
    }
    renderMode = MODE_MENU;
    irHardwareEnabled = false;
    screenDirty = true;
    Serial.println("BTN_RIGHT: back to menu");
    break;

  default:
    break;
  }
}

// ─── waitButtonRelease ────────────────────────────────────────────────────────
void waitButtonRelease(int pin, int delayMs) {
  while (digitalRead(pin) == LOW) delay(delayMs);
}

// ─── pollButtons ─────────────────────────────────────────────────────────────
static const unsigned long DEBOUNCE_DELAY = 40;

void pollButtons()
{
  for (int i = 0; i < NUM_BUTTONS; i++)
  {
    bool reading = digitalRead(buttons[i].pin);
    if (reading != buttons[i].lastReading)
    {
      buttons[i].lastDebounceTime = millis();
    }
    if ((millis() - buttons[i].lastDebounceTime) > DEBOUNCE_DELAY)
    {
      if (reading != buttons[i].state)
      {
        buttons[i].state = reading;
        screenDirty = true;
        if (buttons[i].state == LOW)
        {
          onButtonPressed(i);
        }
      }
    }
    buttons[i].lastReading = reading;
  }

}

// ─── pollJoystick ─────────────────────────────────────────────────────────────
static unsigned long lastJoyPoll = 0;
static const unsigned long JOY_INTERVAL = 50;

void pollJoystick()
{
  if (millis() - lastJoyPoll > JOY_INTERVAL)
  {
    int rawX = analogRead(JOY_X);
    int rawY = analogRead(JOY_Y);

    float nx = (rawX / 2047.5f) - 1.0f;
    float ny = (rawY / 2047.5f) - 1.0f;
    if (fabsf(nx) < JOY_DEADBAND)
      nx = 0.0f;
    if (fabsf(ny) < JOY_DEADBAND)
      ny = 0.0f;

    // Menu navigation: delegate to BadgeMenu
    if (renderMode == MODE_MENU)
    {
      menuHandleJoystick(ny);
    }

    // Joystick dot position (used on main screen)
    float px = nx * JOY_CIRCLE_R;
    float py = ny * JOY_CIRCLE_R;
    float dist = sqrtf(px * px + py * py);
    if (dist > JOY_CIRCLE_R)
    {
      px = px / dist * JOY_CIRCLE_R;
      py = py / dist * JOY_CIRCLE_R;
    }

    int newX = JOY_CIRCLE_CX + (int)roundf(px);
    int newY = JOY_CIRCLE_CY + (int)roundf(py);
    if (newX != joySquareX || newY != joySquareY)
    {
      joySquareX = newX;
      joySquareY = newY;
      if (renderMode == MODE_BOOP || renderMode == MODE_INPUT_TEST)
      {
        screenDirty = true;
      }
    }
    lastJoyPoll = millis();
  }
}

// ─── tiltFadeTransition ───────────────────────────────────────────────────────
void tiltFadeTransition()
{
  for (int c = 255; c >= 0; c -= 17)
  {
    u8g2.setContrast((uint8_t)c);
    delay(12);
  }
  u8g2.setContrast(0);
  renderScreen();
  for (int c = 0; c <= 255; c += 17)
  {
    u8g2.setContrast((uint8_t)c);
    delay(12);
  }
  u8g2.setContrast(255);
}

// ─── pollTilt ─────────────────────────────────────────────────────────────────
static unsigned long tiltLastChange = 0;
static bool tiltRawPrev = HIGH;
static const unsigned long TILT_DEBOUNCE_MS = 250;

void pollTilt()
{
  // FR-004: tilt-to-nametag must not activate outside the menu screen.
  // Also deactivates any live nametag when the user switches screens.
  if (renderMode != MODE_MENU) {
    tiltNametagActive = false;
    return;
  }

  bool raw = digitalRead(TILT_PIN);

  // Debounce: only commit a state change after the pin is stable for TILT_DEBOUNCE_MS
  if (raw != tiltRawPrev)
  {
    tiltLastChange = millis();
    tiltRawPrev = raw;
  }

  if ((millis() - tiltLastChange) < TILT_DEBOUNCE_MS)
    return;

  // Pin is stable — only act on state changes
  if (raw == tiltState)
    return;
  tiltState = raw;
  screenDirty = true;
  Serial.printf("Tilt: %s renderMode=%d nametagActive=%d badgeName='%s'\n",
                raw == HIGH ? "HIGH" : "LOW",
                (int)renderMode, (int)tiltNametagActive, badgeName);

  if (tiltNametagActive)
  {
    if (raw == LOW)
    {
      tiltNametagActive = false;
      setDisplayFlip(false);
      screenDirty = true;
      Serial.println("Tilt: nametag deactivated");
    }
  }
  else
  {
    if (raw == HIGH)
    {
      if (renderMode == MODE_MENU && badgeName[0] != '\0')
      {
        tiltNametagActive = true;
        screenDirty = true;
        Serial.println("Tilt: nametag activated");
      }
    }
  }
}
