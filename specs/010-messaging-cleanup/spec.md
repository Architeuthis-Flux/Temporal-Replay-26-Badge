# spec-010: Messaging Cleanup

## Rationale

The firmware now keeps the three social primitives separate:

- **Boop** is only the IR pairing handshake and `/api/v1/boops` journal sync.
- **Message** is API-only text sent through `/api/v1/pings` with `activity_type="message"`.
- **Zigmoji** is an API-only ephemeral reaction with its own unread playback queue.

This removes the old mixed Contacts/New Msg/Zigmoji flow and the persistent message outbox. Each app owns its state directly, and shared pieces are small: `PeerPickerScreen`, `MessageComposerScreen`, `MouseOverlay`, `MessageInbox`, and `ZigmojiInbox`.

## User Flow

Boot starts directly on the identity-gated destination. Paired badges enter the 4-item menu: `Boop / Zigmoji / Messages / Settings`. Unpaired badges are forced to QR and cannot back out to the menu until pairing completes.

Messages opens a thread list with `+ New` at the top. New sends use the peer picker, then the joystick keyboard. Outgoing bubbles appear immediately with a sending glyph, then transition to sent or failed. Long-press LEFT in a thread retries the newest failed outgoing bubble.

Zigmoji opens to unread playback when the queue is non-empty. Each played item drains from `/zigmojis.json`; once empty, the app opens the peer picker first, then shows the palette for that recipient. Picking a zigmoji posts a zigmoji ping.

## API Contract

Firmware sends text with:

```json
{
  "activity_type": "message",
  "data": {"id": "<client-token>", "body": "..."}
}
```

Firmware sends zigmojis with:

```json
{
  "activity_type": "zigmoji",
  "data": {"key": "<palette-key>"}
}
```

`GET /api/v1/pings` uses the structured cursor shape `{before_ts, before_id}`. `GET /api/v1/boops` is paginated and partner name/company are consumed directly from the server row.

## File Inventory

Added or renamed:

- `messaging/MessageInbox.{h,cpp}`
- `messaging/ZigmojiInbox.{h,cpp}`
- `messaging/SendMessage.{h,cpp}`
- `screens/MessagesScreen.{h,cpp}`
- `screens/PeerPickerScreen.{h,cpp}`
- `screens/MessageComposerScreen.{h,cpp}`
- `ui/MouseOverlay.{h,cpp}`
- `infra/JsonEscape.{h,cpp}`
- `messaging/MessageStatusGlyph.{h,cpp}`

Removed:

- Contacts and contact-detail screens
- Send-boop background/confirm screens
- Persistent message outbox
- Disabled IR notify stream

## Test Plan

Build all firmware environments. On paired hardware, verify message send/sent/failed glyph transitions, retry via long-press LEFT, foreground 5s polling in Messages/Zigmoji, 60s base polling elsewhere, focused-thread haptic suppression, zigmoji queue playback/drain, and the unique active pairing count on the Boop screen.
