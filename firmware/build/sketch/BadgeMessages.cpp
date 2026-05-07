#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeMessages.cpp"
// BadgeMessages.cpp — Messages screen: emoji ping state machine
//
// Blocking state machine entered from loop() via handleMessages().
// Loads boops (contacts), displays a scrollable contact list, lets user
// pick an emoji, sends a ping, and polls for incoming pings.

#include "BadgeMessages.h"
#include "BadgeConfig.h"
#include "BadgeDisplay.h"
#include "BadgeInput.h"
#include "BadgeMenu.h"
#include "BadgeAPI.h"
#include "BadgeStorage.h"
#include "BadgeUID.h"
#include <ArduinoJson.h>
#include <WiFi.h>

// ─── Emoji table ──────────────────────────────────────────────────────────────

static const char* EMOJI_GLYPHS[8]    = { "\xe2\x99\xa5", "\xe2\x98\x85", "\xe2\x9c\x93", "\xe2\x9a\xa1", "\xe2\x98\xba", "!",  "?",  "\xe2\x80\xa6" };
static const char* EMOJI_FALLBACK[8]  = { "<3",            "*",            "ok",           "zap",           "hi",           "!!", "??", "..." };
static const int   EMOJI_COUNT        = 8;

// ─── State machine ────────────────────────────────────────────────────────────

enum MessagesState {
  MSG_LOADING,
  MSG_CONTACT_LIST,
  MSG_EMOJI_PALETTE,
  MSG_SENDING,
  MSG_SENT,
  MSG_ERROR,
  MSG_INCOMING,
};

void handleMessages() {
  // ── PSRAM-backed large buffers (allocated once on first call) ────────────────
  // Moved from .bss to PSRAM to free ~27 KB of internal RAM.
  // Array references below preserve all existing access syntax unchanged.
  struct _MsgBufs {
    BoopRecord     boops        [BADGE_BOOPS_MAX_RECORDS];
    char           partnerNames [BADGE_BOOPS_MAX_RECORDS][BADGE_FIELD_NAME_MAX];
    char           partnerCompanies[BADGE_BOOPS_MAX_RECORDS][BADGE_FIELD_NAME_MAX];
    char           partnerTickets[BADGE_BOOPS_MAX_RECORDS][BADGE_UUID_MAX];
    int            lastEmojiIdx [BADGE_BOOPS_MAX_RECORDS];
    GetBoopsResult gb;
    GetPingsResult recentPings;
    GetPingsResult gp;
  };
  static _MsgBufs* _ps = nullptr;
  if (!_ps) {
    _ps = (_MsgBufs*)ps_malloc(sizeof(_MsgBufs));
    if (!_ps) {
      Serial.println("[BadgeMessages] PSRAM alloc failed");
      setScreenText("Messages", "Out of memory");
      renderScreen();
      delay(2000);
      return;
    }
  }
  BoopRecord      (&boops)          [BADGE_BOOPS_MAX_RECORDS]                        = _ps->boops;
  char            (&partnerNames)   [BADGE_BOOPS_MAX_RECORDS][BADGE_FIELD_NAME_MAX]  = _ps->partnerNames;
  char            (&partnerCompanies)[BADGE_BOOPS_MAX_RECORDS][BADGE_FIELD_NAME_MAX] = _ps->partnerCompanies;
  char            (&partnerTickets) [BADGE_BOOPS_MAX_RECORDS][BADGE_UUID_MAX]        = _ps->partnerTickets;
  int             (&lastEmojiIdx)   [BADGE_BOOPS_MAX_RECORDS]                        = _ps->lastEmojiIdx;
  GetBoopsResult& gb          = _ps->gb;
  GetPingsResult& recentPings = _ps->recentPings;
  GetPingsResult& gp          = _ps->gp;

  // ── Small state (internal RAM .bss) ──────────────────────────────────────────
  static int        boopCount       = 0;
  static int        selectedContact = 0;
  static int        selectedEmoji   = 0;
  static char       lastPingId [BADGE_UUID_MAX] = "";
  static char       lastPingTs [32]             = "";
  static uint32_t   lastPollMs    = 0;
  static MessagesState msgState   = MSG_LOADING;
  static unsigned long stateEnteredAt = 0;
  static int        lastErrCode   = 0;
  static char       incomingSender[64] = "";
  static int        incomingEmojiIdx   = 0;
  static unsigned long lastNavMs  = 0;

  // Reset on entry
  msgState        = MSG_LOADING;
  boopCount       = 0;
  selectedContact = 0;
  selectedEmoji   = 0;
  lastPingId[0]   = '\0';
  lastPingTs[0]   = '\0';
  lastPollMs      = 0;
  lastNavMs       = 0;
  stateEnteredAt  = millis();
  for (int i = 0; i < 50; i++) lastEmojiIdx[i] = -1;

  char myUUID  [BADGE_UUID_MAX] = "";
  char myTicket[BADGE_UUID_MAX] = "";
  BadgeStorage::loadMyTicketUUID(myTicket, sizeof(myTicket));
  strncpy(myUUID, uid_hex, sizeof(myUUID) - 1);

  waitButtonRelease(BTN_DOWN);
  delay(50);

  while (true) {
    // ── LOADING ──────────────────────────────────────────────────────────────
    if (msgState == MSG_LOADING) {
      DISPLAY_TAKE();
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(0, 12, "Messages");
      u8g2.setFont(u8g2_font_4x6_tf);
      u8g2.drawStr(0, 28, "Loading contacts...");
      u8g2.sendBuffer();
      DISPLAY_GIVE();

      Serial.printf("[Messages] WiFi=%d heap=%u myUUID='%s'\n",
                    (int)WiFi.status(), (unsigned)ESP.getFreeHeap(), myUUID);
      gb = BadgeAPI::getBoops(myUUID);
      Serial.printf("[Messages] getBoops: ok=%d http=%d count=%d\n",
                    gb.ok, gb.httpCode, gb.count);
      boopCount = 0;
      if (gb.ok) {
        for (int i = 0; i < gb.count && i < 50; i++) {
          boops[i] = gb.boops[i];

          // Partner ticket = whichever UUID in ticket_uuids is not ours
          const char* t0 = boops[i].ticket_uuids[0];
          const char* t1 = boops[i].ticket_uuids[1];
          const char* partnerTicket = (strcmp(t0, myTicket) == 0) ? t1 : t0;
          strncpy(partnerTickets[i], partnerTicket, BADGE_UUID_MAX - 1);
          partnerTickets[i][BADGE_UUID_MAX - 1] = '\0';

          BoopPartnerResult bp = BadgeAPI::getBoopPartner(boops[i].id, myUUID);
          if (bp.ok) {
            strncpy(partnerNames    [i], bp.partnerName,    BADGE_FIELD_NAME_MAX - 1); partnerNames    [i][BADGE_FIELD_NAME_MAX - 1] = '\0';
            strncpy(partnerCompanies[i], bp.partnerCompany, BADGE_FIELD_NAME_MAX - 1); partnerCompanies[i][BADGE_FIELD_NAME_MAX - 1] = '\0';
          } else {
            strncpy(partnerNames[i], "(unknown)", BADGE_FIELD_NAME_MAX - 1);
            partnerCompanies[i][0] = '\0';
          }
          boopCount++;
        }
      }

      // Populate per-contact last-emoji badges from recent ping history
      if (boopCount > 0 && myTicket[0] != '\0') {
        recentPings = BadgeAPI::getPings(myUUID, myTicket, PING_TYPE_EMOJI, 50, nullptr, nullptr);
        if (recentPings.ok) {
          for (int p = 0; p < recentPings.count; p++) {
            const char* src = recentPings.records[p].source_ticket_uuid;
            const char* tgt = recentPings.records[p].target_ticket_uuid;
            for (int i = 0; i < boopCount; i++) {
              if (lastEmojiIdx[i] >= 0) continue;
              if (strcmp(partnerTickets[i], src) == 0 ||
                  strcmp(partnerTickets[i], tgt) == 0) {
                DynamicJsonDocument ed(128);
                if (deserializeJson(ed, recentPings.records[p].data) == DeserializationError::Ok) {
                  const char* eg = ed["emoji"] | "";
                  for (int j = 0; j < EMOJI_COUNT; j++) {
                    if (strcmp(eg, EMOJI_GLYPHS[j]) == 0 ||
                        strcmp(eg, EMOJI_FALLBACK[j]) == 0) {
                      lastEmojiIdx[i] = j; break;
                    }
                  }
                }
                break;
              }
            }
          }
        }
      }

      msgState        = MSG_CONTACT_LIST;
      selectedContact = 0;
      stateEnteredAt  = millis();
      lastPollMs      = millis();
    }

    // ── CONTACT_LIST ─────────────────────────────────────────────────────────
    else if (msgState == MSG_CONTACT_LIST) {
      // Poll for incoming pings
      if (myTicket[0] != '\0' &&
          (uint32_t)(millis() - lastPollMs) >= MSG_POLL_INTERVAL_MS) {
        lastPollMs = millis();
        gp = BadgeAPI::getPings(myUUID, myTicket, PING_TYPE_EMOJI, 5, nullptr, nullptr);
        if (gp.ok && gp.count > 0 &&
            strcmp(gp.records[0].id, lastPingId) != 0) {
          strncpy(lastPingId, gp.records[0].id, BADGE_UUID_MAX - 1);
          lastPingId[BADGE_UUID_MAX - 1] = '\0';
          strncpy(lastPingTs, gp.records[0].created_at, 31);
          lastPingTs[31] = '\0';

          // Resolve sender name
          incomingSender[0] = '\0';
          for (int i = 0; i < boopCount; i++) {
            if (strcmp(partnerTickets[i], gp.records[0].source_ticket_uuid) == 0) {
              strncpy(incomingSender, partnerNames[i], BADGE_FIELD_NAME_MAX - 1);
              incomingSender[BADGE_FIELD_NAME_MAX - 1] = '\0';
              break;
            }
          }
          if (incomingSender[0] == '\0') {
            strncpy(incomingSender, gp.records[0].source_ticket_uuid, 12);
            incomingSender[12] = '\0';
          }

          // Parse emoji
          incomingEmojiIdx = 0;
          DynamicJsonDocument edoc(128);
          if (deserializeJson(edoc, gp.records[0].data) == DeserializationError::Ok) {
            const char* eg = edoc["emoji"] | "";
            for (int i = 0; i < EMOJI_COUNT; i++) {
              if (strcmp(eg, EMOJI_GLYPHS[i]) == 0 ||
                  strcmp(eg, EMOJI_FALLBACK[i]) == 0) {
                incomingEmojiIdx = i; break;
              }
            }
          }
          // Update last-emoji badge for this contact
          for (int i = 0; i < boopCount; i++) {
            if (strcmp(partnerTickets[i], gp.records[0].source_ticket_uuid) == 0) {
              lastEmojiIdx[i] = incomingEmojiIdx; break;
            }
          }
          msgState = MSG_INCOMING; stateEnteredAt = millis();
        }
      }

      // Render
      DISPLAY_TAKE();
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(0, 10, "Messages");
      if (boopCount == 0) {
        u8g2.setFont(u8g2_font_4x6_tf);
        u8g2.drawStr(0, 28, "No contacts yet");
      } else {
        const int ITEM_H  = 11;
        const int LIST_Y  = 14;
        int visStart = viewportStart(selectedContact, MENU_VISIBLE);
        for (int i = 0; i < MENU_VISIBLE && visStart + i < boopCount; i++) {
          int idx = visStart + i;
          int y   = LIST_Y + i * ITEM_H;
          if (idx == selectedContact) {
            u8g2.drawBox(0, y, 128, ITEM_H);
            u8g2.setDrawColor(0);
          }
          char line[26];
          if (lastEmojiIdx[idx] >= 0)
            snprintf(line, sizeof(line), "%-13.13s %s", partnerNames[idx], EMOJI_FALLBACK[lastEmojiIdx[idx]]);
          else
            snprintf(line, sizeof(line), "%.20s", partnerNames[idx]);
          u8g2.setFont(u8g2_font_5x7_tf);
          u8g2.drawStr(2, y + ITEM_H - 2, line);
          u8g2.setDrawColor(1);
        }
      }
      u8g2.setFont(u8g2_font_4x6_tf);
      u8g2.drawStr(0, 63, "v:send  >:back");
      u8g2.sendBuffer();
      DISPLAY_GIVE();

      // Input
      if (digitalRead(BTN_UP) == LOW) {
        waitButtonRelease(BTN_UP);
        if (selectedContact > 0) selectedContact--;
        lastNavMs = millis();
      }
      if (digitalRead(BTN_DOWN) == LOW) {
        waitButtonRelease(BTN_DOWN);
        if (boopCount > 0) {
          msgState = MSG_EMOJI_PALETTE; selectedEmoji = 0; stateEnteredAt = millis();
        }
      }
      if (digitalRead(BTN_RIGHT) == LOW) {
        waitButtonRelease(BTN_RIGHT);
        break;
      }
      // Joystick nav with repeat gate
      {
        float ny = (analogRead(JOY_Y) / 2047.5f) - 1.0f;
        if (fabsf(ny) > MENU_NAV_THRESHOLD &&
            (unsigned long)(millis() - lastNavMs) >= 150UL) {
          lastNavMs = millis();
          if      (ny < 0 && selectedContact > 0)            selectedContact--;
          else if (ny > 0 && selectedContact < boopCount - 1) selectedContact++;
        }
      }
      delay(10);
    }

    // ── EMOJI_PALETTE ─────────────────────────────────────────────────────────
    else if (msgState == MSG_EMOJI_PALETTE) {
      DISPLAY_TAKE();
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_5x7_tf);
      char toLine[32];
      snprintf(toLine, sizeof(toLine), "To: %.18s", partnerNames[selectedContact]);
      u8g2.drawStr(0, 8, toLine);
      const int ITEM_H = 11;
      const int LIST_Y = 12;
      int visStart = viewportStart(selectedEmoji, MENU_VISIBLE);
      for (int i = 0; i < MENU_VISIBLE && visStart + i < EMOJI_COUNT; i++) {
        int idx = visStart + i;
        int y   = LIST_Y + i * ITEM_H;
        if (idx == selectedEmoji) {
          u8g2.drawBox(0, y, 128, ITEM_H);
          u8g2.setDrawColor(0);
        }
        u8g2.drawStr(4, y + ITEM_H - 2, EMOJI_FALLBACK[idx]);
        u8g2.setDrawColor(1);
      }
      u8g2.setFont(u8g2_font_4x6_tf);
      u8g2.drawStr(0, 63, "v:send  >:back");
      u8g2.sendBuffer();
      DISPLAY_GIVE();

      if (digitalRead(BTN_UP) == LOW) {
        waitButtonRelease(BTN_UP);
        if (selectedEmoji > 0) selectedEmoji--;
        lastNavMs = millis();
      }
      if (digitalRead(BTN_DOWN) == LOW) {
        waitButtonRelease(BTN_DOWN);
        msgState = MSG_SENDING; stateEnteredAt = millis();
      }
      if (digitalRead(BTN_RIGHT) == LOW) {
        waitButtonRelease(BTN_RIGHT);
        msgState = MSG_CONTACT_LIST; stateEnteredAt = millis();
      }
      {
        float ny = (analogRead(JOY_Y) / 2047.5f) - 1.0f;
        if (fabsf(ny) > MENU_NAV_THRESHOLD &&
            (unsigned long)(millis() - lastNavMs) >= 150UL) {
          lastNavMs = millis();
          if      (ny < 0 && selectedEmoji > 0)              selectedEmoji--;
          else if (ny > 0 && selectedEmoji < EMOJI_COUNT - 1) selectedEmoji++;
        }
      }
      delay(10);
    }

    // ── SENDING ───────────────────────────────────────────────────────────────
    else if (msgState == MSG_SENDING) {
      DISPLAY_TAKE();
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(0, 20, "Sending...");
      u8g2.sendBuffer();
      DISPLAY_GIVE();

      char dataJson[64];
      snprintf(dataJson, sizeof(dataJson), "{\"emoji\":\"%s\"}", EMOJI_FALLBACK[selectedEmoji]);
      SendPingResult sp = BadgeAPI::sendPing(
          myUUID, partnerTickets[selectedContact], PING_TYPE_EMOJI, dataJson);

      lastErrCode = sp.httpCode;
      if (sp.ok) lastEmojiIdx[selectedContact] = selectedEmoji;
      msgState = sp.ok ? MSG_SENT : MSG_ERROR;
      stateEnteredAt = millis();
    }

    // ── SENT ──────────────────────────────────────────────────────────────────
    else if (msgState == MSG_SENT) {
      DISPLAY_TAKE();
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tf);
      char sentLine[32];
      snprintf(sentLine, sizeof(sentLine), "Sent! %s", EMOJI_FALLBACK[selectedEmoji]);
      u8g2.drawStr(0, 20, sentLine);
      u8g2.sendBuffer();
      DISPLAY_GIVE();

      if (millis() - stateEnteredAt >= 2000) {
        msgState = MSG_CONTACT_LIST; stateEnteredAt = millis(); lastPollMs = millis();
      }
      delay(50);
    }

    // ── ERROR ─────────────────────────────────────────────────────────────────
    else if (msgState == MSG_ERROR) {
      DISPLAY_TAKE();
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(0, 16, "Send failed");
      u8g2.setFont(u8g2_font_4x6_tf);
      char errLine[20];
      snprintf(errLine, sizeof(errLine), "HTTP %d", lastErrCode);
      u8g2.drawStr(0, 28, errLine);
      u8g2.sendBuffer();
      DISPLAY_GIVE();

      if (millis() - stateEnteredAt >= 3000) {
        msgState = MSG_CONTACT_LIST; stateEnteredAt = millis(); lastPollMs = millis();
      }
      delay(50);
    }

    // ── INCOMING ──────────────────────────────────────────────────────────────
    else if (msgState == MSG_INCOMING) {
      DISPLAY_TAKE();
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_5x7_tf);
      u8g2.drawStr(0, 10, incomingSender);
      u8g2.drawStr(0, 22, "sent you:");
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(0, 38, EMOJI_FALLBACK[incomingEmojiIdx]);
      u8g2.sendBuffer();
      DISPLAY_GIVE();

      if (millis() - stateEnteredAt >= 4000) {
        msgState = MSG_CONTACT_LIST; stateEnteredAt = millis(); lastPollMs = millis();
      }
      delay(50);
    }
  }

  renderMode  = MODE_MENU;
  screenDirty = true;
}
