# 04 — Keyboard polish

Goal: extend `TextInputScreen` with cancel, a proper Grid/QWERTY
toggle (with Settings persistence), cleaner mouse mode, emoji
support via u8g2's unifont glyph range, and a canned-message
preset list for quick replies.

## Context — what exists

[`../src/GUI.cpp`](../src/GUI.cpp) `TextInputScreen` (search
`void TextInputScreen::onEnter`):

```cpp
enum class Layer : uint8_t { Lower, Upper, Digits, Symbol, kCount };
enum class Mode  : uint8_t { Grid, Mouse };

static constexpr uint8_t  kGridCols   = 10;
static constexpr uint8_t  kGridRows   =  4;
static constexpr uint16_t kJoyDeadband = 500;
```

Layer grids live in a big `kKeyGrid[Layer::kCount][4][10]` table
just above the class impl (grep for `kKeyGrid`). Currently:

- UP short = `cycleLayer()` (lower → upper → digits → symbol).
- UP held ≥ 800 ms = toggle `Mode::Grid` ↔ `Mode::Mouse` (mouse
  mode exists but isn't documented anywhere the user can find).
- DOWN = submit, calls `onDone_` and `popScreen()`.
- LEFT = backspace.
- RIGHT = commit highlighted char.
- Joystick = move cursor / move mouse.

Configure entry point:
`TextInputScreen::configure(title, buffer, capacity, onDone, user)`.

Callers (grep `sTextInput.configure`):

- `ContactDetailScreen` field edits (`[](const char* text, void* u){...}`
  lambda callback).
- `BadgeInfoEditScreen` field edits (`onFieldEditDone`).
- `SendBoopBackground::launchBodyEditor` (`onSendBoopBodyDone`).

### Config system

[`../src/BadgeConfig.h`](../src/BadgeConfig.h) `enum SettingIndex`:
each setting is an integer index with a matching `kDefs[]` row in
[`../src/BadgeConfig.cpp`](../src/BadgeConfig.cpp). Add new keys
at the end of the enum + matching `kDefs[]` line + a handler in
`Config::apply()` if the setting needs live-reaction. Config loads
from `/settings.txt` and hot-reloads via `ConfigWatcher`.

## Goals

1. **Cancel**: a way to abort without committing. Today pressing
   LEFT on an empty buffer is a no-op; there's no cancel path
   that calls something like `onCancel_(user)` or pops without
   firing `onDone_`. Add an explicit cancel gesture so a user
   who entered the keyboard by accident can back out without
   committing a typo.
2. **Grid vs QWERTY**: add a `kbLayout` setting (0=Grid,
   1=QWERTY). Grid is the current 4×10 alphabet-order grid;
   QWERTY is a phone-style layout. Runtime-switchable via
   Settings + live-reload.
3. **Mouse mode as a discoverable toggle**: surface the current
   hidden UP-long-press toggle in Settings (key `kb_mouse`,
   0=off/Grid-only, 1=on/allow toggle). Possibly simplify: always
   on for the user who explicitly enables it; otherwise always
   Grid. Also: keep an in-screen hint "hold ^ for mouse".
4. **Emoji layer** using u8g2 unifont glyphs. Add a 5th `Layer`
   value `Emoji` that renders emoji codepoints in a grid.
   Committing an emoji inserts its UTF-8 bytes into the buffer
   (rendering in receivers requires the same font — see notes).
5. **Canned-message presets**: a quick-send list of ~8 canned
   strings surfaced as a dedicated screen or a long-press on some
   keyboard key. Suggested defaults: "where you at?", "on my
   way", "lol", "👋", "missed you", "lfg", "send help", "wyd?".
   Selecting a preset commits the string into the buffer
   in-place (append) or replaces the buffer, TBD (see Open
   questions).
6. **General polish** — sharper label positioning, selected-cell
   inversion consistency with HomeScreen's pattern, explicit
   "< cancel > submit" footer hint, press haptic on key commit.
7. **Larger, more readable text** — the whole screen is currently
   drawn with `FONT_TINY` (~4×6). Bump the typed-buffer echo
   line (and ideally the key glyphs) up to `FONT_SMALL` (~6×10)
   or larger if we can re-flow the grid to still fit 128×64.
   This is the single biggest legibility win available on the
   keyboard surface today.

## Research to do first

- **u8g2 unifont emoji**: the wiki page the user linked —
  <https://github.com/olikraus/u8g2/wiki/fntgrpunifont> —
  lists the unifont glyph groups. Key ones for us:
  `u8g2_font_unifont_t_emoticons`,
  `u8g2_font_unifont_t_symbols`, plus the base
  `u8g2_font_unifont_*` for CJK etc.
  Glyph size in unifont is 16×16 and 8×16 — we've been drawing
  with FONT_TINY (4×6). An emoji layer at 16×16 eats a lot of
  screen; budget ~3 columns × 2 rows per visible grid page.
- How the current `oled` class selects fonts: see
  [`../src/oled.h`](../src/oled.h) `setFontPreset`. Adding a
  dedicated "emoji font" preset may be the cleanest path; or
  temporarily override the font for just the emoji grid render.
- **Phone keyboard layouts**: look at common 10-col × 4-row
  QWERTY renderings for low-res displays. Key rows typically:
  `q w e r t y u i o p`
  `a s d f g h j k l '`
  `z x c v b n m , . ?`
  `SHIFT SPACE SPACE ENTER` — but we use UP for shift-like
  (layer cycle), DOWN for submit, so the bottom row is freer.
- Canned-message UX on phones: iOS "Tapbacks" / Android quick
  replies — usually a pop-up from a dedicated key. For us,
  consider a dedicated on-screen icon key that pushes a small
  menu.
- Any community u8g2 QWERTY keyboard implementations on GitHub;
  there's usually someone who's already figured out the tight
  layout on a 128×64 panel.

## Design sketch

### A. Cancel (shipped — LEFT-on-empty instead of long-press)

Shipped approach: **LEFT on an empty buffer** cancels —
pops without firing `onDone_`. Cheaper than a long-press
timer and more discoverable: after a user has already
backspaced everything away, one more LEFT means "get me out
of here." On a fresh editor entry (accidental keyboard
open) one LEFT immediately cancels.

All three callsites tolerate this cleanly — `ContactDetail`
and `SendBoop` pass scratch buffers, and `BadgeInfoEdit`
requires an explicit "Save" menu item to persist so the
pop just drops the in-memory edit. No `onCancel_` callback
plumbing needed.

See `TextInputScreen::handleInput` in
[`../src/GUI.cpp`](../src/GUI.cpp) — `e.leftPressed &&
len_ == 0 → gui.popScreen()`.

**Originally planned** (superseded): long-press LEFT
(≥ 800 ms) with a `leftHoldStartMs_` timer and an
`onCancel_` callback — more code, same user outcome.

### B. Grid vs QWERTY toggle (shipped — default is now Qwerty)

Done in full. Architecture:

- `TextInputScreen::Layout` enum (`Legacy`, `Qwerty`) with a
  `layout_` member read from `badgeConfig.get(kKbLayout)` on
  `onEnter`.
- Setting added to `BadgeConfig.h` enum + `kDefs[]` row in
  `BadgeConfig.cpp` as `{"kb_layout","Keyboard",1,0,1,1}` —
  default 1 (Qwerty) so new installs get the new UX
  immediately; set to 0 in `settings.txt` to fall back to the
  old alphabet-order grid.
- `render()` and `handleInput()` are thin dispatchers that call
  `renderLegacy`/`renderQwerty` and `handleInputLegacy`/
  `handleInputQwerty` respectively. The Legacy path is the
  existing code moved unchanged; the Qwerty path is new.
- New D-pad mapping in Qwerty:
  - `<` LEFT = exit (pops without firing `onDone_`; the
    caller's buffer retains the draft so you can leave and
    come back)
  - `>` RIGHT = commit selected cell (letter or action)
  - `^` UP short = cycle layer forward
  - `^` UP held ≥ 800 ms = toggle Grid ↔ Mouse mode (with
    haptic pulse at the threshold)
  - `v` DOWN = cycle layer backward
  - joystick = navigate selector (Grid) or absolute
    cursor (Mouse)
- New letter / digit / symbol grid (`kKeyGridQwerty`):
  - Lower: `q w e r t y u i o p / a s d f g h j k l ' /
    z x c v b n m , . ?`
  - Upper: capitals with secondary punctuation (`" ! ? :`)
  - Digits / Symbol: 3 rows of typical phone-keyboard
    number-and-punctuation glyphs
- Persistent bottom action row (row 3, same in every
  content layer):
  `SHF | 123/abc | em | space×4 | DEL | SEND×2`
  - SHF: tap → one-shot capitalization (indicator `'`
    next to the layer tag); double-tap inside 400 ms or
    re-tap when one-shot is active → caps-lock (indicator
    `"`); third tap clears.
  - 123 / abc: flips between letter-track and digit-track.
    Dynamic label.
  - em: Phase-1 stub; cycles to Symbol layer (see §D below).
  - Space / DEL / SEND: the verbs that used to live on the
    D-pad.

Layout data for QWERTY (10×4):

```
Lower:   q w e r t y u i o p
         a s d f g h j k l '
         z x c v b n m , . ?
         ⇧ _ _ _ _ _ _ _ _ ⏎

Upper:   Q W E R T Y U I O P   (mirror lower but caps)
         ...

Digits:  1 2 3 4 5 6 7 8 9 0
         ...

Symbol:  ! @ # $ % ^ & * ( )
         ...
```

The bottom row's "⇧" can be a visual-only cell (UP already
cycles layer), or swapped for common shortcut chars like
`@ ! space space space space ? , .`.

### C. Mouse mode toggle

The existing UP-long-press toggle is staying in — just
document it in the in-screen footer hint and add a Settings
key `kb_mouse` that gates whether the long-press toggle does
anything at all. Default on.

### D. Emoji layer

Biggest piece. Steps:

1. Add `Layer::Emoji` before `kCount` in the enum (so cycleLayer
   stops at Symbol → Emoji → back to Lower).
2. Add an **emoji page** data table: array of const char*
   UTF-8 sequences, e.g. `"\xF0\x9F\x91\x8B"` for 👋. Target
   ~32 emoji to start — enough for a single scrolled page.
3. Rendering: when `layer_ == Layer::Emoji`, switch to a larger
   font preset that has emoji glyphs (new
   `FONT_EMOJI` → `u8g2_font_unifont_t_emoticons` or similar),
   draw a 3×3 (or 4×2) grid, each cell ~16×16 px. Cursor + commit
   work same as before.
4. Commit path: `commitChar` extended to `commitBytes(const char*)`
   so a multi-byte UTF-8 emoji writes all bytes into `buf_` and
   advances `len_` by that many.
5. Note to verify: the RECEIVING badge also needs the same emoji
   font loaded for `ThreadDetailScreen` to render it correctly.
   Otherwise the bubble shows mojibake. If we don't ship the
   emoji font everywhere, constrain emoji to a small set that
   falls back cleanly (e.g. use `[emoji]` textual placeholders in
   plain-text locations).

### E. Canned messages

1. New screen: `CannedReplyScreen` (or just reuse
   `ListMenuScreen` with a static items table).
2. Push it from within `TextInputScreen` on a dedicated key (pick
   an unused cell — maybe Symbol layer's unused corner). Or add
   a separate Settings entry "Canned replies" that lists them
   for editing.
3. Selecting a preset copies the string into `TextInputScreen`'s
   buffer and returns. Decide: APPEND to existing text or
   OVERWRITE? (Ask user.)

Preset table (starter set):

```cpp
static const char* kCannedReplies[] = {
  "where you at?",
  "on my way",
  "lol",
  "missed you",
  "lfg",
  "send help",
  "wyd?",
  "nm you?",
};
```

Store in `BadgeConfig` or as a compile-time array? If editable
from Settings, they need a persisted store — probably new file
`/canned.txt` with one line per preset. For a v1, compile-time
is fine.

### F. Visual polish

- **Help as the last layer** (shipped): rather than a bottom
  footer hint — which fought for vertical pixels against
  bigger key glyphs — a dedicated `Layer::Help` renders a
  button-binding guide in the grid area itself. Reached by
  cycling UP past Symbol (layer tag becomes `?`). On this
  layer, RIGHT / joystick do nothing (no selector); UP
  cycles back to Lower; DOWN still submits; LEFT still
  backspaces, so the guide's description of every other
  key remains truthful while you're reading it. See
  `TextInputScreen::render` in [`../src/GUI.cpp`](../src/GUI.cpp).
- **Single-pixel lattice** (shipped): the grid is drawn as
  one outer rect + interior `drawVLine`/`drawHLine` calls,
  sharing borders between cells. The old per-cell
  `drawFrame` stacked each cell's edges with its neighbor's,
  giving a visually 2 px thick inter-cell border; now it's
  a clean 1 px. Selected cell invert fills only
  `(x+1, y+1, cellW-1, cellH-1)` so the lattice stays
  continuous through the selection.
- On commit: `Haptics::shortPulse()` for tactile confirmation.
- Layer label in top-right — already present, now has a `?`
  value for the Help layer.

### G. Larger text / tighter vertical budget

Current layout (128×64, everything `FONT_TINY` ~4×6):

```
y=0..6   title row (FONT_TINY baseline y=6)
y=8      divider hline
y=16     typed-buffer echo baseline (FONT_TINY)
y=24..63 keyboard grid: 4 rows × 10 cols, cellW=12, cellH=10
```

Cells are `12 × 10` — a `FONT_SMALL` (~6×10) glyph centers in
that footprint with room for a 3 px left margin, so the key
labels can be upsized with **zero** geometry change. The win
is huge because the typed buffer is the line the user is
actually reading.

Two incremental passes, pick either or both:

1. **Typed-buffer only (safe, ~5 min)**: wrap the `buf_` draw
   at `y=16` in `d.setFontPreset(FONT_SMALL); ...;
   d.setFontPreset(FONT_TINY);`. `FONT_SMALL` glyphs are
   ~10 px tall, so baseline shifts to `y=18` and `showStart`
   cap drops from 20 chars → ~14 chars wide at 128 px. Still
   plenty for a message preview; long messages already scroll
   via `len_ > showCap` slicing. No grid reflow needed.
2. **Key labels too (medium, affects hit cells)**: also switch
   the inner grid-render loop to `FONT_SMALL`. Because cells
   are 10 px tall and `FONT_SMALL` glyphs are ~10 px, the
   `y + cellH - 2` baseline math needs a 1 px bump so
   descenders on `g j p q y ,` don't clip. Inspect the
   `,` `;` `'` cells visually after the change — any clip
   means drop baseline by 1 more px or tighten cell to
   cellH=11 and rebuild the grid at `kbY=20` (steal 4 px
   from the typed-buffer gap).

Geometry budget for option 2 (`cellH=11`, `kbY=20`):

```
y=0..6   title (FONT_TINY)
y=8      divider
y=10..19 typed buffer (FONT_SMALL, baseline y=18)
y=20..63 grid: 4 rows × 11 px = 44 px, cells 12×11
```

That's flush against the bottom row — no footer hint fits.
So the `< cancel > submit` footer (goal F) and the bigger
keys (goal G option 2) trade off against each other. If
footer hint wins, stay on `cellH=10` and only upsize the
typed buffer. Call the user's preference in open questions.

Optional: an `FONT_SMALL` title row too. Title is currently 1
line at `y=6`; `FONT_SMALL` baselines would land at `y=10`,
pushing the divider to `y=12`. Probably not worth the vertical
cost unless we also shrink the grid to 3 rows, which defeats
the QWERTY work in goal B.

## Implementation checklist

Split into small PRs:

- [x] **Cancel** (shipped as LEFT-on-empty, not long-press):
      pressing LEFT when `len_ == 0` pops without firing
      `onDone_`. No `onCancel_` callback plumbing needed
      because none of the three callsites persist until an
      explicit commit path (their own "Save" menu item or
      the `onDone_` firing). See design sketch §A.
- [x] **Commit haptic** (Qwerty only): every Qwerty commit
      path (letter, space, backspace, shift, layer cycle,
      emoji stub, submit) fires `Haptics::shortPulse()` for
      tactile confirmation. Pulses fire only on the RIGHT-
      press edge, never on joystick ticks. Legacy layout
      unchanged — has no haptic on commit, which matches
      its original behavior.
- [~] **Footer hint** (superseded): a bottom-bar
      `< cancel  > submit` hint was considered but replaced
      by the Help layer (goal F above), which puts ALL button
      bindings in one discoverable place without stealing
      vertical pixels from the grid. No footer strip will be
      added.
- [x] **Typed-buffer legibility** (goal G option 1): render
      the `buf_` echo line with `FONT_SMALL` instead of
      `FONT_TINY`. Adjust baseline to `y=18`, reduce the
      rolling-window `showStart` cap from 20 → ~14 chars.
      Zero grid reflow. Done — see `TextInputScreen::render`
      in [`../src/GUI.cpp`](../src/GUI.cpp) around the
      `buf_` draw. Font is saved-and-restored inside the
      `if (buf_)` block so the grid loop keeps
      `FONT_TINY` unchanged.
- [x] **Key-label legibility** (goal G option 2): grid render
      loop switched to `FONT_SMALL`, baseline kept at
      `y + cellH - 2`. Descenders on `g j p q y` land on the
      shared bottom grid line — drawn with
      `setDrawColor(2)` (XOR) so those overlapping pixels
      punch a 1-px notch into the separator line instead of
      being absorbed by it. The descender reads as an
      "anti-pixel" cut through the line. The XOR color also
      removes the old explicit `setDrawColor(0)` flip for
      selected cells: XOR on a filled interior naturally
      renders dark glyph on lit fill. No `cellH` bump needed.
      The footer-hint/key-size conflict flagged in goal G is
      resolved by *not* shipping the footer — see goal F
      "Help as the last layer" above.
- [x] **Grid vs QWERTY**: shipped — `kKbLayout` config key
      added with default 1 (Qwerty), new `Layout` enum in
      `TextInputScreen`, content tables `kKeyGrid` (Legacy)
      and `kKeyGridQwerty` (new). Both layouts fully
      functional; Legacy reachable by writing `kb_layout = 0`
      to `settings.txt`. See design sketch §B.
- [~] **Mouse mode setting**: superseded — mouse mode is
      always reachable via UP-long-press on the Qwerty layout;
      the old `kKbMouseEnable` gate is unnecessary now that
      the UP-hold detection is repeat-immune (see the
      `Inputs::heldMs()` fix and the `upHoldFired_` guard in
      `handleInputQwerty`). No new config key needed.
- [x] **Emoji layer (Phase 2 — shipped)**:
  - `FONT_EMOJI` preset added to
    [`../src/oled.h`](../src/oled.h) /
    [`../src/oled.cpp`](../src/oled.cpp), mapped to
    `u8g2_font_unifont_t_emoticons`. Also added
    `drawUTF8` / `getUTF8Width` wrappers so the emoji
    render path can decode multi-byte Unicode.
  - `Layer::Emoji` inserted between `Symbol` and `Help`
    in the enum. `cycleLayer` / `cycleLayerBack` now
    skip it on the Legacy layout (Legacy's `kKeyGrid`
    has no emoji table and its D-pad can't commit
    multi-byte glyphs) but include it in Qwerty's cycle.
  - Emoji cells are 15×15 (vs text 12×10) to fit the
    16-wide unifont glyphs. Layout: 8 cols × 2 rows = 16
    visible emoji per page in the `y = 24..53` region;
    the shared action row at `y = 54..63` is unchanged.
  - `kEmojiUtf8[16]` table holds raw UTF-8 byte sequences
    for U+1F600..U+1F64F codepoints (emoticons block).
    Examples: 😀 = `\xF0\x9F\x98\x80`, 🙏 = `\xF0\x9F\x99\x8F`.
  - The `emoji` action cell toggles into / out of
    `Layer::Emoji` (tap while there to return to Lower).
    The cell's label flips from `emoji` to `abc` when on
    the emoji layer, for a clear "go back" affordance.
  - `commitBytes(const char*)` inserts a multi-byte glyph
    at `cursorPos_`, advancing `cursorPos_` by the byte
    count. `commitChar(char)` is now a thin wrapper
    around it.
  - `backspace()` is UTF-8-aware: walks back over
    continuation bytes (`0x80..0xBF`) until a lead byte,
    then shifts the tail. A single emoji press now
    deletes its full 4 bytes instead of leaving mojibake
    trailers.
  - Joystick nav on Emoji rows 0..1 clamps to 8 cols;
    vertical nav skips row 2 (dead zone) and jumps
    straight from row 1 to row 3 (action row).
  - **NOT shipped in Phase 2 but needed to be fully
    useful**: `ThreadDetailScreen` still renders in
    `FONT_SMALL` (ASCII-only). Emoji bytes transmitted
    to receivers will show as mojibake in the bubble
    unless that screen switches to `FONT_EMOJI` for
    emoji runs. Flagged as a follow-up.
- [x] **Edit-in-place / text-editing cursor (Phase 2 —
      shipped)**:
  - `cursorPos_` (uint16_t byte offset) added to
    `TextInputScreen`. `configure` / `onEnter` reset it
    to `len_` (append mode) so existing call sites keep
    working unchanged.
  - `commitBytes` / `commitChar` insert *at* `cursorPos_`
    and slide the tail right with `memmove`.
  - `backspace` deletes the glyph *before* `cursorPos_`
    (UTF-8-aware) and slides the tail left.
  - `cursorMoveLeft` / `cursorMoveRight` walk the cursor
    one codepoint at a time.
  - Mouse-mode `kMouseMinY` lowered from 24 → 10, letting
    the joystick push the cursor *above* the keyboard
    into the typed-buffer echo region. When `mouseY_ < kbY`
    the render suppresses the key highlight and drives
    `cursorPos_` directly from `mouseX_` (assumes ~6 px
    per FONT_SMALL glyph — rough but good enough for
    ASCII; UTF-8 glyph-walk refinement is a follow-up).
  - Typed-buffer echo now draws a blinking caret (~3 Hz)
    at `cursorPos_` via `getUTF8Width` of the prefix.
    The visible window slides to keep the cursor
    onscreen — centered when possible, pinned to start
    or end otherwise.
- [ ] **Draft preservation (Phase 3 glue)**: the Qwerty
      LEFT handler already pops without firing `onDone_`,
      so the caller's buffer is preserved — but callers
      like `SendBoopBackground::launchBodyEditor` clear
      the buffer at the *start* of each launch
      (`s_send.body[0] = '\0';`). For
      "leave the keyboard, look at the thread, come back
      and keep composing" to work, the caller needs to
      stop clearing on re-entry. Fix is caller-side; the
      keyboard already cooperates.
- [ ] **Cursor nav via D-pad for Grid mode**: text-cursor
      editing currently only works in Mouse mode (joystick
      above keyboard). For Grid-mode users, left/right
      joystick pushes on row 0 could extend to a
      "text-cursor mode" that moves `cursorPos_` glyph by
      glyph. Not shipped in Phase 2.
- [ ] **UTF-8 glyph-accurate mouse→cursor mapping**: the
      current mapping assumes 6 px per glyph (FONT_SMALL
      ASCII). Emoji glyphs in the buffer throw this off —
      they're 16 px wide when rendered but only 4 bytes
      in the buffer, and the mouse-to-byte math doesn't
      know to skip continuation bytes. Fine while
      editing pure ASCII, rough when mixing emoji.
- [ ] **Receiver-side emoji render in ThreadDetailScreen**:
      bubble text is drawn in FONT_SMALL which can't
      render UTF-8 emoji. Need to detect emoji byte runs
      (lead byte `>= 0xF0`) and switch to FONT_EMOJI for
      those spans, then back to FONT_SMALL for text.
- [ ] **Canned replies**: implement as a new screen or a
      push-on-dedicated-key flow.
- [ ] Update
      [`../codeDocs/NotificationSystem.md`](../codeDocs/NotificationSystem.md)
      / add a new `codeDocs/KeyboardSystem.md` if the surface
      grows big enough to justify.

## Open questions for the user at start

- **Cancel gesture**: LEFT long-press (≥ 800 ms) vs UP+DOWN combo
  vs a dedicated on-screen `< cancel` cell. Pick one.
- **Emoji fallback**: receivers that DON'T have the emoji font
  loaded get mojibake — acceptable cost? Alternative: send emoji
  as `[wave]` / `[lol]` text tokens and render the glyph only
  when the receiving font supports it. More conservative.
- **Canned replies behavior**: append to existing buffer or
  overwrite? Append feels safer (user doesn't lose typed text),
  overwrite is simpler.
- **QWERTY layout**: use the traditional `q w e r t y ...` first
  row or a compressed 2-row landscape? Phone keyboards use 3
  rows of 10 keys, which fits our 4×10 grid naturally with the
  4th row free for shift/space/enter.
- **Which presets** to ship in the v1 canned-reply list? Above
  is a starter; user can prune / edit.
- ~~**Footer hint vs bigger keys**~~ (resolved): Help-layer
  approach (goal F) replaces the footer and gets the bigger
  keys (goal G) for free. No tradeoff needed.

## Testing approach

- Keyboard screen accessible from any TextInput caller:
  - Send Boop body entry (Main Menu → Send Boop → pick
    contact).
  - Contact field edit (Contacts → pick → RIGHT → pick field →
    RIGHT).
  - Badge info field edit.
- Cycle layers (UP short press) — confirm emoji layer reachable.
- Type a 3–4 word message across multiple layers, including an
  emoji. Send to the other badge, confirm it renders (or
  degrades acceptably) in the ThreadDetail bubble.
- Toggle QWERTY vs Grid via Settings — confirm layout flips on
  next TextInput entry without a reboot.
- Cancel an entry — confirm buffer isn't changed upstream (e.g.
  enter Contact Name edit, press cancel, verify the contact's
  name in `/boops.json` is untouched).

## Conventions to preserve

- `TextInputScreen` must remain callable from anywhere via
  `sTextInput.configure(...)` — don't break the existing three
  call sites.
- Font-switch discipline: wrap any `setFontPreset(FONT_EMOJI)`
  in a save/restore around the emoji region. Screens further
  down the stack assume FONT_TINY is the steady-state.
- UTF-8 safety: never split a multi-byte codepoint in
  `backspace()`. The existing backspace drops one raw byte at a
  time — fine for ASCII; with emoji it would strip off the
  final continuation byte of a UTF-8 sequence. Fix this as
  part of the emoji layer work (walk back until the previous
  byte has MSB cleared, or is a lead byte).
