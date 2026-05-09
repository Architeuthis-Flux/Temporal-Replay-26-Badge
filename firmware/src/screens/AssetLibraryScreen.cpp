#include "AssetLibraryScreen.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>

#include "../api/WiFiService.h"
#include "../hardware/Haptics.h"
#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"

namespace {

const ota::AssetEntry* sActiveAsset = nullptr;
char sQueuedSelectId[ota::kAssetIdMax] = "";

const char* statusLabel(ota::AssetStatus s) {
  switch (s) {
    case ota::AssetStatus::kInstalled:       return "OK";
    case ota::AssetStatus::kUpdateAvailable: return "UPD";
    case ota::AssetStatus::kFailed:          return "ERR";
    case ota::AssetStatus::kNotInstalled:
    default:                                 return " - ";
  }
}

}  // namespace

// ── AssetLibraryScreen ────────────────────────────────────────────────────

AssetLibraryScreen::AssetLibraryScreen()
    : ListMenuScreen(kScreenAssetLibrary, "ASSET LIBRARY") {}

void AssetLibraryScreen::selectAssetById(const char* id) {
  if (!id) { sQueuedSelectId[0] = '\0'; return; }
  std::strncpy(sQueuedSelectId, id, sizeof(sQueuedSelectId) - 1);
  sQueuedSelectId[sizeof(sQueuedSelectId) - 1] = '\0';
}

void AssetLibraryScreen::onEnter(GUIManager& gui) {
  ListMenuScreen::onEnter(gui);
  cachedCount_ = static_cast<uint8_t>(ota::registry::count());

  // First time we open the screen since boot, kick a refresh if WiFi
  // is up. Cooldown still applies — if we refreshed today this is a
  // no-op.
  if (!didInitialRefresh_) {
    didInitialRefresh_ = true;
    if (cachedCount_ == 0 && wifiService.isConnected()) {
      ota::registry::refresh(false);
      cachedCount_ = static_cast<uint8_t>(ota::registry::count());
    }
  }

  // Honor a pending selectAssetById() from the DOOM no-WAD redirect.
  if (sQueuedSelectId[0]) {
    for (uint8_t i = 0; i < cachedCount_; ++i) {
      const ota::AssetEntry* e = ota::registry::at(i);
      if (e && std::strcmp(e->id, sQueuedSelectId) == 0) {
        cursor_ = i;
        break;
      }
    }
    sQueuedSelectId[0] = '\0';
  }
}

uint8_t AssetLibraryScreen::itemCount() const {
  return cachedCount_;
}

void AssetLibraryScreen::formatItem(uint8_t index, char* buf,
                                    uint8_t bufSize) const {
  const ota::AssetEntry* e = ota::registry::at(index);
  if (!e) {
    std::snprintf(buf, bufSize, "(missing)");
    return;
  }
  ota::AssetStatus s = ota::registry::statusOf(*e);
  std::snprintf(buf, bufSize, "%-3s %s", statusLabel(s), e->name);
}

void AssetLibraryScreen::onItemSelect(uint8_t index, GUIManager& gui) {
  const ota::AssetEntry* e = ota::registry::at(index);
  if (!e) return;
  AssetDetailScreen::setActiveAsset(e);
  Haptics::shortPulse();
  gui.pushScreen(kScreenAssetDetail);
}

// ── AssetDetailScreen ─────────────────────────────────────────────────────

void AssetDetailScreen::setActiveAsset(const ota::AssetEntry* entry) {
  sActiveAsset = entry;
}

void AssetDetailScreen::onEnter(GUIManager& /*gui*/) {
  phase_ = Phase::kIdle;
  bytesWritten_ = 0;
  totalBytes_ = sActiveAsset ? sActiveAsset->size : 0;
}

bool AssetDetailScreen::needsRender() {
  return phase_ == Phase::kInstalling;
}

void AssetDetailScreen::render(oled& d, GUIManager& /*gui*/) {
  d.setDrawColor(1);
  if (!sActiveAsset) {
    OLEDLayout::drawStatusHeader(d, "ASSET");
    d.setFontPreset(FONT_TINY);
    d.drawStr(2, 24, "(no asset selected)");
    OLEDLayout::drawNavFooter(d, "Cancel:Back");
    return;
  }

  OLEDLayout::drawStatusHeader(d, sActiveAsset->name);
  d.setFontPreset(FONT_TINY);

  if (phase_ == Phase::kInstalling) {
    d.drawStr(2, 22, "Downloading...");
    char szBuf[40];
    if (totalBytes_ > 0) {
      std::snprintf(szBuf, sizeof(szBuf), "%u / %u KB",
                    (unsigned)(bytesWritten_ / 1024),
                    (unsigned)(totalBytes_ / 1024));
    } else {
      std::snprintf(szBuf, sizeof(szBuf), "%u KB",
                    (unsigned)(bytesWritten_ / 1024));
    }
    d.drawStr(2, 31, szBuf);
    constexpr int kBarX = 4, kBarY = 38, kBarW = 120, kBarH = 8;
    d.drawRFrame(kBarX, kBarY, kBarW, kBarH, 1);
    if (totalBytes_ > 0) {
      int fill = static_cast<int>(((bytesWritten_ * (kBarW - 2)) /
                                   totalBytes_));
      if (fill < 0) fill = 0;
      if (fill > kBarW - 2) fill = kBarW - 2;
      d.drawBox(kBarX + 1, kBarY + 1, fill, kBarH - 2);
    }
    OLEDLayout::drawNavFooter(d, "Do not unplug");
    return;
  }

  if (phase_ == Phase::kError) {
    d.drawStr(2, 22, "Install failed:");
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%s",
                  ota::registry::lastErrorMessage());
    OLEDLayout::fitText(d, buf, sizeof(buf), 124);
    d.drawStr(2, 32, buf);
    OLEDLayout::drawNavFooter(d, "Any:Back");
    return;
  }

  if (phase_ == Phase::kDone) {
    d.setFontPreset(FONT_SMALL);
    d.drawStr(2, 24, "Installed!");
    d.setFontPreset(FONT_TINY);
    d.drawStr(2, 36, sActiveAsset->dest_path);
    OLEDLayout::drawNavFooter(d, "Cancel:Back");
    return;
  }

  // Idle: show metadata + action footer.
  ota::AssetStatus status = ota::registry::statusOf(*sActiveAsset);
  char line[64];

  std::snprintf(line, sizeof(line), "Latest:    %s",
                sActiveAsset->version);
  d.drawStr(2, 18, line);

  const char* installed = ota::registry::installedVersionOf(*sActiveAsset);
  if (installed[0]) {
    std::snprintf(line, sizeof(line), "Installed: %s", installed);
  } else {
    std::snprintf(line, sizeof(line), "Installed: (none)");
  }
  d.drawStr(2, 27, line);

  if (sActiveAsset->size > 0) {
    std::snprintf(line, sizeof(line), "Size:      %u KB",
                  (unsigned)(sActiveAsset->size / 1024));
    d.drawStr(2, 36, line);
  }

  if (sActiveAsset->description[0]) {
    char desc[80];
    std::snprintf(desc, sizeof(desc), "%s", sActiveAsset->description);
    OLEDLayout::fitText(d, desc, sizeof(desc), 124);
    d.drawStr(2, 45, desc);
  }

  const char* action = "Install";
  if (status == ota::AssetStatus::kInstalled) {
    action = "Remove";
  } else if (status == ota::AssetStatus::kUpdateAvailable) {
    action = "Update";
  }
  OLEDLayout::drawActionFooter(d, action, "Confirm");
}

void AssetDetailScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                    int16_t /*cy*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();

  if (phase_ == Phase::kInstalling) return;

  if (phase_ == Phase::kError || phase_ == Phase::kDone) {
    if (e.cancelPressed || e.confirmPressed || e.xPressed || e.yPressed) {
      phase_ = Phase::kIdle;
      gui.popScreen();
    }
    return;
  }

  if (e.cancelPressed) {
    Haptics::shortPulse();
    gui.popScreen();
    return;
  }

  if (!sActiveAsset) return;

  if (e.confirmPressed) {
    Haptics::shortPulse();
    ota::AssetStatus status = ota::registry::statusOf(*sActiveAsset);
    if (status == ota::AssetStatus::kInstalled) {
      runRemove(gui);
    } else {
      runInstall(gui);
    }
  }
}

void AssetDetailScreen::progressCb(const ota::AssetProgress& prog,
                                   void* user) {
  auto* self = static_cast<AssetDetailScreen*>(user);
  if (!self) return;
  self->bytesWritten_ = prog.bytesWritten;
  self->totalBytes_ = prog.totalBytes;
  if (prog.done) self->lastResult_ = prog.result;
  extern GUIManager guiManager;
  oled& d = guiManager.oledDisplay();
  d.clearBuffer();
  self->render(d, guiManager);
  d.sendBuffer();
}

void AssetDetailScreen::runInstall(GUIManager& gui) {
  if (!sActiveAsset) return;
  phase_ = Phase::kInstalling;
  bytesWritten_ = 0;
  totalBytes_ = sActiveAsset->size;

  oled& d = gui.oledDisplay();
  d.clearBuffer();
  render(d, gui);
  d.sendBuffer();

  const bool ok =
      ota::registry::install(*sActiveAsset, &AssetDetailScreen::progressCb,
                             this);
  if (ok) {
    phase_ = Phase::kDone;
  } else {
    phase_ = Phase::kError;
  }
}

void AssetDetailScreen::runRemove(GUIManager& /*gui*/) {
  if (!sActiveAsset) return;
  ota::registry::remove(*sActiveAsset);
  phase_ = Phase::kIdle;
}
