# Research: Firmware Second-Pass Modularization

**Branch**: `005-switch-button-ui` | **Date**: 2026-03-12
**Status**: Complete — all decisions derived from direct code audit

No external unknowns to resolve — this is a refactor of an existing, working codebase.

---

## Decision 1: Module boundary for BadgeMenu extraction

**Decision**: New `BadgeMenu.h` / `BadgeMenu.cpp` owns `renderMenu()`, `menuIndex`, `MENU_ITEMS[]`, `MENU_COUNT`, and joystick threshold navigation state (`menuNavLocked`, `MENU_NAV_THRESHOLD`).

**Rationale**: Currently `renderMenu()` lives in `BadgeDisplay.cpp` and joystick menu navigation is interleaved with hardware polling in `BadgeInput.cpp`. These are menu concerns, not display-driver or input-driver concerns. Extracting them makes the menu self-contained.

**Coupling resolved**:
- `BadgeDisplay.cpp` → `BadgeMenu.h` (calls `renderMenu()`)
- `BadgeInput.cpp` → `BadgeMenu.h` (reads/writes `menuIndex`, reads `MENU_COUNT`)

**Alternatives considered**:
- Keep in `BadgeDisplay.cpp` — rejected: display driver should not contain application-level menu logic.
- Extract to `BadgeConfig.h` — rejected: `BadgeConfig.h` is for compile-time constants, not runtime state.

---

## Decision 2: Module boundary for BadgePairing extraction

**Decision**: New `BadgePairing.h` / `BadgePairing.cpp` owns `wifiConnect()`, `osConnectWiFi()`, `osUnpairedFlow()`, `osPairedFlow()`, `rePair()`, `osRun()`.

**Rationale**: The main `.ino` sketch currently contains ~180 lines of WiFi + HTTP + pairing logic. The sketch should be a wiring file — `setup()` and `loop()` only. Extracting pairing reduces it to ~80 lines.

**Shared state access**: `qrBits`, `badgeBits`, `qrByteCount`, `badgeByteCount`, `assignedRole`, `badgeState`, `nvsWasPaired` remain defined in the main `.ino` and accessed via `extern` declarations in `BadgePairing.cpp` — consistent with how `BadgeDisplay.cpp` already externs `qrBits` and `badgeBits`.

**Alternatives considered**:
- Move shared state to a `BadgeState.h` — considered and deferred. Would require changing every consumer. The extern pattern already works.
- Keep in `.ino` — rejected: the sketch is the hardest file to navigate.

---

## Decision 3: Joystick and tilt constants to BadgeConfig.h

**Decision**: Move these constants from `BadgeInput.cpp` static locals to `BadgeConfig.h` / `BadgeConfig.h.example`:
`JOY_DEADBAND` (0.08f), `MENU_NAV_THRESHOLD` (0.5f), `JOY_CIRCLE_R` (6), `JOY_CIRCLE_CX` (100), `JOY_CIRCLE_CY` (53), `TILT_HOLD_MS` (1500).

**Rationale**: These are tuning parameters buried as `static const` values in `.cpp` files. Centralizing in `BadgeConfig.h` makes all deployment-time tunables discoverable in one place.

**Alternatives considered**:
- New `BadgeTuning.h` — over-engineering for 6 constants; rejected.

---

## Decision 4: submitPairing() decomposition

**Decision**: Extract three private (file-scope) helpers from `submitPairing()`:
1. `pairingPost(theirUID, theirAddr, &boop)` — POST /boops, immediate-confirm branch
2. `pairingPollLoop(workflowId, theirUID, theirAddr)` — 30 s poll loop with cancel check
3. `pairingCancel(theirUID)` — sends DELETE, sets IR_PAIR_CANCELLED phase

`submitPairing()` becomes a dispatcher calling these helpers. Public signature unchanged.

**Rationale**: `submitPairing()` is 105 lines with two duplicate cancel-check blocks. Each branch has distinct local variables — splitting is clean.

---

## Audit findings — out of scope this spec

| Item | Reason deferred |
|------|----------------|
| BadgeDisplay ↔ BadgeIR decoupling | irStatus is read-only from display; low friction. Deferred. |
| BadgeInput ↔ BadgeIR decoupling | Volatile flag pattern is documented inter-core contract. No change needed. |
| BadgeState centralization | Touches every module. Separate future spec. |
| irTask decomposition | 200 lines but well-commented. Deferred. |

---

## (Archived) Prior research for button-hint widget (superseded)

The original 005 spec was a Nintendo Switch-style button hint widget. That work is complete
in the codebase (diamond layout, drawCircle/drawDisc, setButtonHints API) but was replaced
by the scrollable menu as the primary UI. The following sections are kept for reference only.

---

## 1. U8G2 Circle Drawing API

**Decision**: Use `u8g2.drawCircle(cx, cy, r)` for outlined state and `u8g2.drawDisc(cx, cy, r)` for filled state.

**Findings**:
- `U8G2::drawCircle(x0, y0, rad, opt)` — draws outlined circle. `opt` defaults to `U8G2_DRAW_ALL`.
- `U8G2::drawDisc(x0, y0, rad, opt)` — draws filled circle (disc).
- Both are available in the full-buffer (`_F_`) driver used by this project (`U8G2_SSD1306_128X64_NONAME_F_HW_I2C`).
- Full-buffer mode redraws the entire frame each cycle — no incremental update constraints.

**Inverse text on filled disc**:
- U8G2 `setDrawColor(0)` sets "black" (clear) draw color.
- Sequence: draw white disc (`setDrawColor(1)` + `drawDisc`), then draw black label (`setDrawColor(0)` + `drawStr`), then reset to `setDrawColor(1)`.
- Full-buffer mode ensures the disc is already in the buffer when the text renders over it.

**Alternatives considered**:
- XBM bitmaps per button state — more bytes, harder to update labels dynamically; rejected.
- `drawFrame` (rectangular) — does not match the Switch aesthetic; rejected per spec.

---

## 2. Layout: Circle Center Positions & Collision Analysis

**Decision**: Reuse existing button indicator coordinates as circle centers, with radius 4.

**Existing indicator positions** (from `BadgeInput.cpp`):

| Button | Current indicatorX | Current indicatorY | Circle center |
|--------|-------------------|--------------------|---------------|
| UP     | 118               | 48                 | (118, 48)     |
| DOWN   | 118               | 58                 | (118, 58)     |
| LEFT   | 113               | 53                 | (113, 53)     |
| RIGHT  | 123               | 53                 | (123, 53)     |

These already form a diamond. With r=4 (diameter 8):

**Diamond geometry**:
- Vertical span: UP top edge y=44 to DOWN bottom edge y=62 — 18px
- Horizontal span: LEFT left edge x=109 to RIGHT right edge x=127 — 18px
- All circles within 128×64 screen bounds (rightmost x=127, bottommost y=62 ✓)

**Collision check against existing chrome**:

| Chrome element | Bounding box | Nearest widget circle | Gap |
|---------------|-------------|----------------------|-----|
| Joystick disc | center (100,53), r=6 → x:94–106, y:47–59 | LEFT circle x:109–117 | 3px horizontal ✓ |
| Tilt indicator | (84,48), 4×5 → x:84–88, y:48–53 | LEFT circle x:109–117 | 21px ✓ |
| IR arrows      | (55–74, 0–6)                       | All circles y≥44      | 38px vertical ✓ |
| Status text    | y≤42 (debug line bottom)           | All circles y≥44      | 2px ✓ |

No overlaps. The 2px gap between debug line (y=42, font height ~6 → bottom ~48) and UP circle (top y=44) is tight — **implementation must verify** the debug line does not extend to y>43.

**Rationale**: Reusing the existing positions avoids a layout redesign. They were already placed to avoid chrome conflicts; the circle upgrade is purely cosmetic.

---

## 3. Font Selection & Character Centering

**Decision**: `u8g2_font_4x6_tr` — the font already in use for chrome elements.

**Findings**:
- `u8g2_font_4x6_tr`: 4px wide, 6px tall (cap height ~5px above baseline).
- Already loaded in current `renderMain()` code — no additional font loading cost.
- Single ASCII printable characters (A–Z, 0–9, symbols) fit within an 8px-diameter circle:
  - Horizontal: 4px char in 8px circle → 2px margin each side ✓
  - Vertical: 5px cap height in 8px circle → ~1.5px margin top/bottom ✓

**Character centering formula** (U8G2 `drawStr` uses bottom-left of baseline):
- `drawStr(cx - 2, cy + 2, label)` — places a 4px-wide char 2px left of center, baseline 2px below center.
- This gives visual center for capitals: ascent ~5px → top of char at approximately cy - 3. Acceptable.
- Exact pixel alignment is a build-time visual check; formula may need ±1 tuning.

**Alternatives considered**:
- `u8g2_font_micro_tr` (3×5) — harder to read at distance; rejected.
- `u8g2_font_5x7_tr` — too large for r=4 circle; rejected.

---

## 4. Default Label Mapping

**Decision**: Main-screen defaults: UP="B", DOWN="X", LEFT="Q", RIGHT="P".

| Button | Action (main screen)     | Default label | Rationale |
|--------|--------------------------|---------------|-----------|
| UP     | IR boop send             | "B"           | Boop |
| DOWN   | Cancel / stop listening  | "X"           | Cancel/close convention |
| LEFT   | Toggle QR code view      | "Q"           | QR |
| RIGHT  | WiFi reconnect / re-pair | "P"           | Pair |

**Spec note**: Exact defaults are deferred to implementation per spec assumption. These are a starting proposal; the `setButtonHints()` API means they are trivially changed without touching draw code.

**Full-screen omission**: `renderQR()` and `renderNametag()` do not call `drawButtonHints()` — the widget is simply absent from those code paths. No special guard needed in the widget itself.

---

## 5. Struct Cleanup: `indicatorX` / `indicatorY` Fields

**Decision**: Remove `indicatorX` and `indicatorY` from the `Button` struct in `BadgeInput.h/.cpp`; move positions into `BadgeDisplay.cpp`.

**Rationale**: These fields exist solely to tell `BadgeDisplay` where to draw the indicators. With the circle positions hardcoded in `BadgeDisplay.cpp` (derived from the same values), the struct fields are dead weight. Removing them keeps `BadgeInput` focused on input state.

**Impact**: `BadgeDisplay.cpp` currently accesses `buttons[i].indicatorX` / `buttons[i].indicatorY`. After removal, it will use local `const int cx[4]` / `const int cy[4]` arrays instead.

---

## 6. Context-Aware Labels API (P2)

**Decision**: Module-level `g_hintLabels[4]` with a `setButtonHints()` setter in `BadgeDisplay`.

```cpp
// BadgeDisplay.h
void setButtonHints(const char* up, const char* down, const char* left, const char* right);

// BadgeDisplay.cpp
static const char* g_hintLabels[4] = {"B", "X", "Q", "P"};

void setButtonHints(const char* up, const char* down, const char* left, const char* right) {
    g_hintLabels[0] = up;
    g_hintLabels[1] = down;
    g_hintLabels[2] = left;
    g_hintLabels[3] = right;
}
```

**Rationale**: Simple, zero-overhead. The pointers are expected to be string literals with static lifetime — no heap allocation, no copy semantics needed. Any screen-transition code can call `setButtonHints(...)` before triggering a re-render.

**Inactive button representation** (FR-007 / User Story 2, scenario 3):
Pass `""` (empty string) or `"-"` as a label to signal an inactive button. The draw function will skip the `drawStr` call if `label[0] == '\0'`, rendering only an outlined circle with no label. A dimmed appearance (e.g., draw only top half of circle via `opt`) is possible but deferred — an empty circle is sufficient for P1.

---

## 7. `screenDirty` Flag Behavior

**Finding**: `pollButtons()` sets `screenDirty = true` on any state change. The render loop checks `screenDirty` before calling `renderScreen()`. This means:
- Press: dirty → render → circle fills ✓
- Release: dirty → render → circle returns to outlined ✓
- SC-003 (fill within one refresh cycle ~50ms) is met by the existing dirty-flag mechanism.

No changes needed to the dirty-flag or loop timing.
