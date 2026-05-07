# 03 — Chat bubble UI for ThreadDetailScreen

Goal: replace the current `< body` / `> body` single-line rows with
phone-texting-style chat bubbles — outgoing right-justified, incoming
left-justified, rounded-rectangle fills, word-wrapped bodies.

## Context — what exists

### Renderer today

[`../src/GUI.cpp`](../src/GUI.cpp) `ThreadDetailScreen::render`
(search: `// ThreadDetailScreen — chat-style message list for a
single thread`). Current layout:

- Row 0..10: header band — thread display name in FONT_SMALL with
  an underline at row 12.
- Row 13..: message list. Each message renders as a single
  128-px-wide row of height `kRowHeight = 9`, containing
  `<arrow> <body-preview>` where arrow is `>` for outgoing and
  `<` for incoming. Selected row inverts fill.
- Row 62: footer `< back` always, `> reply` when the thread has
  a primary participant.

Up to 5 rows visible; `fetchMessage(threadId, i)` returns the
`i`th message newest-first (physical index reverse-mapped inside
`BadgeNotifications::fetchMessage`).

Input: UP/DOWN scroll, RIGHT = reply (open keyboard), LEFT short =
pop, LEFT long ≥ 800 ms = delete thread.

### Drawing primitives available

The `oled` class wraps u8g2 in
[`../src/oled.h`](../src/oled.h) / `../src/oled.cpp`. Relevant
calls that already exist and work:

- `drawBox(x,y,w,h)` — filled rectangle.
- `drawHLine(x,y,w)` / `drawVLine(x,y,h)` — used by local helper
  `drawFrame` (see [`../src/GUI.cpp`](../src/GUI.cpp) L39).
- `drawStr(x,y,str)` — font-aware text draw. Uses whatever font
  preset was last set via `setFontPreset(FONT_TINY|SMALL|LARGE|XLARGE)`.
- `drawNametag(...)` — the contact-card pattern in
  `ContactDetailScreen`.
- `setDrawColor(0|1|2)` — 0=clear, 1=set, 2=XOR. XOR tripped us
  up on HomeScreen against the Graphics_Base chrome; read that
  history in
  [`../codeDocs/NotificationSystem.md`](../codeDocs/NotificationSystem.md)
  §Home screen before using it here.

What's NOT wired up but is available in u8g2 upstream (see
research below): `drawRBox(x,y,w,h,r)` (filled rounded box),
`drawRFrame(x,y,w,h,r)` (rounded outline). Both take a corner
radius `r` in pixels. If `oled` doesn't expose them yet, add
thin wrappers in `oled.h`/`oled.cpp` following the same pattern
as `drawBox`.

### Font metrics

`d.getAscent()`, `d.getDescent()`, `d.getLineHeight()`,
`d.getStrWidth(str)`. Use these to size bubbles to fit the text
— don't hard-code widths.

## Goals

Incoming (from peer):

- Left-aligned bubble, max width ~96 px (leaving ~28 px gutter on
  the right so the shape clearly reads as "theirs").
- Rounded corners, corner radius 2 px.
- Filled (white) on black background — they pop.
- Text in black (drawColor 0) inside the bubble.
- Small "tail" nub pointing at the left edge (optional, time-
  permitting).

Outgoing (from us):

- Right-aligned bubble, max width ~96 px.
- Rounded corners.
- Outlined (frame only, no fill) — our own messages read as less
  visually urgent than the peer's.
- Text in white (drawColor 1) against the black screen.

Both:

- Multi-line word wrap inside the bubble. Line count in a bubble
  is clamped (say max 2 lines per bubble on the visible list;
  if truncated, append `…`).
- Vertical stacking with a 2 px gap between bubbles.
- Cursor highlight: when a bubble is selected (UP/DOWN), flip
  its fill/outline treatment OR draw a 1 px selection ring 1 px
  outside the bubble rect.

Keep:

- Header + footer bands unchanged.
- Auto-mark-read on onEnter.
- Reply / delete input mapping.

## Research to do first

- u8g2 reference list of drawing primitives:
  <https://github.com/olikraus/u8g2/wiki/u8g2reference>
  Key ones: `drawRBox`, `drawRFrame`, `drawTriangle`
  (for bubble tails), `setFontMode(1)` for transparent glyph
  backgrounds.
- u8g2-supported fonts catalog — the wrapper is limited to the
  FONT_TINY/SMALL/LARGE/XLARGE presets but u8g2 itself has many
  more; look for a slightly-nicer-than-FONT_TINY font that still
  fits 6 lines in a ~40-px band.
- Phone chat UI inspiration: iMessage (blue outgoing, grey
  incoming; sharp tail; 12 px radius), SMS (rounded); screenshot
  search for "SMS chat bubble UI low resolution" or similar.
  Goal: steal visual proportions, NOT pixel-art them 1-to-1 —
  we're working in 128×64 monochrome.
- Any extant u8g2 "chat bubble" community examples — search
  GitHub for `u8g2 chat` or `u8g2 bubble`. There are a few
  tutorial repos with reference implementations.
- Read
  [`../codeDocs/NotificationSystem.md`](../codeDocs/NotificationSystem.md)
  §Thread detail screen (current state).

## Design sketch

### Layout math (128×64 screen, rows 13..55 for list, 62 footer)

Constants:

```cpp
static constexpr uint8_t kListTopY    = 13;  // unchanged
static constexpr uint8_t kListBotY    = 56;  // new; was 52
static constexpr uint8_t kBubbleMaxW  = 96;  // 96 px = ~16 chars FONT_TINY
static constexpr uint8_t kBubbleGapY  =  2;  // vertical gap
static constexpr uint8_t kBubbleGapX  =  4;  // left/right margin from screen edge
static constexpr uint8_t kBubbleRadius = 2;
static constexpr uint8_t kMaxLinesPerBubble = 2;
```

Per-message height = `kMaxLinesPerBubble * lineH + 2*padding +
kBubbleGapY`. With FONT_TINY lineH ≈ 8 and 1 px padding top/bot,
a two-line bubble is ~18 px tall → 2 bubbles fit in the 43 px
list band. If that feels cramped, drop to 1 line per bubble
(shows 3–4 bubbles).

### Rendering algorithm

```cpp
void ThreadDetailScreen::render(oled& d, GUIManager&) {
  // header + underline unchanged

  d.setFontPreset(FONT_TINY);

  const int total = BadgeNotifications::messageCountInThread(threadId_);
  if (total == 0) { /* empty state unchanged */ return; }

  // Walk messages newest-first but render oldest-of-visible-window
  // at the top so the reading order reads like a chat (new at
  // the bottom). OR keep newest-at-top to match the Home list.
  // (Ask the user — see Open questions.)

  uint8_t y = kListTopY;
  for (uint8_t i = scroll_; i < total && y < kListBotY; i++) {
    Message m;
    fetchMessage(threadId_, i, &m);

    // 1. Wrap m.body into lines, truncate to kMaxLinesPerBubble.
    // 2. Compute bubble width as max(line_widths) + 2*xpad, capped
    //    at kBubbleMaxW.
    // 3. Place bubble:
    //    - outgoing: x = 128 - kBubbleGapX - bubbleW
    //    - incoming: x =         kBubbleGapX
    // 4. Draw: outgoing = drawRFrame; incoming = drawRBox.
    // 5. Draw text:
    //    - outgoing: setDrawColor(1) (white on black)
    //    - incoming: setDrawColor(0) (black on white fill)
    // 6. If i == cursor_: 1-px selection ring 1 px outside.

    y += bubbleH + kBubbleGapY;
  }

  // footer unchanged
}
```

Utility helpers to add to `oled.h` if missing:

```cpp
void drawRBox(int x, int y, int w, int h, int r);    // filled rounded
void drawRFrame(int x, int y, int w, int h, int r);  // outline rounded
```

Thin wrappers over `u8g2.drawRBox` / `u8g2.drawRFrame`. Check
the existing `oled.cpp` to see which u8g2 handle it uses; the
wrappers follow the same pattern as `drawBox`.

### Word wrap

Reuse the greedy word-break loop already present in
`ThreadDetailScreen::render` (was originally lifted from
`NotificationDetailScreen`). Factor into a helper:

```cpp
// Returns line count written into lines[][]. Caller decides
// how many to show.
uint8_t wrapBody(oled& d, const char* body,
                 uint8_t maxW, uint8_t maxLines,
                 char lines[][24]);
```

Place it near the top of `ThreadDetailScreen` impl in
[`../src/GUI.cpp`](../src/GUI.cpp); it can also be factored into
`oled.cpp` later if `SendBoopConfirmScreen`'s similar wrap wants
to share.

## Implementation checklist

- [ ] Add `drawRBox` / `drawRFrame` wrappers to `oled.{h,cpp}`
      if they're not already exposed.
- [ ] Refactor `ThreadDetailScreen::render` to draw bubbles per
      the layout math above.
- [ ] Add `wrapBody` helper; replace the inline wrap loop.
- [ ] Verify word-wrap handles CJK / unicode gracefully (i.e.
      doesn't break mid-codepoint). u8g2's `getStrWidth` measures
      per-codepoint when the font is UTF-8 (FONT_TINY and FONT_SMALL
      in the current wrapper are ASCII-only; noting here for the
      keyboard's emoji plan in `04-keyboard-polish.md`).
- [ ] Cursor selection: decide between "invert bubble" or
      "1 px ring"; implement the cleaner of the two after
      eyeballing the tiny screen.
- [ ] Scroll math: `moveCursor` + auto-scroll logic need to
      re-measure bubble heights (they're variable now). Simplest:
      cache bubble heights alongside the message during render
      and use that cache in `moveCursor` to decide when to scroll.
- [ ] Preserve existing reply/delete behavior.
- [ ] Update
      [`../codeDocs/NotificationSystem.md`](../codeDocs/NotificationSystem.md)
      §Thread detail screen with the new layout diagram.

## Open questions for the user at start

- **Scroll direction**: top-of-list = newest (current convention,
  matches Home) or bottom-of-list = newest (phone-chat convention)?
  Auto-scroll to bottom on new message may be worth it if option 2.
- **Bubble fill direction**: outgoing FILLED and incoming OUTLINED
  feels counter-intuitive to some (iMessage has outgoing BLUE and
  incoming GREY — both filled, just different colors, but we only
  have one color on mono OLED). Current sketch swaps it so the
  peer's bubble pops more. Pick your poison.
- **Bubble tail** (the little triangle pointing at the sender's
  side): aesthetically nice but wastes 3–4 px on a 128 px wide
  screen. Include or skip?
- **Avatars / initial bubbles** next to each bubble (like iMessage
  group chats): worth doing for future group-chat support, but
  out of scope for now.

## Testing approach

- After the refactor, send ~5 boops back and forth between the
  two badges. Scroll the thread. Confirm visual alternation
  reads like a chat.
- Mix a long (wraps to 2 lines) and short (1 line) body to verify
  the width-fit math.
- Select individual bubbles with UP/DOWN; confirm the highlight
  treatment is readable over the chrome pattern.
- Reply (RIGHT) and delete (LEFT long) still work.
