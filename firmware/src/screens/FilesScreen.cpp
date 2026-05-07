#include "FilesScreen.h"

#include <cstdio>
#include <cstring>

#include "../infra/Filesystem.h"
#include "../ui/GUI.h"
#include "FileViewScreen.h"

extern "C" {
#include "lib/oofatfs/ff.h"
FATFS* replay_get_fatfs(void);
}

extern FileViewScreen sFileView;

FilesScreen::FilesScreen(ScreenId sid) : ListMenuScreen(sid, "FILES") {}

void FilesScreen::onEnter(GUIManager& gui) {
  ListMenuScreen::onEnter(gui);
  scanDirectory("/");
}

uint8_t FilesScreen::itemCount() const { return fileCount_; }

void FilesScreen::formatItem(uint8_t index, char* buf,
                             uint8_t bufSize) const {
  if (index >= fileCount_) {
    buf[0] = '\0';
    return;
  }
  char sizeBuf[8];
  if (fileSizes_[index] < 1024)
    std::snprintf(sizeBuf, sizeof(sizeBuf), "%luB",
                  (unsigned long)fileSizes_[index]);
  else
    std::snprintf(sizeBuf, sizeof(sizeBuf), "%luK",
                  (unsigned long)(fileSizes_[index] / 1024));

  int nameLen = static_cast<int>(strlen(fileNames_[index]));
  int sizeLen = static_cast<int>(strlen(sizeBuf));
  int pad = 20 - nameLen - sizeLen;
  if (pad < 1) pad = 1;
  std::snprintf(buf, bufSize, "%s%*s%s", fileNames_[index], pad, "", sizeBuf);
}

void FilesScreen::onItemSelect(uint8_t index, GUIManager& gui) {
  if (index >= fileCount_) return;
  sFileView.loadFile(fileNames_[index]);
  gui.pushScreen(kScreenFileView);
}

void FilesScreen::scanDirectory(const char* path) {
  fileCount_ = 0;
  Filesystem::IOLock fsLock;  // serialise vs other journal writers
  FATFS* fs = replay_get_fatfs();
  if (fs == nullptr) return;

  FF_DIR dir;
  FILINFO fno;
  if (f_opendir(fs, &dir, path) != FR_OK) return;

  while (fileCount_ < kMaxFiles) {
    if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == '\0') break;
    if (fno.fattrib & AM_DIR) continue;
    strncpy(fileNames_[fileCount_], fno.fname, kMaxNameLen - 1);
    fileNames_[fileCount_][kMaxNameLen - 1] = '\0';
    fileSizes_[fileCount_] = static_cast<uint32_t>(fno.fsize);
    fileCount_++;
  }
  f_closedir(&dir);
}
