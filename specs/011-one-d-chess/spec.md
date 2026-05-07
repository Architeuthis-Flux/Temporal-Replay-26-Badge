# spec-011: 1D Chess

1D Chess is a badge-native turn-based game using badge pings for async play.
It replaces the previous card-based tactics prototype.

## Rules

- Board: 8 squares in one row.
- Starting position: `K N R _ _ r n k`.
- White moves first; the challenge sender is White.
- King moves one square left or right.
- Knight jumps exactly two squares left or right.
- Rook slides left or right until blocked.
- A move is illegal if it leaves your own king in check.
- Win by checkmate.
- Draw by stalemate, threefold repetition, or only kings remaining.
- A player may resign; the opponent wins and the game becomes complete.

## Badge Behavior

- Activity type: `one_d_chess`.
- Ping payload kind `c` starts a game, `m` records a move, and `r` records a
  terminal resignation.
- First move sends the challenge and counts as acceptance.
- Only one active game is allowed per peer.
- The main menu shows a notification count for games where it is your turn.
- The game has a first-open tutorial and an in-game tutorial menu item.

## Credits

- Badge adaptation inspired by Rowan Monk's 1D Chess browser game
  (`github.com/Rowan441`).
- 1D Chess variant credited to Martin Gardner's Mathematical Games column in
  the July 1980 issue of Scientific American.
