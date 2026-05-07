#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeQR.cpp"
// BadgeQR.cpp — on-device QR code generation
//
// Uses ricmoo/QRCode (Arduino library, MIT): https://github.com/ricmoo/QRCode
// Tries versions 1–7 with ECC_MEDIUM until the text fits.
// Produces a heap-allocated, scale-multiplied XBM byte buffer compatible
// with u8g2.drawXBMP().  Logs timing + dimensions to Serial.

#include "BadgeQR.h"
#include "qrcode.h"

namespace BadgeQR {

LocalQRResult generate(const char* text, uint8_t requestedScale) {
    LocalQRResult r = {};
    unsigned long t0 = millis();

    // ── 1. Find minimum QR version that fits 'text' ───────────────────────
    QRCode qrc;
    // Max buffer for version 7 = ((7*4+17)^2 + 7) / 8 = 254 bytes — stack OK
    uint8_t qrData[qrcode_getBufferSize(7)];

    int ver = 1;
    bool found = false;
    while (ver <= 7) {
        if (qrcode_initText(&qrc, qrData, ver, ECC_MEDIUM, text) == 0) {
            found = true;
            break;
        }
        ver++;
    }

    if (!found) {
        Serial.printf("[QRLocal] FAIL: text too long for version 7 (%d chars)\n", (int)strlen(text));
        r.ok = false;
        return r;
    }

    int modules = qrc.size;

    // ── 2. Choose scale ───────────────────────────────────────────────────
    uint8_t scale = requestedScale;
    if (scale == 0) {
        scale = (uint8_t)(64 / modules);
        if (scale < 1) scale = 1;
    }

    // ── 3. Allocate scaled XBM buffer ────────────────────────────────────
    int pixW     = modules * scale;
    int pixH     = modules * scale;
    if (pixH > 64) pixH = 64;   // cap to display height (clips quiet zone rows)

    int rowBytes = (pixW + 7) / 8;
    int bufLen   = rowBytes * pixH;

    uint8_t* buf = (uint8_t*)ps_malloc(bufLen);
    if (!buf) {
        Serial.printf("[QRLocal] FAIL: ps_malloc %d bytes\n", bufLen);
        r.ok = false;
        return r;
    }
    memset(buf, 0, bufLen);

    // ── 4. Convert module grid → scaled XBM (LSB-first, dark module = 1) ─
    // y_offset: if pixH < modules*scale we're clipping the bottom;
    // to centre vertically clip equally top+bottom.
    int fullH    = modules * scale;
    int clipRows = fullH - pixH;       // total rows clipped (distributed top/bottom)
    int clipTop  = clipRows / 2;       // rows skipped at top

    for (int my = 0; my < modules; my++) {
        for (int s_y = 0; s_y < scale; s_y++) {
            int py = my * scale + s_y - clipTop;
            if (py < 0 || py >= pixH) continue;
            for (int mx = 0; mx < modules; mx++) {
                if (!qrcode_getModule(&qrc, mx, my)) continue;
                for (int s_x = 0; s_x < scale; s_x++) {
                    int px = mx * scale + s_x;
                    if (px >= pixW) continue;
                    buf[py * rowBytes + px / 8] |= (1 << (px % 8));
                }
            }
        }
    }

    unsigned long genMs = millis() - t0;

    // ── 5. Log timing + diagnostics ───────────────────────────────────────
    Serial.printf("[QRLocal] ok  ver=%d modules=%d scale=%d "
                  "pixW=%d pixH=%d buf=%d bytes  genMs=%lu\n",
                  ver, modules, (int)scale, pixW, pixH, bufLen, genMs);

    r.ok      = true;
    r.buf     = buf;
    r.len     = bufLen;
    r.w       = pixW;
    r.h       = pixH;
    r.modules = modules;
    r.scale   = (int)scale;
    r.genMs   = genMs;
    return r;
}

} // namespace BadgeQR
