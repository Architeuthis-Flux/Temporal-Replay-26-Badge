#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgePython_bridges.cpp"
// BadgePython_bridges.cpp — C++ implementations of badge_bridge_* functions
//
// Bridge functions cross the C/C++ boundary: called from badge_mp/*.c (C) and
// implemented here in C++ with access to Arduino/ESP32 libraries.
//
// Also provides required MicroPython HAL functions:
//   mp_hal_stdout_tx_strn  — routes Python print() output to Serial
//   mp_hal_ticks_ms        — utime.ticks_ms() → millis()
//   mp_hal_delay_ms        — utime.sleep_ms()  → vTaskDelay

#include "BadgeDisplay.h"
#include "BadgeConfig.h"
#include "BadgeUID.h"
#include "BadgeAPI.h"
#include "BadgeStorage.h"
#include "BadgeIR.h"

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Bridge declarations (C linkage)
#include "badge_mp/BadgePython_bridges.h"

// ─── MicroPython HAL — required by embed port ─────────────────────────────────

// Defined in BadgePython.cpp; set true when MicroPython emits a traceback line.
extern volatile bool _pyHadException;

extern "C" void mp_hal_stdout_tx_strn(const char *str, size_t len) {
    // Route Python print() and exception output to Serial.
    // Detect exception tracebacks so BadgePython::execApp can show an error screen.
    // MicroPython always starts traceback output with the literal string "Traceback".
    Serial.write((const uint8_t*)str, len);
    if (!_pyHadException && len >= 9 && strncmp(str, "Traceback", 9) == 0) {
        _pyHadException = true;
    }
}

extern "C" unsigned long mp_hal_ticks_ms(void) {
    return millis();
}

extern "C" unsigned long mp_hal_ticks_us(void) {
    return micros();
}

extern "C" unsigned long mp_hal_ticks_cpu(void) {
    return micros();
}

extern "C" uint64_t mp_hal_time_ns(void) {
    return (uint64_t)micros() * 1000ULL;
}

extern "C" void mp_hal_delay_ms(unsigned long ms) {
    // Use FreeRTOS delay to yield to other tasks (IR task on Core 0)
    vTaskDelay(ms / portTICK_PERIOD_MS);
}

extern "C" void mp_hal_delay_us(unsigned long us) {
    delayMicroseconds(us);
}

// Required by modmicropython.c when MICROPY_KBD_EXCEPTION=1.
// micropython.kbd_intr() is unused — escape is via FreeRTOS timer in BadgePython.cpp.
extern "C" void mp_hal_set_interrupt_char(int c) {
    (void)c;  // no-op
}

// ─── Display bridges ─────────────────────────────────────────────────────────

extern "C" void badge_bridge_display_clear(void) {
    DISPLAY_TAKE();
    u8g2.clearBuffer();
    DISPLAY_GIVE();
}

extern "C" void badge_bridge_display_text(const char* s, int x, int y) {
    DISPLAY_TAKE();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(x, y, s);
    DISPLAY_GIVE();
}

extern "C" void badge_bridge_display_show(void) {
    DISPLAY_TAKE();
    u8g2.sendBuffer();
    DISPLAY_GIVE();
}

// ─── Input bridges ────────────────────────────────────────────────────────────

extern "C" int badge_bridge_button_read(int pin) {
    // Buttons are active-low with pull-up; return 1 if pressed (LOW)
    return digitalRead(pin) == LOW ? 1 : 0;
}

extern "C" int badge_bridge_tilt_read(void) {
    return digitalRead(TILT_PIN) == LOW ? 1 : 0;
}

// ─── Backend probe bridge ─────────────────────────────────────────────────────

extern "C" bool BadgePython_probe_backend(void) {
    ProbeResult r = BadgeAPI::probeBadgeExistence(uid_hex);
    return r.ok;
}

// ─── IR bridges ───────────────────────────────────────────────────────────────

// badge_bridge_ir_send is implemented in BadgeIR.cpp (extern "C") to avoid
// including <IRremote.hpp> here, which would cause multiple-definition linker errors.

extern "C" void badge_bridge_ir_start(void) {
    pythonIrListening = true;
}

extern "C" void badge_bridge_ir_stop(void) {
    pythonIrListening = false;
    // Flush the Python receive queue
    taskENTER_CRITICAL(&irPythonQueueMux);
    irPythonQueueHead = 0;
    irPythonQueueTail = 0;
    taskEXIT_CRITICAL(&irPythonQueueMux);
}

extern "C" int badge_bridge_ir_available(void) {
    int result;
    taskENTER_CRITICAL(&irPythonQueueMux);
    result = (irPythonQueueHead != irPythonQueueTail) ? 1 : 0;
    taskEXIT_CRITICAL(&irPythonQueueMux);
    return result;
}

extern "C" int badge_bridge_ir_read(int* addr_out, int* cmd_out) {
    taskENTER_CRITICAL(&irPythonQueueMux);
    if (irPythonQueueHead == irPythonQueueTail) {
        taskEXIT_CRITICAL(&irPythonQueueMux);
        return -1;  // empty
    }
    *addr_out = irPythonQueue[irPythonQueueHead].addr;
    *cmd_out  = irPythonQueue[irPythonQueueHead].cmd;
    irPythonQueueHead = (irPythonQueueHead + 1) % IR_PYTHON_QUEUE_SIZE;
    taskEXIT_CRITICAL(&irPythonQueueMux);
    return 0;
}

// ─── HTTP bridges ─────────────────────────────────────────────────────────────

// _http_begin — configure HTTP client for the URL (handles HTTPS via setInsecure).
// Returns true if begin succeeded. Must be balanced with http.end().
static bool _http_begin(HTTPClient& http, WiFiClientSecure& tls, const char* url) {
    if (strncmp(url, "https://", 8) == 0) {
        tls.setInsecure();  // skip cert verification (no CA store on badge)
        http.begin(tls, url);
    } else {
        http.begin(url);
    }
    http.setTimeout(10000);  // 10-second read timeout
    return true;
}

extern "C" int badge_bridge_http_get(const char* url, char* buf, int buflen) {
    if (WiFi.status() != WL_CONNECTED) return -1;
    HTTPClient http;
    WiFiClientSecure tls;
    _http_begin(http, tls, url);
    int code = http.GET();
    if (code > 0) {
        String resp = http.getString();
        strncpy(buf, resp.c_str(), buflen - 1);
        buf[buflen - 1] = '\0';
    } else {
        buf[0] = '\0';
    }
    http.end();
    return code;
}

extern "C" int badge_bridge_http_post(const char* url, const char* body,
                                       char* buf, int buflen) {
    if (WiFi.status() != WL_CONNECTED) return -1;
    HTTPClient http;
    WiFiClientSecure tls;
    _http_begin(http, tls, url);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST((uint8_t*)body, strlen(body));
    if (code > 0) {
        String resp = http.getString();
        strncpy(buf, resp.c_str(), buflen - 1);
        buf[buflen - 1] = '\0';
    } else {
        buf[0] = '\0';
    }
    http.end();
    return code;
}

// ─── Identity bridges ─────────────────────────────────────────────────────────

extern "C" int badge_bridge_get_uid(char* buf, int buflen) {
    strncpy(buf, uid_hex, buflen - 1);
    buf[buflen - 1] = '\0';
    return 0;
}

extern "C" const char* badge_bridge_get_server_url(void) {
    return SERVER_URL;
}

// ─── spec-009: Identity extension bridges ────────────────────────────────────

extern "C" int badge_bridge_my_ticket_uuid(char* buf, int buflen) {
    // spec-009: Returns own ticket UUID from NVS; -1 if not stored.
    bool ok = BadgeStorage::loadMyTicketUUID(buf, (size_t)buflen);
    if (!ok) { buf[0] = '\0'; return -1; }
    return 0;
}

extern "C" int badge_bridge_boops(char* buf, int buflen) {
    // spec-009: Returns active pairings as JSON array.
    // [{"pairing_id":N,"ticket":"...","name":"...","company":"..."},...]
    //
    // Uses badge_bridge_http_get (fresh HTTPClient per call) instead of
    // BadgeAPI::getBoops / getBoopPartner (which share a static TLS client
    // that hangs when reused across FreeRTOS task contexts).

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[boops] WiFi not connected");
        strncpy(buf, "[]", buflen - 1); buf[buflen - 1] = '\0';
        return 0;
    }

    // ── Step 1: GET /api/v1/boops?badge_uuid=<uid> ────────────────────────────
    char url[256];
    static char* httpBuf = nullptr;  // allocated from PSRAM once
    if (!httpBuf) {
        httpBuf = (char*)ps_malloc(4096);
        if (!httpBuf) {
            strncpy(buf, "[]", buflen - 1); buf[buflen - 1] = '\0';
            return 0;
        }
    }
    snprintf(url, sizeof(url), "%s/api/v1/boops?badge_uuid=%s", SERVER_URL, uid_hex);
    Serial.printf("[boops] GET %s\n", url);
    int code = badge_bridge_http_get(url, httpBuf, 4096);
    Serial.printf("[boops] GET returned code=%d\n", code);
    if (code != 200) {
        strncpy(buf, "[]", buflen - 1); buf[buflen - 1] = '\0';
        return 0;
    }

    // ── Step 2: Parse pairings list — extract all data before issuing inner GETs.
    // ArduinoJson uses zero-copy deserialization from char* buffers: string fields
    // in doc point directly into httpBuf. The inner partner GETs (step 3) overwrite
    // httpBuf, which would corrupt those pointers. Pre-extract into a local struct
    // array so httpBuf is safe to reuse once the loop below begins.
    DynamicJsonDocument doc(8192);
    if (deserializeJson(doc, httpBuf) != DeserializationError::Ok) {
        strncpy(buf, "[]", buflen - 1); buf[buflen - 1] = '\0';
        return 0;
    }
    JsonArray pairings = doc["pairings"];
    if (!pairings) {
        strncpy(buf, "[]", buflen - 1); buf[buflen - 1] = '\0';
        return 0;
    }

    char myTicket[BADGE_UUID_MAX] = {};
    badge_bridge_my_ticket_uuid(myTicket, sizeof(myTicket));

    struct PairingEntry { int id; char partnerTicket[BADGE_UUID_MAX]; };
    PairingEntry entries[BADGE_BOOPS_MAX_RECORDS];
    int entryCount = 0;

    for (JsonObject p : pairings) {
        if (entryCount >= BADGE_BOOPS_MAX_RECORDS) break;
        const char* revokedAt = p["revoked_at"] | "";
        if (revokedAt[0] != '\0') continue;
        int id = p["id"] | 0;
        if (id == 0) continue;

        JsonArray tids = p["ticket_uuids"];
        const char* t0 = tids.size() > 0 ? (const char*)tids[0] : "";
        const char* t1 = tids.size() > 1 ? (const char*)tids[1] : "";
        const char* partnerTicket = (myTicket[0] != '\0' && strcmp(t0, myTicket) == 0) ? t1 : t0;

        entries[entryCount].id = id;
        strncpy(entries[entryCount].partnerTicket, partnerTicket, BADGE_UUID_MAX - 1);
        entries[entryCount].partnerTicket[BADGE_UUID_MAX - 1] = '\0';
        entryCount++;
    }
    // doc goes out of scope here — httpBuf is now safe to reuse for inner GETs.

    int pos = 0;
    pos += snprintf(buf + pos, buflen - pos, "[");
    int added = 0;

    for (int i = 0; i < entryCount; i++) {
        if (pos >= buflen - 8) break;

        // ── Step 3: GET /api/v1/boops/<id>/partner?badge_uuid=<uid> ─────────
        snprintf(url, sizeof(url), "%s/api/v1/boops/%d/partner?badge_uuid=%s",
                 SERVER_URL, entries[i].id, uid_hex);
        code = badge_bridge_http_get(url, httpBuf, 4096);

        const char* name    = "Unknown";
        const char* company = "";
        char nameBuf[64]    = {};
        char companyBuf[64] = {};
        if (code == 200) {
            DynamicJsonDocument pdoc(256);
            if (deserializeJson(pdoc, httpBuf) == DeserializationError::Ok) {
                const char* n = pdoc["partner_name"]    | "";
                const char* c = pdoc["partner_company"] | "";
                strncpy(nameBuf,    n, sizeof(nameBuf)    - 1);
                strncpy(companyBuf, c, sizeof(companyBuf) - 1);
                if (nameBuf[0])    name    = nameBuf;
                if (companyBuf[0]) company = companyBuf;
            }
        }

        if (added > 0) pos += snprintf(buf + pos, buflen - pos, ",");
        pos += snprintf(buf + pos, buflen - pos,
                        "{\"pairing_id\":%d,\"ticket\":\"%s\","
                        "\"name\":\"%s\",\"company\":\"%s\"}",
                        entries[i].id, entries[i].partnerTicket, name, company);
        added++;
    }

    if (pos < buflen - 1) buf[pos++] = ']';
    buf[pos] = '\0';
    return 0;
}

// ─── GPIO / ADC bridges ───────────────────────────────────────────────────────

extern "C" int badge_bridge_pin_read(int n) {
    return digitalRead(n);
}

extern "C" void badge_bridge_pin_write(int n, int v) {
    digitalWrite(n, v);
}

extern "C" int badge_bridge_adc_read(int n) {
    return analogRead(n);
}
