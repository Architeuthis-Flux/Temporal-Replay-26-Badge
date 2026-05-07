#include "BadgeInfoViewScreen.h"

#include <cstdio>
#include <cstring>

#include "../identity/BadgeInfo.h"
#include "../ui/GUI.h"
#include "TextInputScreen.h"

extern TextInputScreen sTextInput;

namespace {

struct FieldSpec {
    const char* label;
    size_t      offset;   // offset into BadgeInfo::Fields
    size_t      cap;      // sizeof(member)
    bool        editable; // ticket_uuid + note are read-only
};

#define FS_ROW(LABEL, MEMBER, EDIT)                                         \
    { LABEL,                                                                \
      offsetof(BadgeInfo::Fields, MEMBER),                                  \
      sizeof(((BadgeInfo::Fields*)0)->MEMBER),                              \
      EDIT }

constexpr FieldSpec kRows[] = {
    FS_ROW("Name",    name,         true),
    FS_ROW("Title",   title,        true),
    FS_ROW("Company", company,      true),
    FS_ROW("Type",    attendeeType, true),
    FS_ROW("Email",   email,        true),
    FS_ROW("Web",     website,      true),
    FS_ROW("Phone",   phone,        true),
    FS_ROW("Bio",     bio,          true),
    FS_ROW("UUID",    ticketUuid,   false),
    FS_ROW("Help",    note,         false),
};
#undef FS_ROW

constexpr uint8_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

const char* readField(const BadgeInfo::Fields& f, uint8_t index) {
    if (index >= kRowCount) return "";
    return reinterpret_cast<const char*>(&f) + kRows[index].offset;
}

char* writeField(BadgeInfo::Fields& f, uint8_t index) {
    if (index >= kRowCount) return nullptr;
    return reinterpret_cast<char*>(&f) + kRows[index].offset;
}

void onEditDone(const char* /*text*/, void* user) {
    auto* self = static_cast<BadgeInfoViewScreen*>(user);
    if (self) self->onEditSubmit();
}

}  // namespace

BadgeInfoViewScreen::BadgeInfoViewScreen()
    : ListMenuScreen(kScreenBadgeInfo, "BADGE INFO") {}

uint8_t BadgeInfoViewScreen::itemCount() const { return kRowCount; }

void BadgeInfoViewScreen::formatItem(uint8_t index, char* buf,
                                     uint8_t bufSize) const {
    BadgeInfo::Fields f;
    BadgeInfo::getCurrent(f);
    const char* val = readField(f, index);
    const char* label = (index < kRowCount) ? kRows[index].label : "";
    if (val[0] == '\0') val = "\xAD";
    std::snprintf(buf, bufSize, "%-7s%s", label, val);
}

void BadgeInfoViewScreen::onItemSelect(uint8_t index, GUIManager& gui) {
    if (index >= kRowCount) return;
    if (!kRows[index].editable) return;

    BadgeInfo::Fields f;
    BadgeInfo::getCurrent(f);
    const char* current = readField(f, index);

    editIndex_ = index;
    // Cap copy at the smaller of the field cap and our scratch buffer
    // — bio is 128 bytes, all others are smaller.
    const size_t cap = kRows[index].cap < sizeof(editBuf_)
                           ? kRows[index].cap
                           : sizeof(editBuf_);
    std::strncpy(editBuf_, current, cap - 1);
    editBuf_[cap - 1] = '\0';

    sTextInput.configure(kRows[index].label, editBuf_,
                         static_cast<uint16_t>(cap),
                         &onEditDone, this);
    gui.pushScreen(kScreenTextInput);
}

void BadgeInfoViewScreen::onEditSubmit() {
    if (editIndex_ >= kRowCount) return;
    if (!kRows[editIndex_].editable) return;

    BadgeInfo::Fields f;
    BadgeInfo::getCurrent(f);
    char* dst = writeField(f, editIndex_);
    if (!dst) return;

    const size_t cap = kRows[editIndex_].cap;
    std::strncpy(dst, editBuf_, cap - 1);
    dst[cap - 1] = '\0';

    BadgeInfo::saveToFile(f);
    BadgeInfo::applyToGlobals(f);
}
