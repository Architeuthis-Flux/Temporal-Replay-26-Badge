# Data Model: Modularize Arduino Badge Firmware

This document covers the key types, enums, and shared state produced by the modularization.
No new data is introduced — this is a reorganization of existing Firmware-0308 types.

---

## Enums

### BadgeState (main sketch)
```cpp
enum BadgeState { BADGE_UNPAIRED, BADGE_PAIRED, BADGE_DEMO };
```
- Owned by main sketch global `badgeState`
- Transitions: `UNPAIRED → PAIRED` (badge XBM fetched), `UNPAIRED → DEMO` (timeout/offline), `DEMO → PAIRED` (retry on BTN_DOWN)
- Persisted to NVS via `BadgeStorage::nvsSavePaired()` / loaded via `BadgeStorage::nvsLoadState()`

### IrPhase (BadgeIR)
```cpp
enum IrPhase {
  IR_IDLE, IR_LISTENING, IR_SENDING, IR_WAITING, IR_INCOMING,
  IR_NO_REPLY, IR_PAIR_CONSENT, IR_PAIRED_OK, IR_PAIR_IGNORED,
  IR_PAIR_FAILED, IR_PAIR_CANCELLED, IR_UNAVAIL
};
```
- Owned by `irStatus.phase` in BadgeIR.cpp
- Written exclusively by Core 0 (irTask); read by Core 1 (renderMain)
- `phaseUntil`: millis() deadline for timed phases (NO_REPLY, PAIRED_OK, etc.) — Core 1 loop reverts to IR_IDLE when expired

### RenderMode (BadgeDisplay)
```cpp
enum RenderMode { MODE_BOOT, MODE_QR, MODE_MAIN };
```
- Owned by `renderMode` global in BadgeDisplay.cpp
- Set by main sketch boot stages and osRun()

---

## Structs

### IrStatus (BadgeIR)
```cpp
struct IrStatus {
  volatile IrPhase phase;
  char             peerUID[13];   // hex string, null-terminated
  char             peerRole[12];  // role name string
  char             peerName[32];  // partner_name from backend
  char             debugMsg[24];  // HTTP codes / debug shown on screen
  unsigned long    phaseUntil;    // millis() timer
};
```
- Global instance `irStatus` defined in BadgeIR.cpp, extern'd in BadgeIR.h
- Written exclusively by Core 0 (irTask), read atomically by Core 1 (renderMain treats as snapshot)
- Updated via helpers: `irSetPhase()`, `irSetPeer()`, `irSetDebug()`

### Button (BadgeInput)
```cpp
struct Button {
  uint8_t       pin;
  bool          lastReading;
  bool          state;
  unsigned long lastDebounceTime;
  int           indicatorX;
  int           indicatorY;
};
```
- Array `buttons[4]` defined in BadgeInput.cpp
- Polled by `pollButtons()` on Core 1 every 5 ms
- `state == LOW` means button currently pressed

### Modal (BadgeDisplay — internal)
```cpp
struct Modal {
  String message;
  String leftLabel;
  String rightLabel;
};
```
- Used internally by `renderModal()` and `showModal()`; not exposed in header

---

## BadgeAPI Result Types (BadgeAPI)

All public functions return result structs. No exceptions; caller inspects `ok` field.

```cpp
struct APIResult {
  bool    ok;        // false on network error, 404, or parse failure
  int     httpCode;  // raw HTTP code for debug; 0 on connection failure
};

struct BoopResult : APIResult {
  String workflowId;   // empty on 200 (immediate confirm) or error
  String status;       // "pending", "confirmed"
  String partnerName;  // optional — from partner_name field
};

struct BoopStatusResult : APIResult {
  String status;       // "pending", "confirmed", "not_requested"
};

struct FetchQRResult : APIResult {
  uint8_t* buf;        // heap-allocated; caller must free()
  int      len;        // byte count
};

struct FetchBadgeXBMResult : APIResult {
  uint8_t* buf;        // heap-allocated; caller must free()
  int      len;
  int      assignedRole;  // ROLE_NUM_* constant
};

struct BadgeInfoResult : APIResult {
  String name;
  String title;
  String company;
  String attendeeType;
  // bitmap not included — use fetchBadgeXBM for bitmap
};
```

---

## Shared Mutable State (Cross-Module Globals)

All globals are C++ file-scope with extern declarations in headers. No global namespace
pollution beyond what Arduino sketches conventionally use.

| Variable | Type | Defined in | Accessed from |
|----------|------|------------|--------------|
| `badgeState` | `BadgeState` | main .ino | BadgeDisplay (renderMain), BadgeInput (onButtonPressed) |
| `assignedRole` | `int` | main .ino | BadgeDisplay (renderMain), BadgeIR (IR address encoding) |
| `qrBits` | `uint8_t*` | main .ino | BadgeDisplay (renderQR), BadgeStorage (nvsSaveQR) |
| `badgeBits` | `uint8_t*` | main .ino | BadgeDisplay (renderMain tilt nametag) |
| `qrByteCount` | `int` | main .ino | BadgeStorage (nvsSaveQR) |
| `badgeByteCount` | `int` | main .ino | — |
| `uid[]` | `uint8_t[16]` | BadgeUID.cpp | BadgeIR (NEC TX) |
| `uid_hex[]` | `char[33]` | BadgeUID.cpp | BadgeAPI (URL construction), BadgeDisplay (renderQR), main .ino |
| `irStatus` | `IrStatus` | BadgeIR.cpp | BadgeDisplay (renderMain) |
| `boopListening` | `volatile bool` | BadgeIR.cpp | BadgeInput (pollButtons disarm) |
| `irPairingRequested` | `volatile bool` | BadgeIR.cpp | BadgeInput (onButtonPressed) |
| `pairingCancelRequested` | `volatile bool` | BadgeIR.cpp | BadgeInput (onButtonPressed) |
| `modalActive` | `bool` | BadgeDisplay.cpp | main .ino loop() guard |
| `screenDirty` | `bool` | BadgeDisplay.cpp | BadgeIR (irSetPhase/Debug), BadgeInput (poll*), main .ino loop() |
| `tiltState` | `bool` | BadgeInput.cpp | BadgeDisplay (renderMain indicator) |
| `tiltNametagActive` | `bool` | BadgeInput.cpp | BadgeDisplay (renderMain nametag path) |
| `joySquareX`, `joySquareY` | `int` | BadgeInput.cpp | BadgeDisplay (renderMain joystick dot) |
| `buttons[]` | `Button[4]` | BadgeInput.cpp | BadgeDisplay (renderMain indicators) |
| `renderMode` | `RenderMode` | BadgeDisplay.cpp | main .ino boot stages |
| `displayMutex` | `SemaphoreHandle_t` | BadgeDisplay.cpp | BadgeDisplay.cpp (DISPLAY_TAKE/GIVE) |

---

## NVS Key Map (BadgeStorage)

NVS namespace: `"badge"`

| Key | Type | Value |
|-----|------|-------|
| `"paired"` | bool | true if badge has ever successfully paired |
| `"uid"` | String | uid_hex at time of pairing |
| `"role"` | int | ROLE_NUM_* assigned by backend |
| `"qr"` | bytes | cached QR XBM byte array |
| `"qrlen"` | int | byte count of cached QR |

---

## State Transitions

### Badge State Machine
```
UNPAIRED ──WiFi fail──→ DEMO
UNPAIRED ──badge XBM fetch OK──→ PAIRED
UNPAIRED ──poll timeout──→ DEMO
PAIRED   ──reboot──→ DEMO (loaded from NVS, refresh attempted in osPairedFlow)
DEMO     ──[no transition in current firmware; BTN_DOWN in DEMO shows IR_UNAVAIL]
```

### IrPhase Transitions (Core 0)
```
IDLE ──BTN_DOWN held──→ LISTENING
IDLE ──BTN_UP pressed──→ SENDING → WAITING
WAITING ──reply received──→ INCOMING → [consent modal] → PAIR_CONSENT → PAIRED_OK | PAIR_FAILED
WAITING ──timeout──→ NO_REPLY (timed, reverts to IDLE)
LISTENING ──UID received──→ INCOMING → [consent + reply TX] → PAIR_CONSENT → PAIRED_OK | PAIR_FAILED
PAIR_CONSENT ──cancel button──→ PAIR_CANCELLED (timed, reverts to IDLE)
PAIR_CONSENT ──not_requested──→ PAIR_FAILED (timed, reverts to IDLE)
PAIRED_OK, NO_REPLY, PAIR_IGNORED, PAIR_FAILED, PAIR_CANCELLED ──phaseUntil expired──→ IDLE
```
