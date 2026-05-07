#include "DrawPickerScreen.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../../BadgeGlobals.h"
#include "../../hardware/Inputs.h"
#include "../../hardware/oled.h"
#include "../../infra/BadgeConfig.h"
#include "../../infra/PsramAllocator.h"
#include "../../ui/GUI.h"
#include "../../ui/OLEDLayout.h"
#include "../../ui/UIFonts.h"
#include "../TextInputScreen.h"
#include "DrawScreen.h"

extern TextInputScreen sTextInput;

DrawPickerScreen sDrawPicker;

namespace {

void onRenameDone(const char* /*text*/, void* user) {
    auto* self = static_cast<DrawPickerScreen*>(user);
    if (self) self->onRenameSubmit();
}

}  // namespace

void DrawPickerScreen::onEnter(GUIManager& /*gui*/) {
    reload();
    cursor_ = 0;
    scroll_ = 0;
    lastJoyNavMs_ = 0;
    mode_ = Mode::List;
    pendingActionId_[0] = '\0';
}

void DrawPickerScreen::reload() {
    entries_.clear();
    draw::listAll(entries_);
    std::sort(entries_.begin(), entries_.end(),
              [](const draw::AnimSummary& a, const draw::AnimSummary& b) {
                  return a.editedAt > b.editedAt;
              });
}

uint8_t DrawPickerScreen::totalRows() const {
    return 2 + (uint8_t)entries_.size();
}

void DrawPickerScreen::moveCursor(int8_t delta) {
    const uint8_t total = totalRows();
    if (total == 0) return;
    int16_t next = (int16_t)cursor_ + delta;
    if (next < 0) next = 0;
    if (next >= total) next = total - 1;
    cursor_ = (uint8_t)next;
    if (cursor_ < scroll_) scroll_ = cursor_;
    if (cursor_ >= scroll_ + kVisibleRows) scroll_ = cursor_ - kVisibleRows + 1;
}

// ── Render ────────────────────────────────────────────────────────────────

void DrawPickerScreen::render(oled& d, GUIManager& /*gui*/) {
    if (mode_ == Mode::List) renderList(d);
    else if (mode_ == Mode::Context) renderContext(d);
    else renderConfirm(d);
}

void DrawPickerScreen::renderList(oled& d) {
    OLEDLayout::drawHeader(d, "DRAW", nullptr);
    d.setFont(UIFonts::kText);

    const uint8_t total = totalRows();
    if (total == 0) {
        d.drawStr(2, 30, "no items");
        OLEDLayout::drawGameFooter(d);
        OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", "ok");
        return;
    }

    for (uint8_t i = 0; i < kVisibleRows; i++) {
        uint8_t idx = scroll_ + i;
        if (idx >= total) break;
        uint8_t y = kTopY + i * kRowHeight;
        const bool selected = (idx == cursor_);
        if (selected) {
            OLEDLayout::drawSelectedRow(d, y, kRowHeight);
            d.setDrawColor(0);
        } else {
            d.setDrawColor(1);
        }

        char line[40];
        if (idx == 0) {
            std::snprintf(line, sizeof(line), "+ New 128x64");
        } else if (idx == 1) {
            std::snprintf(line, sizeof(line), "+ New 48x48");
        } else {
            const auto& s = entries_[savedIndex(idx)];
            const char* nm = s.name[0] ? s.name : "Untitled";
            std::snprintf(line, sizeof(line), "%s  %ux%u f%u",
                          nm, (unsigned)s.w, (unsigned)s.h,
                          (unsigned)s.frameCount);
        }
        OLEDLayout::fitText(d, line, sizeof(line), 124);
        d.drawStr(3, y + d.getAscent() + 1, line);
        d.setDrawColor(1);
    }

    if (scroll_ > 0) {
        d.fillTriangle(124, kTopY + 3, 121, kTopY + 6, 127, kTopY + 6);
    }
    if (scroll_ + kVisibleRows < total) {
        uint8_t arrowY = kTopY + kVisibleRows * kRowHeight;
        d.fillTriangle(124, arrowY, 121, arrowY - 3, 127, arrowY - 3);
    }

    OLEDLayout::clearFooter(d);
    OLEDLayout::drawGameFooter(d);
    if (isSavedRow(cursor_)) {
        OLEDLayout::drawFooterActions(d, nullptr, "menu", "back", "open");
    } else {
        OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", "new");
    }
}

void DrawPickerScreen::renderContext(oled& d) {
    OLEDLayout::drawHeader(d, "DRAW", nullptr);

    const bool showNametag = ctxIsFullScreen();
    const char* labels[4] = {"Rename", "Duplicate", "Delete", "Nametag"};
    const uint8_t count = showNametag ? 4 : 3;

    const uint8_t boxW = 80;
    const uint8_t boxH = (uint8_t)(6 + count * 10);
    const uint8_t boxX = (128 - boxW) / 2;
    const uint8_t boxY = 12;
    d.setDrawColor(0);
    d.drawBox(boxX, boxY, boxW, boxH);
    d.setDrawColor(1);
    d.drawRFrame(boxX, boxY, boxW, boxH, 2);

    for (uint8_t i = 0; i < count; i++) {
        uint8_t y = boxY + 4 + i * 10;
        const bool selected = (i == ctxCursor_);
        if (selected) {
            OLEDLayout::drawSelectedRow(d, y, 10, boxX + 2, boxW - 4);
            d.setDrawColor(0);
        } else {
            d.setDrawColor(1);
        }
        d.drawStr(boxX + 6, y + 8, labels[i]);
        d.setDrawColor(1);
    }

    OLEDLayout::clearFooter(d);
    OLEDLayout::drawGameFooter(d);
    OLEDLayout::drawFooterActions(d, nullptr, nullptr, "back", "ok");
}

void DrawPickerScreen::renderConfirm(oled& d) {
    OLEDLayout::drawHeader(d, "DELETE?", nullptr);
    d.setFont(UIFonts::kText);
    d.drawStr(8, 24, "Delete this anim?");
    d.drawStr(8, 36, "This cannot be undone.");
    OLEDLayout::clearFooter(d);
    OLEDLayout::drawGameFooter(d);
    OLEDLayout::drawFooterActions(d, nullptr, nullptr, "no", "yes");
}

// ── Input ─────────────────────────────────────────────────────────────────

void DrawPickerScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                   int16_t /*cy*/, GUIManager& gui) {
    const Inputs::ButtonEdges& e = inputs.edges();

    if (mode_ == Mode::Confirm) {
        if (e.confirmPressed) {
            doDelete(gui);
            return;
        }
        if (e.cancelPressed) {
            mode_ = Mode::List;
            gui.requestRender();
            return;
        }
        return;
    }

    if (mode_ == Mode::Context) {
        if (e.cancelPressed) {
            mode_ = Mode::List;
            gui.requestRender();
            return;
        }
        if (e.confirmPressed) {
            switch (ctxCursor_) {
                case 0: doRename(gui); return;
                case 1: doDuplicate(gui); return;
                case 2:
                    mode_ = Mode::Confirm;
                    gui.requestRender();
                    return;
                case 3: doSetNametag(gui); return;
            }
            return;
        }
        const uint8_t ctxCount = ctxIsFullScreen() ? 4 : 3;
        const uint32_t now = millis();
        const int16_t dy = (int16_t)inputs.joyY() - 2047;
        if (abs(dy) > (int16_t)kJoyDeadband) {
            if (lastJoyNavMs_ == 0 || now - lastJoyNavMs_ >= 250) {
                lastJoyNavMs_ = now;
                if (dy > 0 && ctxCursor_ + 1 < ctxCount) {
                    ctxCursor_++;
                    gui.requestRender();
                } else if (dy < 0 && ctxCursor_ > 0) {
                    ctxCursor_--;
                    gui.requestRender();
                }
            }
        } else {
            lastJoyNavMs_ = 0;
        }
        return;
    }

    // Mode::List
    if (e.confirmPressed) {
        if (cursor_ == 0) enterEditorNew(gui, draw::kCanvasFullW, draw::kCanvasFullH);
        else if (cursor_ == 1) enterEditorNew(gui, draw::kCanvasZigW, draw::kCanvasZigH);
        else enterEditorForCurrent(gui);
        return;
    }
    if (e.cancelPressed) {
        gui.popScreen();
        return;
    }
    if (e.upPressed && isSavedRow(cursor_)) {
        openContextMenu();
        gui.requestRender();
        return;
    }

    const uint32_t now = millis();
    const int16_t dy = (int16_t)inputs.joyY() - 2047;
    if (abs(dy) > (int16_t)kJoyDeadband) {
        const uint32_t repeatMs = abs(dy) > 1500 ? 80 : (abs(dy) > 900 ? 160 : 300);
        if (lastJoyNavMs_ == 0 || now - lastJoyNavMs_ >= repeatMs) {
            lastJoyNavMs_ = now;
            moveCursor(dy > 0 ? 1 : -1);
            gui.requestRender();
        }
    } else {
        lastJoyNavMs_ = 0;
    }
}

void DrawPickerScreen::openContextMenu() {
    if (!isSavedRow(cursor_)) return;
    const auto& s = entries_[savedIndex(cursor_)];
    std::strncpy(pendingActionId_, s.animId, sizeof(pendingActionId_) - 1);
    pendingActionId_[sizeof(pendingActionId_) - 1] = '\0';
    ctxCursor_ = 0;
    mode_ = Mode::Context;
}

// ── Actions ───────────────────────────────────────────────────────────────

void DrawPickerScreen::enterEditorForCurrent(GUIManager& gui) {
    if (!isSavedRow(cursor_)) return;
    const auto& s = entries_[savedIndex(cursor_)];
    sDrawScreen.openExisting(s.animId);
    gui.pushScreen(kScreenDraw);
}

void DrawPickerScreen::enterEditorNew(GUIManager& gui, uint16_t w, uint16_t h) {
    sDrawScreen.openNew(w, h);
    gui.pushScreen(kScreenDraw);
}

void DrawPickerScreen::doRename(GUIManager& gui) {
    if (!pendingActionId_[0]) return;
    // Seed buffer with current name.
    for (const auto& s : entries_) {
        if (std::strcmp(s.animId, pendingActionId_) == 0) {
            std::strncpy(renameBuf_, s.name, sizeof(renameBuf_) - 1);
            renameBuf_[sizeof(renameBuf_) - 1] = '\0';
            break;
        }
    }
    sTextInput.configure("Rename", renameBuf_, sizeof(renameBuf_),
                         &onRenameDone, this);
    mode_ = Mode::List;
    gui.pushScreen(kScreenTextInput);
}

void DrawPickerScreen::onRenameSubmit() {
    if (!pendingActionId_[0] || !renameBuf_[0]) return;
    draw::AnimDoc doc;
    if (draw::load(pendingActionId_, doc)) {
        std::strncpy(doc.name, renameBuf_, sizeof(doc.name) - 1);
        doc.name[sizeof(doc.name) - 1] = '\0';
        // Mark every frame dirty so save writes ALL .fb files? No — only need
        // info.json to change. dirtyOnDisk for frames is already false; save()
        // skips them. Just update editedAt and save.
        draw::save(doc);
    }
    draw::freeAll(doc);
    pendingActionId_[0] = '\0';
    reload();
}

void DrawPickerScreen::doDuplicate(GUIManager& gui) {
    if (!pendingActionId_[0]) return;
    char newId[draw::kAnimIdLen + 1] = {};
    draw::duplicateAnim(pendingActionId_, newId, sizeof(newId));
    pendingActionId_[0] = '\0';
    reload();
    mode_ = Mode::List;
    gui.requestRender();
}

void DrawPickerScreen::doDelete(GUIManager& gui) {
    if (!pendingActionId_[0]) {
        mode_ = Mode::List;
        return;
    }
    draw::removeAnim(pendingActionId_);
    pendingActionId_[0] = '\0';
    reload();
    if (cursor_ >= totalRows()) cursor_ = totalRows() - 1;
    mode_ = Mode::List;
    gui.requestRender();
}

bool DrawPickerScreen::ctxIsFullScreen() const {
    for (const auto& s : entries_) {
        if (std::strcmp(s.animId, pendingActionId_) == 0) {
            return s.w == draw::kCanvasFullW && s.h == draw::kCanvasFullH;
        }
    }
    return false;
}

void DrawPickerScreen::doSetNametag(GUIManager& gui) {
    if (!pendingActionId_[0]) { mode_ = Mode::List; return; }

    // Load the chosen animation into the live nametag doc.
    auto* newDoc = new draw::AnimDoc();
    if (!draw::load(pendingActionId_, *newDoc) || newDoc->frames.empty()) {
        draw::freeAll(*newDoc);
        delete newDoc;
        mode_ = Mode::List;
        gui.requestRender();
        return;
    }

    adoptNametagAnimationDoc(pendingActionId_, newDoc);

    badgeConfig.setNametagSetting(pendingActionId_);
    badgeConfig.saveToFile();

    pendingActionId_[0] = '\0';
    mode_ = Mode::List;
    gui.requestRender();
}
