#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeUID.cpp"
// BadgeUID.cpp — ESP32-S3 eFuse UID read and hex conversion

#include "BadgeUID.h"
#include "BadgeDisplay.h"
#include "graphics.h"

#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_err.h"

uint8_t uid[UID_SIZE];
char    uid_hex[UID_SIZE * 2 + 1];

static void uid_to_hex();  // forward declaration

void read_uid() {
  esp_err_t err = esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, uid, UID_SIZE * 8);
  if (err != ESP_OK) {
    Serial.println("eFuse UID read failed");
    u8g2.begin();
    u8g2.clearBuffer();
    drawXBM(0, 0, Graphics_Base_width, Graphics_Base_height, Graphics_Base_bits);
    u8g2.setFont(u8g2_font_6x10_tf);
    drawStringCharWrap(0, 7, 128, 10, "UID read failed. Please reboot.");
    u8g2.sendBuffer();
    while (true) delay(1000);
  }
  uid_to_hex();
}

static void uid_to_hex() {
  const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 6; i++) {
    uid_hex[i * 2]     = hex[(uid[i] >> 4)];
    uid_hex[i * 2 + 1] = hex[uid[i] & 0xF];
  }
  uid_hex[12] = '\0';
}
