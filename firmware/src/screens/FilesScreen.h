#pragma once
#include "Screen.h"

// ─── Files screen (FAT directory listing) ───────────────────────────────────

class FilesScreen : public ListMenuScreen {
 public:
  FilesScreen(ScreenId sid);

  uint8_t itemCount() const override;
  void formatItem(uint8_t index, char* buf, uint8_t bufSize) const override;
  void onItemSelect(uint8_t index, GUIManager& gui) override;
  void onEnter(GUIManager& gui) override;
  bool navigableItems() const override { return true; }
  const char* hintText() const override { return "Cancel:back Confirm:open"; }

 private:
  static constexpr uint8_t kMaxFiles = 20;
  static constexpr uint8_t kMaxNameLen = 28;

  char fileNames_[kMaxFiles][kMaxNameLen];
  uint32_t fileSizes_[kMaxFiles];
  uint8_t fileCount_ = 0;

  void scanDirectory(const char* path);
};
