#pragma once

// ============================================================
//  WiFi indicator icon (11 × 6)
//
//  Sourced verbatim from
//  Replay-26-Badge_QAFW/QA-Firmware/assets/wifi.xbm — the slim arched
//  three-arc glyph used in the reference badge's status header. Painted
//  by OLEDLayout::drawWifiIcon; disconnected/busy cues are layered on
//  top in code (diagonal slash for not-connected, blinking dot for
//  active sync).
// ============================================================

namespace WifiIcon {

constexpr int kWidth  = 11;
constexpr int kHeight = 6;
static const unsigned char kBits[] = {
    0xfc, 0x01, 0x02, 0x02, 0xf9, 0x04,
    0x04, 0x01, 0x70, 0x00, 0x20, 0x00,
};

// "No WiFi" variant — same arched glyph with a baked-in diagonal slash
// (QA-Firmware/assets/no-wifi.xbm). Drawn in place of kBits when the
// service is offline so the disconnect cue is part of the icon rather
// than an XOR-cut overlay.
static const unsigned char kBitsNoWifi[] = {
    0xdc, 0x01, 0xde, 0x03, 0xff, 0x07,
    0xdc, 0x01, 0x70, 0x00, 0x20, 0x00,
};

}  // namespace WifiIcon
