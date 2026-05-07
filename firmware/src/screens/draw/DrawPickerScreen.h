// DrawPickerScreen — top-level entry for the Draw composer.
//
// Rows:
//   0  + New (128x64)
//   1  + New (48x48)
//   2..N  saved animations (sorted by editedAt desc)
//
// CONFIRM on a saved row → enter editor for that anim.
// UP on a saved row      → context menu (Rename / Duplicate / Delete).
// CANCEL                 → leave the picker.

#pragma once

#include <vector>

#include "../Screen.h"
#include "AnimDoc.h"

class DrawPickerScreen : public Screen {
 public:
  void onEnter(GUIManager& gui) override;
  void render(oled& d, GUIManager& gui) override;
  void handleInput(const Inputs& inputs, int16_t cursorX, int16_t cursorY,
                   GUIManager& gui) override;
  ScreenId id() const override { return kScreenDrawPicker; }
  bool showCursor() const override { return false; }

  // Called by the rename TextInputScreen submit callback. The TextInputScreen
  // pops itself afterwards; control returns to this screen via its onEnter
  // (which reload()s the picker so the new name shows up immediately).
  void onRenameSubmit();

 private:
  enum class Mode : uint8_t {
    List,
    Context,   // small popup over the list row
    Confirm,   // delete confirmation
  };

  void reload();
  void moveCursor(int8_t delta);
  void enterEditorForCurrent(GUIManager& gui);
  void enterEditorNew(GUIManager& gui, uint16_t w, uint16_t h);
  void openContextMenu();
  void closeContextMenu();
  void doRename(GUIManager& gui);
  void doDuplicate(GUIManager& gui);
  void doDelete(GUIManager& gui);
  void doSetNametag(GUIManager& gui);
  bool ctxIsFullScreen() const;
  void renderList(oled& d);
  void renderContext(oled& d);
  void renderConfirm(oled& d);
  uint8_t totalRows() const;
  bool isSavedRow(uint8_t row) const { return row >= 2; }
  uint8_t savedIndex(uint8_t row) const { return row - 2; }

  static constexpr uint8_t kVisibleRows = 5;
  static constexpr uint8_t kRowHeight = 11;
  static constexpr uint8_t kTopY = 9;
  static constexpr uint16_t kJoyDeadband = 400;

  std::vector<draw::AnimSummary> entries_;
  uint8_t cursor_ = 0;
  uint8_t scroll_ = 0;
  uint32_t lastJoyNavMs_ = 0;
  Mode mode_ = Mode::List;
  uint8_t ctxCursor_ = 0;            // 0=Rename 1=Duplicate 2=Delete
  char renameBuf_[draw::kAnimNameMax + 1] = {};
  char pendingActionId_[draw::kAnimIdLen + 1] = {};
};

extern DrawPickerScreen sDrawPicker;
