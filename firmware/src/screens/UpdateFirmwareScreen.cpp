#include "UpdateFirmwareScreen.h"

#include <cstdio>
#include <cstring>
#include <Arduino.h>

#include "../hardware/Haptics.h"
#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "../identity/BadgeVersion.h"
#include "../ui/ButtonGlyphs.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"
#include "../api/WiFiService.h"

namespace {

void formatRelativeTime(time_t epoch, char* buf, size_t cap) {
  if (epoch <= 1) {
    std::snprintf(buf, cap, "never");
    return;
  }
  time_t now = time(nullptr);
  if (now <= 0 || now < epoch) {
    std::snprintf(buf, cap, "just now");
    return;
  }
  uint32_t delta = static_cast<uint32_t>(now - epoch);
  if (delta < 60) {
    std::snprintf(buf, cap, "%us ago", (unsigned)delta);
  } else if (delta < 3600) {
    std::snprintf(buf, cap, "%um ago", (unsigned)(delta / 60));
  } else if (delta < 86400) {
    std::snprintf(buf, cap, "%uh ago", (unsigned)(delta / 3600));
  } else {
    std::snprintf(buf, cap, "%ud ago", (unsigned)(delta / 86400));
  }
}

int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

void drawCentered(oled& d, int y, const char* text) {
  if (!text) return;
  const int w = d.getStrWidth(text);
  d.drawStr((128 - w) / 2, y, text);
}

void drawSimpleSpinner(oled& d, int cx, int cy, uint8_t phase) {
  static constexpr int8_t kPts[4][2] = {
      {0, -5}, {5, 0}, {0, 5}, {-5, 0},
  };
  d.setDrawColor(1);
  for (uint8_t i = 0; i < 4; ++i) {
    const int px = cx + kPts[i][0];
    const int py = cy + kPts[i][1];
    if (i == (phase & 3)) {
      d.drawBox(px - 1, py - 1, 3, 3);
    } else {
      d.drawPixel(px, py);
    }
  }
}

void drawCleanProgress(oled& d, int x, int y, int w, int h,
                       size_t bytes, size_t total, uint8_t phase) {
  d.setDrawColor(1);
  d.drawRFrame(x, y, w, h, 3);

  const int innerX = x + 2;
  const int innerY = y + 2;
  const int innerW = w - 4;
  const int innerH = h - 4;
  if (innerW <= 0 || innerH <= 0) return;

  if (total > 0) {
    int fill = static_cast<int>((bytes * innerW) / total);
    fill = clampInt(fill, 0, innerW);
    if (fill > 0) {
      const int radius = (fill >= innerH) ? 2 : 1;
      d.setClipWindow(innerX, innerY, innerX + fill, innerY + innerH);
      d.drawRBox(innerX, innerY, fill + 2, innerH, radius);

      // A small inverted shimmer gives motion without cluttering the
      // screen. It stays clipped to the filled portion of the rounded bar.
      d.setDrawColor(0);
      for (int sx = innerX - innerH + (phase % 8); sx < innerX + fill; sx += 8) {
        d.drawLine(sx, innerY + innerH - 1, sx + innerH - 1, innerY);
      }
      d.setDrawColor(1);
      d.setMaxClipWindow();

      const int glint = innerX + fill - 1;
      if (glint >= innerX && glint < innerX + innerW) {
        d.drawVLine(glint, innerY + 1, innerH - 2);
      }
    }
  } else {
    const int blockW = 24;
    const int travel = innerW + blockW;
    const int bx = innerX - blockW + ((phase * 2) % travel);
    d.setClipWindow(innerX, innerY, innerX + innerW, innerY + innerH);
    d.drawRBox(bx, innerY, blockW, innerH, 2);
    d.setMaxClipWindow();
  }
}

void drawTwoLineError(oled& d, const char* message) {
  char first[80];
  char second[80];
  first[0] = '\0';
  second[0] = '\0';
  std::snprintf(first, sizeof(first), "%s", message && message[0] ? message : "Unknown error");

  char* split = nullptr;
  for (char* p = first; *p; ++p) {
    if (*p == ' ') {
      *p = '\0';
      if (d.getStrWidth(first) <= 120 && d.getStrWidth(p + 1) <= 120) {
        split = p;
      }
      *p = ' ';
    }
  }
  if (split) {
    *split = '\0';
    std::snprintf(second, sizeof(second), "%s", split + 1);
  }
  OLEDLayout::fitText(d, first, sizeof(first), 120);
  OLEDLayout::fitText(d, second, sizeof(second), 120);
  d.drawStr(4, 31, first);
  if (second[0]) d.drawStr(4, 41, second);
}

void drawModalPanel(oled& d, const char* title, const char* line1,
                    const char* line2, const char* line3,
                    bool /*danger*/ = false) {
  d.setDrawColor(1);
  d.drawRFrame(4, 15, 120, 34, 2);
  d.setFontPreset(FONT_TINY);
  drawCentered(d, 24, title ? title : "");
  if (line1) drawCentered(d, 34, line1);
  if (line2) drawCentered(d, 43, line2);
  if (line3) drawCentered(d, 52, line3);
}

}  // namespace

void UpdateFirmwareScreen::onEnter(GUIManager& /*gui*/) {
  phase_ = Phase::kIdle;
  installDone_ = false;
  installBytes_ = 0;
  installTotal_ = 0;
  // The OTA cooldown is gone: every screen entry triggers a fresh
  // check so the user always sees the very latest release tag the
  // moment they open the screen. Skipped if WiFi is down — the idle
  // render surfaces the "WiFi off" warning in that case.
  if (wifiService.isConnected()) {
    runCheck(true);
  }
  firstEnter_ = false;
}

bool UpdateFirmwareScreen::needsRender() {
  // The Installing phase animates a progress bar even when the user
  // isn't pressing buttons. Other phases only repaint on input.
  return phase_ == Phase::kInstalling || phase_ == Phase::kChecking;
}

void UpdateFirmwareScreen::render(oled& d, GUIManager& /*gui*/) {
  d.setDrawColor(1);

  // Expand-storage phases own their own status header — paint it
  // first instead of the FW UPDATE header so the two don't overlap.
  if (phase_ == Phase::kExpandConfirm) {
    renderExpandConfirm(d, /*secondConfirm=*/false);
    return;
  }
  if (phase_ == Phase::kExpandConfirm2) {
    renderExpandConfirm(d, /*secondConfirm=*/true);
    return;
  }
  if (phase_ == Phase::kReinstallConfirm) {
    renderReinstallConfirm(d);
    return;
  }

  OLEDLayout::drawStatusHeader(d, "FW UPDATE");
  d.setFontPreset(FONT_TINY);

  if (phase_ == Phase::kChecking) {
    const uint8_t phase = static_cast<uint8_t>((millis() - spinnerStartMs_) / 80);
    drawSimpleSpinner(d, 64, 28, phase);
    d.setFontPreset(FONT_TINY);
    drawCentered(d, 43, "Checking GitHub...");
    OLEDLayout::drawNavFooter(d, "Please wait");
    return;
  }

  if (phase_ == Phase::kInstalling) {
    const uint8_t phase = static_cast<uint8_t>((millis() - installStartMs_) / 70);
    const uint8_t pct = installTotal_ > 0
                            ? static_cast<uint8_t>(clampInt(
                                  (int)((installBytes_ * 100u) / installTotal_), 0, 100))
                            : 0;
    d.setFontPreset(FONT_TINY);
    drawCentered(d, 18, "Installing firmware");

    char pctBuf[8];
    if (installTotal_ > 0) {
      std::snprintf(pctBuf, sizeof(pctBuf), "%u%%", (unsigned)pct);
    } else {
      std::snprintf(pctBuf, sizeof(pctBuf), "--%%");
    }
    d.setFontPreset(FONT_SMALL);
    drawCentered(d, 31, pctBuf);

    char szBuf[40];
    if (installTotal_ > 0) {
      std::snprintf(szBuf, sizeof(szBuf), "%u / %u KB",
                    (unsigned)(installBytes_ / 1024),
                    (unsigned)(installTotal_ / 1024));
    } else {
      std::snprintf(szBuf, sizeof(szBuf), "%u KB",
                    (unsigned)(installBytes_ / 1024));
    }
    d.setFontPreset(FONT_TINY);
    drawCentered(d, 41, szBuf);
    drawCleanProgress(d, 16, 47, 96, 6, installBytes_, installTotal_, phase);
    OLEDLayout::drawNavFooter(d, "Do not unplug");
    return;
  }

  if (phase_ == Phase::kError) {
    d.setFontPreset(FONT_TINY);
    drawCentered(d, 22, "Update failed");
    drawTwoLineError(d, ota::lastErrorMessage());
    OLEDLayout::drawNavFooter(d, "Any:Back");
    return;
  }

  // Idle. Keep the screen sparse: versions, status, actions.
  char line[48];
  d.setFontPreset(FONT_TINY);
  std::snprintf(line, sizeof(line), "Current: %s", FIRMWARE_VERSION);
  d.drawStr(4, 20, line);

  const char* tag = ota::latestKnownTag();
  std::snprintf(line, sizeof(line), "Latest:  %s", tag[0] ? tag : "(not checked)");
  OLEDLayout::fitText(d, line, sizeof(line), 120);
  d.drawStr(4, 30, line);

  const bool wifiUp = wifiService.isConnected();
  const char* warning = nullptr;
  if (!wifiUp) {
    warning = "WiFi off - connect first";
  } else if (lastCheckResult_ == ota::CheckResult::kNoMatchingAsset) {
    warning = "No asset for this build";
  } else if (lastCheckResult_ == ota::CheckResult::kOkOlder) {
    warning = "Newer than published";
  }
  if (warning) {
    std::snprintf(line, sizeof(line), "%s", warning);
  } else {
    char age[20];
    formatRelativeTime(ota::lastCheckEpoch(), age, sizeof(age));
    std::snprintf(line, sizeof(line), "Last check: %s", age);
  }
  OLEDLayout::fitText(d, line, sizeof(line), 120);
  d.drawStr(4, 40, line);

  const size_t volBytes = ota::ffatVolumeBytes();
  if (volBytes > 0) {
    char szLine[48];
    const float curMb = volBytes / (1024.0f * 1024.0f);
    if (ota::ffatExpansionAvailable()) {
      const float partMb =
          ota::ffatPartitionBytes() / (1024.0f * 1024.0f);
      std::snprintf(szLine, sizeof(szLine),
                    "FS: %.1f MB  X expand %.1f", curMb, partMb);
    } else {
      std::snprintf(szLine, sizeof(szLine), "FS: %.1f MB", curMb);
    }
    ButtonGlyphs::drawInlineHint(d, 4, 50, szLine);
  }

  // Footer: D-pad Up = Check, D-pad Right = Install. Drawn through
  // ButtonGlyphs::drawInlineHint so the literal `^` and `>` are
  // substituted with the up/right glyphs (per UI conventions —
  // see firmware/src/ui/ButtonGlyphs.h). drawNavFooter handles the
  // chrome; we feed it the glyph-bearing string ourselves.
  // Right-press always offers an install path now: a strictly newer
  // release runs straight through, otherwise the screen prompts the
  // user to confirm a same-or-older reinstall before flashing.
  const char* hint;
  if (ota::updateAvailable()) {
    hint = "^ Check  > Install";
  } else if (tag[0] && ota::latestKnownAssetUrl()[0]) {
    hint = "^ Check  > Reinstall";
  } else {
    hint = "^ Check";
  }
  OLEDLayout::drawNavFooter(d);
  // drawNavFooter() with no text only paints chrome; render the
  // glyph hint directly afterwards on the footer baseline (y=63 is
  // the project's standard hint baseline used elsewhere).
  ButtonGlyphs::drawInlineHint(d, 2, 63, hint);
}

void UpdateFirmwareScreen::renderExpandConfirm(oled& d, bool secondConfirm) {
  OLEDLayout::drawStatusHeader(d, "EXPAND STORAGE");

  const unsigned partMb =
      static_cast<unsigned>(ota::ffatPartitionBytes() / (1024u * 1024u));

  if (!secondConfirm) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "Reformat to %u MB?", partMb);
    drawModalPanel(d, "Expand storage", buf,
                   "This wipes user data.",
                   nullptr, true);
    OLEDLayout::drawActionFooter(d, "Continue", "Confirm");
    return;
  }

  drawModalPanel(d, "Are you sure?", "This cannot be undone.",
                 "Badge reboots after format.",
                 nullptr, true);
  OLEDLayout::drawActionFooter(d, "WIPE & EXPAND", "Wipe");
}

void UpdateFirmwareScreen::renderReinstallConfirm(oled& d) {
  OLEDLayout::drawStatusHeader(d, "REINSTALL?");

  const char* tag = ota::latestKnownTag();
  char line[48];
  std::snprintf(line, sizeof(line), "%s -> %s", FIRMWARE_VERSION,
                tag[0] ? tag : "(unknown)");

  // The user requested an install even though we're at-or-ahead of
  // the published tag. Make the consequences explicit so a fat-fingered
  // D-pad press doesn't trigger a multi-MB flash + reboot.
  drawModalPanel(d, "Reinstall firmware", line,
                 "Already current or newer.",
                 nullptr);
  OLEDLayout::drawActionFooter(d, "Reinstall", "Confirm");
}

void UpdateFirmwareScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                       int16_t /*cy*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();

  if (phase_ == Phase::kChecking || phase_ == Phase::kInstalling) {
    return;  // synchronous; ignore input until done
  }

  if (phase_ == Phase::kError) {
    if (e.cancelPressed || e.confirmPressed || e.xPressed || e.yPressed) {
      phase_ = Phase::kIdle;
    }
    return;
  }

  if (phase_ == Phase::kExpandConfirm) {
    if (e.cancelPressed) { phase_ = Phase::kIdle; return; }
    if (e.confirmPressed) {
      Haptics::shortPulse();
      phase_ = Phase::kExpandConfirm2;
    }
    return;
  }
  if (phase_ == Phase::kExpandConfirm2) {
    if (e.cancelPressed) { phase_ = Phase::kIdle; return; }
    if (e.confirmPressed) {
      Haptics::shortPulse();
      // Final paint then wipe + reboot. reformatFfatAndReboot()
      // does not return.
      extern GUIManager guiManager;
      oled& d = guiManager.oledDisplay();
      d.clearBuffer();
      OLEDLayout::drawStatusHeader(d, "EXPAND STORAGE");
      d.setFontPreset(FONT_SMALL);
      d.drawStr(8, 30, "Formatting...");
      d.sendBuffer();
      Haptics::off();
      delay(100);
      ota::reformatFfatAndReboot();
    }
    return;
  }

  if (phase_ == Phase::kReinstallConfirm) {
    if (e.cancelPressed) { phase_ = Phase::kIdle; return; }
    if (e.confirmPressed) {
      Haptics::shortPulse();
      Haptics::off();
      delay(100);
      runInstall();
    }
    return;
  }

  if (e.cancelPressed) {
    Haptics::shortPulse();
    gui.popScreen();
    return;
  }

  // X button (yPressed in the codebase's swap-aware naming) opens
  // the expand-storage flow when the partition has unclaimed space.
  if (e.xPressed && ota::ffatExpansionAvailable()) {
    Haptics::shortPulse();
    phase_ = Phase::kExpandConfirm;
    return;
  }

  // D-pad split: Up = Check, Right = Install. Confirm/Y are unused
  // here so two adjacent presses can't accidentally double-fire the
  // multi-megabyte install. Plan calls for explicit Up/Right rather
  // than the previous toggle-on-Confirm behaviour.
  if (e.upPressed) {
    Haptics::shortPulse();
    Haptics::off();
    delay(100);
    runCheck(true);
    return;
  }
  if (e.rightPressed) {
    // Right-press always offers an install when we have an asset URL
    // cached. If the published tag is strictly newer we go straight
    // into runInstall (most common path); if we're already at-or-ahead
    // of the published version we surface a reinstall confirmation
    // first so the user knows they're flashing the same image.
    const char* tag = ota::latestKnownTag();
    if (!tag[0] || ota::latestKnownAssetUrl()[0] == '\0') {
      // No cached asset — nothing to install. Bounce back to a check
      // so the next press has something to flash.
      Haptics::shortPulse();
      Haptics::off();
      delay(100);
      runCheck(true);
      return;
    }
    Haptics::shortPulse();
    if (ota::updateAvailable()) {
      Haptics::off();
      delay(100);
      runInstall();
    } else {
      phase_ = Phase::kReinstallConfirm;
    }
    return;
  }
}

void UpdateFirmwareScreen::runCheck(bool ignoreCooldown) {
  phase_ = Phase::kChecking;
  spinnerStartMs_ = millis();
  // Force one render so the user sees the spinner before we block.
  extern GUIManager guiManager;
  guiManager.requestRender();
  // We can't yield to the render loop from here, so just paint
  // directly before the synchronous call.
  oled& d = guiManager.oledDisplay();
  d.clearBuffer();
  render(d, guiManager);
  d.sendBuffer();

  lastCheckResult_ = ota::checkNow(ignoreCooldown);
  Serial.printf("[updscreen] checkNow result=%d tag=%s\n",
                (int)lastCheckResult_, ota::latestKnownTag());

  if (lastCheckResult_ == ota::CheckResult::kNetworkError ||
      lastCheckResult_ == ota::CheckResult::kParseError) {
    phase_ = Phase::kError;
  } else {
    phase_ = Phase::kIdle;
  }
}

void UpdateFirmwareScreen::installProgressCb(
    const ota::InstallProgress& prog, void* user) {
  auto* self = static_cast<UpdateFirmwareScreen*>(user);
  if (!self) return;
  self->installBytes_ = prog.bytesWritten;
  self->installTotal_ = prog.totalBytes;
  self->installDone_ = prog.done;
  // Repaint progress.
  extern GUIManager guiManager;
  oled& d = guiManager.oledDisplay();
  d.clearBuffer();
  self->render(d, guiManager);
  d.sendBuffer();
}

void UpdateFirmwareScreen::runInstall() {
  phase_ = Phase::kInstalling;
  installStartMs_ = millis();
  installBytes_ = 0;
  installTotal_ = ota::latestKnownAssetSize();
  installDone_ = false;

  extern GUIManager guiManager;
  oled& d = guiManager.oledDisplay();
  d.clearBuffer();
  render(d, guiManager);
  d.sendBuffer();

  Haptics::off();
  // delay(100);
  installResult_ = ota::installCached(&UpdateFirmwareScreen::installProgressCb,
                                      this);

  if (installResult_ == ota::InstallResult::kOk) {
    // Final paint then reboot.
    d.clearBuffer();
    OLEDLayout::drawStatusHeader(d, "UPDATE COMPLETE");
    d.setFontPreset(FONT_SMALL);
    drawCentered(d, 32, "Rebooting...");
    d.setFontPreset(FONT_TINY);
    drawCentered(d, 44, "Firmware installed");
    d.sendBuffer();
    delay(800);
    ESP.restart();
  } else {
    phase_ = Phase::kError;
  }
}
