#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeConfig.h"
// BadgeConfig.h.example — copy to BadgeConfig.h and fill in your values.
// BadgeConfig.h is in .gitignore; never commit credentials.

#pragma once

// ─── WiFi credentials ─────────────────────────────────────────────────────────
static const char* WIFI_SSID = "Electromagnetism";
static const char* WIFI_PASS = "internetphotons";

// ─── Backend ──────────────────────────────────────────────────────────────────
static const char* SERVER_URL       = "http://192.168.50.205:8000";
static const char* EP_BADGE         = "/api/v1/badge";       // GET .../badge/{uid}/info
static const char* EP_BOOPS         = "/api/v1/boops";       // POST — create pairing

// ─── Timing ───────────────────────────────────────────────────────────────────
const int WIFI_TIMEOUT_MS    = 15000; // WPA3 SAE handshake needs up to ~10s
const int PAIRING_TIMEOUT_MS = 15000;  // QR display + poll window
const int POLL_INTERVAL_MS   = 2000;

// ─── Behaviour flags ──────────────────────────────────────────────────────────
const bool TILT_SHOWS_BADGE = true;           // tilt badge upright → show nametag

// ─── Pin definitions ──────────────────────────────────────────────────────────
#define BTN_UP     D7
#define BTN_DOWN   D8
#define BTN_LEFT   D9
#define BTN_RIGHT  D10

#define JOY_X      D0
#define JOY_Y      D1

#define TILT_PIN   D6

#define IR_RX_PIN  D3
#define IR_TX_PIN  D2

// ─── Joystick / tilt tuning ───────────────────────────────────────────────────
static const float         JOY_DEADBAND       = 0.08f;   // normalised axis dead zone (±)
static const float         MENU_NAV_THRESHOLD = 0.5f;    // joystick deflection to move menu cursor
static const int           JOY_CIRCLE_R       = 6;       // radius of joystick dot circle (px)
static const int           JOY_CIRCLE_CX      = 100;     // centre X of joystick dot circle (px)
static const int           JOY_CIRCLE_CY      = 53;      // centre Y of joystick dot circle (px)
// ─── Display ──────────────────────────────────────────────────────────────────
// I2C SDA=D4 (GPIO5), SCL=D5 (GPIO6) — hardware I2C on XIAO ESP32-S3
// U8G2 instance is defined in BadgeDisplay.cpp; no display constants needed here.

// ─── Messaging poll ───────────────────────────────────────────────────────────
// spec-009: Interval between GET /api/v1/pings polls while Messages screen is active.
#define MSG_POLL_INTERVAL_MS 5000

// ─── MicroPython scripting runtime ───────────────────────────────────────────
// See specs/007-embed-micropython-runtime/ for details.

// MicroPython GC heap size in bytes. Allocated from PSRAM via ps_malloc().
// 128 KB is a safe default; can increase if apps need more and PSRAM is available.
#define MICROPY_HEAP_SIZE   (128 * 1024)

// Maximum .py source file size to load before executing.
// Files larger than this are rejected with an error before any malloc.
#define MP_SCRIPT_MAX_BYTES 16384

// VFS directory scanned for Python apps. Must match LittleFS mount point prefix.
#define MP_APPS_DIR         "/spiffs"  // LittleFS mounted at /spiffs; apps are at FS root
