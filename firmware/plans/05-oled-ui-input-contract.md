# OLED UI + Input Contract Pass

Status: active

## Tasks

- [x] Replace button glyphs with shape-based diamond icons.
  - Use the physical hardware metaphor: four diamonds in a gamepad cluster.
  - Selected button is a filled diamond.
  - Unselected buttons are hollow/outline diamonds.
  - Keep Xbox-style names in code/docs for clarity, but avoid showing raw `A/B/X/Y`
    letters as the primary OLED affordance.
  - First candidate: the small diamond version from the preview, since it fits
    the footer best.
- [x] Define the badge input contract.
  - Joystick moves menus, lists, carousels, cursors, and scrolling.
  - Buttons act: confirm, cancel, save, edit, preset, toggle.
  - Canonical physical button names:
    - `X`: left/secondary/delete/alternate action.
    - `Y`: top/secondary/lab/help/preset/cycle.
    - `B`: right action button; default semantic confirm/select/save/apply/load.
    - `A`: bottom action button; default semantic back/cancel/close.
  - Footer action order is always `X`, `Y`, `B`, then `A`, left to right.
- [x] Add a confirm/cancel swap setting.
  - Default is Nintendo-style `B` confirm and `A` cancel.
  - Optional Xbox-style setting swaps the semantic actions so `A` confirms
    and `B` cancels.
  - Screens should consume semantic `confirmPressed` / `cancelPressed` for
    normal actions and reserve physical `aPressed` / `bPressed` only for
    hardware-specific or compatibility paths.
  - MicroPython app constants `BTN_CONFIRM`, `BTN_SAVE`, and `BTN_BACK` follow
    the same setting; physical constants remain fixed.
- [x] Add Xbox-style button aliases in firmware.
  - Add aliases such as `yPressed`, `bPressed`, `aPressed`, and `xPressed`.
  - Keep directional fields during the transition.
  - Start migrating new code and comments to Xbox-style button identity names.
- [x] Update the glyph parser.
  - Support `A`, `B`, `X`, and `Y` button tokens.
  - Keep old `L/R/U/D` token support temporarily for existing hints.
  - Render all button tokens as the new diamond-cluster glyphs, not letters.
- [x] Normalize OLED footer hints.
  - Replace hints like `R save L back` with glyph + verb hints.
  - Example: `[B] select  [A] back` with the default Nintendo-style mapping.
  - Example: `[X] toggle  [Y] preset  [B] save  [A] back`.
  - Keep footer hints short and action-only.
- [x] Fix shared menu navigation.
  - `ListMenuScreen` should use joystick navigation only.
  - Joystick Y should be the only list navigation path.
  - Buttons remain select/back/adjust only; they should not move lists.
- [x] Audit app screens for button navigation.
  - Migrate normal menu/list movement off buttons.
  - Preserve semantic button actions in app-specific screens.
  - Screens to check: messages, schedule, files, peer picker, grid menu, LED app,
    1D chess menus, text input, and boop flows.
- [x] Create shared OLED layout rules.
  - Reserve stable bands:
    - title/status.
    - main content.
    - footer hints.
  - Prevent content from drawing into the footer hint lane.
- [x] Improve the LED app UI.
  - Joystick left/right changes LED mode.
  - Joystick moves editor cursor and preset list.
  - Buttons:
    - `X` toggles the focused pixel/cell.
    - `Y` opens the lab/preset picker.
    - semantic confirm saves, applies, or selects.
    - semantic cancel backs out or cancels the edit.
  - Keep OLED preview static while LED matrix runs animated state.
- [x] Do a menu visual hierarchy pass.
  - Make selected rows unmistakable.
  - Keep one primary visual object per screen.
  - Prefer inverse bars, hollow/filled shapes, dividers, and spacing over extra text.
- [x] Add feedback and busy states.
  - Add shared progress/status UI for WiFi/API/sync.
  - Pair completion/failure with haptics.
  - Clock display now waits briefly after existing WiFi connections for SNTP;
    unpaired badges still do not start background network traffic just to sync time.
- [ ] Hardware verification pass.
  - Build `echo` and `charlie`. (Done.)
  - Flash one badge first.
  - Verify OLED readability, footer fit, joystick-only navigation, button actions,
    LED preview/commit, and keyboard LED override restore behavior.
