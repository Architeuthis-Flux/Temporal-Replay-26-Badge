#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeQR.h"
// BadgeQR.h — on-device QR code generation (ricmoo/QRCode library)
// Generates a QR code from a text string and returns a scaled XBM bitmap
// ready to pass directly to u8g2.drawXBMP().
//
// All timing is logged to Serial at "[QRLocal]" prefix so it can be
// compared against server-fetched QR performance.

#pragma once
#include <Arduino.h>

struct LocalQRResult {
    bool      ok;
    uint8_t*  buf;       // heap-allocated scaled XBM bytes; caller must free()
    int       len;       // byte count of buf
    int       w;         // pixel width  (modules * scale)
    int       h;         // pixel height (modules * scale, capped at 64)
    int       modules;   // raw QR module grid dimension (e.g. 29 for version 3)
    int       scale;     // pixels-per-module used
    unsigned long genMs; // wall-clock generation time in ms
};

namespace BadgeQR {
    // Generate QR code from 'text' and return a scaled XBM buffer.
    // scale=0 → auto (floor(64 / moduleCount), minimum 1).
    // Returns ok=false on malloc failure or text too long for version 7.
    LocalQRResult generate(const char* text, uint8_t scale = 0);
}
