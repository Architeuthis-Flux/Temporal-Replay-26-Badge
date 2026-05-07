#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeTest.cpp"
// BadgeTest.cpp — Serial-triggered self-test framework
//
// C-level tests call bridge functions directly and assert return values.
// Python-level tests execute inline scripts via BadgePython::execStr() and
// assert no exception was raised.
//
// All tests run on Core 1. Python tests require BadgePython::init() to have
// succeeded; if Python is disabled they are reported as SKIP.

#include "BadgeTest.h"
#include "BadgeConfig.h"
#include "BadgePython.h"

#include <Arduino.h>
#include <string.h>

// Bridge declarations (C linkage)
#include "badge_mp/BadgePython_bridges.h"

// mp_hal_ticks_ms is a MicroPython HAL function implemented in BadgePython_bridges.cpp
extern "C" unsigned long mp_hal_ticks_ms(void);

// ─── Test result tracking ─────────────────────────────────────────────────────

static int _pass;
static int _total;

// Print a fixed-width test label and result.
static void reportResult(const char* name, bool passed, const char* detail = nullptr) {
    _total++;
    if (passed) _pass++;

    // Pad name to 20 chars with dots
    char label[24];
    int len = strlen(name);
    if (len > 20) len = 20;
    memcpy(label, name, len);
    for (int i = len; i < 20; i++) label[i] = '.';
    label[20] = '\0';

    if (passed) {
        Serial.printf("[test] %s PASS\n", label);
    } else if (detail) {
        Serial.printf("[test] %s FAIL (%s)\n", label, detail);
    } else {
        Serial.printf("[test] %s FAIL\n", label);
    }
}

static void reportSkip(const char* name, const char* reason) {
    char label[24];
    int len = strlen(name);
    if (len > 20) len = 20;
    memcpy(label, name, len);
    for (int i = len; i < 20; i++) label[i] = '.';
    label[20] = '\0';
    Serial.printf("[test] %s SKIP (%s)\n", label, reason);
}

// ─── C-level tests ────────────────────────────────────────────────────────────

static void runCTests() {
    // c:uid — badge_bridge_get_uid returns non-empty 12-char hex string
    {
        char buf[16] = {0};
        int rc = badge_bridge_get_uid(buf, sizeof(buf));
        bool pass = (rc == 0) && (strlen(buf) == 12);
        char detail[48];
        if (!pass) snprintf(detail, sizeof(detail), "rc=%d len=%u buf=\"%.12s\"", rc, (unsigned)strlen(buf), buf);
        reportResult("c:uid", pass, pass ? nullptr : detail);
    }

    // c:server_url — returns non-null, starts with "https://"
    {
        const char* url = badge_bridge_get_server_url();
        bool pass = (url != nullptr) && (strncmp(url, "https://", 8) == 0);
        char detail[64];
        if (!pass) snprintf(detail, sizeof(detail), "got \"%s\"", url ? url : "(null)");
        reportResult("c:server_url", pass, pass ? nullptr : detail);
    }

    // c:display — clear + text + show complete without crash
    {
        badge_bridge_display_clear();
        badge_bridge_display_text("test", 0, 10);
        badge_bridge_display_show();
        reportResult("c:display", true);
    }

    // c:ir_available — returns 0 (v1: queue not wired)
    {
        int rc = badge_bridge_ir_available();
        bool pass = (rc == 0);
        char detail[24];
        if (!pass) snprintf(detail, sizeof(detail), "got %d", rc);
        reportResult("c:ir_available", pass, pass ? nullptr : detail);
    }

    // c:adc_read — analogRead(JOY_X) returns 0–4095
    {
        int val = badge_bridge_adc_read(JOY_X);
        bool pass = (val >= 0 && val <= 4095);
        char detail[32];
        if (!pass) snprintf(detail, sizeof(detail), "got %d", val);
        reportResult("c:adc_read", pass, pass ? nullptr : detail);
    }

    // c:hal_ticks — mp_hal_ticks_ms() > 0, two calls are monotonic
    {
        unsigned long t0 = mp_hal_ticks_ms();
        delay(2);
        unsigned long t1 = mp_hal_ticks_ms();
        bool pass = (t0 > 0) && (t1 >= t0);
        char detail[48];
        if (!pass) snprintf(detail, sizeof(detail), "t0=%lu t1=%lu", t0, t1);
        reportResult("c:hal_ticks", pass, pass ? nullptr : detail);
    }
}

// ─── Python-level tests ───────────────────────────────────────────────────────

// Execute Python code via execStr and report as a named test.
static void pyTest(const char* name, const char* code) {
    bool ok = BadgePython::execStr(code);
    reportResult(name, ok);
}

static void runPyTests() {
    if (BadgePython::isDisabled()) {
        const char* pyNames[] = {
            "py:arithmetic", "py:strings",  "py:uid",
            "py:server_url", "py:constants", "py:display",
            "py:ir_available"
        };
        for (int i = 0; i < 7; i++) reportSkip(pyNames[i], "runtime disabled");
        return;
    }

    pyTest("py:arithmetic",
           "assert 2 + 2 == 4\n"
           "assert 10 // 3 == 3\n");

    pyTest("py:strings",
           "assert len('hello') == 5\n"
           "assert 'badge'[0] == 'b'\n");

    pyTest("py:uid",
           "import badge\n"
           "u = badge.uid()\n"
           "assert len(u) == 12, 'len=' + str(len(u))\n"
           "assert u == u.lower(), 'not lowercase'\n");

    pyTest("py:server_url",
           "import badge\n"
           "assert badge.server_url().startswith('https://'), "
           "'got: ' + badge.server_url()\n");

    pyTest("py:constants",
           "import badge\n"
           "assert badge.BTN_UP == 44, 'BTN_UP=' + str(badge.BTN_UP)\n");

    pyTest("py:display",
           "import badge\n"
           "badge.display.clear()\n"
           "badge.display.text('test', 0, 10)\n"
           "badge.display.show()\n");

    pyTest("py:ir_available",
           "import badge\n"
           "assert badge.ir_available() == 0, "
           "'got: ' + str(badge.ir_available())\n");
}

// ─── runTests entry point ─────────────────────────────────────────────────────

void runTests(const char* suite) {
    _pass  = 0;
    _total = 0;

    Serial.println("[test] BEGIN");

    bool doC  = (strcmp(suite, "all") == 0 || strcmp(suite, "c") == 0);
    bool doPy = (strcmp(suite, "all") == 0 || strcmp(suite, "py") == 0);

    if (doC) {
        runCTests();
        Serial.printf("[test] C: %d/%d passed\n", _pass, _total);
    }

    int cPass  = _pass;
    int cTotal = _total;

    if (doPy) {
        int pyPass0  = _pass;
        int pyTotal0 = _total;
        runPyTests();
        int pyPass  = _pass  - pyPass0;
        int pyTotal = _total - pyTotal0;
        Serial.printf("[test] Python: %d/%d passed\n", pyPass, pyTotal);
    }

    Serial.printf("[test] TOTAL: %d/%d passed\n", _pass, _total);
    Serial.println("[test] END");
}
