# Tasks: Modularize Arduino Badge Firmware + API SDK

**Input**: Design documents from `/specs/004-modularize-arduino-firmware/`
**Prerequisites**: plan.md ✅, spec.md ✅, research.md ✅, data-model.md ✅, contracts/ ✅

**Build gate**: `arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3 firmware/Firmware-0308-modular`
**Tests**: Manual flash-and-verify per `quickstart.md` (no automated test runner per project constitution)

**Organization**: US2 (BadgeConfig) and US3 (BadgeAPI) are built in Phase 2 Foundational because
BadgeIR (US1) cannot compile without BadgeAPI, and every `.cpp` includes `BadgeConfig.h`.
US1 (P1) is delivered in Phase 3 as the final modularization; US4 (BadgeStorage) is built
alongside other prerequisites in Phase 2 since it is standalone and unblocks the main sketch.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies between marked tasks)
- **[Story]**: User story label (US1–US4)

---

## Phase 1: Setup

**Purpose**: Create sketch directory, copy unchanged assets, configure `.gitignore`.

- [x] T001 Create `firmware/Firmware-0308-modular/` sketch directory
- [x] T002 Copy `firmware/Firmware-0308/graphics.h` to `firmware/Firmware-0308-modular/graphics.h` unchanged (XBM assets verbatim from Firmware-0308)
- [x] T003 [P] Add `firmware/Firmware-0308-modular/BadgeConfig.h` to `.gitignore`; create `firmware/Firmware-0308-modular/BadgeConfig.h.example` with stub constants for WiFi SSID/PASS, SERVER\_URL, BYPASS, THIS\_BADGE\_ROLE

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Config, UID, API, and Storage modules — prerequisites for all Phase 3 work.

**US2 Independent Test** (BadgeConfig): Open only `BadgeConfig.h`. Confirm all deployment constants (WiFi, server URL, endpoint paths, timeouts, pin definitions, IR role, bypass flag) are present with no duplicate definitions in any other file.

**US3 Independent Test** (BadgeAPI): Include only `BadgeAPI.h/cpp` and a WiFi connection in a minimal test sketch. Call each function, verify it produces the correct HTTP request, parses the response, and closes the connection in both success and error cases.

**US4 Independent Test** (BadgeStorage): In a minimal sketch with only `BadgeStorage.h/cpp`, call saveQR + savePaired, power-cycle the device, call loadQR + loadState, verify values are restored.

**⚠️ CRITICAL**: No Phase 3 work can begin until T004–T008 are complete.

- [x] T004 [US2] Extract `firmware/Firmware-0308-modular/BadgeConfig.h` from `firmware/Firmware-0308/Firmware-0308.ino` — copy all deployment constants verbatim: WiFi SSID/PASS, SERVER\_URL, all EP\_\* endpoint path fragments (EP\_INFO, EP\_QR, EP\_BOOPS, EP\_BOOPS\_STATUS, EP\_BOOPS\_PENDING), BYPASS flag, THIS\_BADGE\_ROLE (ROLE\_NUM\_\* constant), TILT\_SHOWS\_BADGE bool, PAIRING\_TIMEOUT\_MS, IR\_POLL\_MS, QR\_POLL\_MS, QR\_TIMEOUT\_MS, all pin definitions (I2C SDA/SCL, IR\_TX, IR\_RX, BTN\_UP/DOWN/LEFT/RIGHT, JOY\_X/Y, TILT), display constants (I2C address, rotation); no functions, no mutable state

- [x] T005 [US1] Create `firmware/Firmware-0308-modular/BadgeUID.h` (copy from `specs/004-modularize-arduino-firmware/contracts/BadgeUID.h`) and implement `firmware/Firmware-0308-modular/BadgeUID.cpp` — `read_uid()` reads ESP32-S3 eFuse OPTIONAL\_UNIQUE\_ID (16 bytes) via `esp_efuse_read_field_blob`; on read failure: initialize u8g2, display error message, enter infinite loop (halt); on success: call `uid_to_hex()`; `uid_to_hex()` converts first 6 bytes of `uid[]` to 12-char hex string in `uid_hex[]`, null-terminates at index 12; define `uid[UID_SIZE]` and `uid_hex[UID_SIZE*2+1]` globals in `.cpp`

- [x] T006 [US3] Create `firmware/Firmware-0308-modular/BadgeAPI.h` by copying `specs/004-modularize-arduino-firmware/contracts/BadgeAPI.h` verbatim (result structs: APIResult, BoopResult, BoopStatusResult, FetchQRResult, FetchBadgeXBMResult, BadgeInfoResult; namespace BadgeAPI with 6 function declarations)

- [x] T007 [US3] Implement `firmware/Firmware-0308-modular/BadgeAPI.cpp` — (1) private `_request()` transport helper: constructs full URL from `SERVER_URL` + path, sets `Authorization` or custom headers if present, sets timeout, reads response body into `String`, closes HTTP connection before returning in both success and error paths, returns raw HTTP code (0 on connection failure); (2) implement all 6 public functions using `_request()`: `getBadgeInfo` (GET `/api/v1/badge/{uid}/info`, parse name/title/company/attendeeType with ArduinoJson DynamicJsonDocument(512)), `createBoop` (POST `/api/v1/boops` body `{"badge_uuids":[myUID,theirUID]}`, HTTP 202 → BoopResult with workflowId+"pending", HTTP 200 → BoopResult with ""+"confirmed"+partnerName), `getBoopStatus` (GET `/api/v1/boops/status/{workflowId}?badge_uuid={myUID}`, parse status string), `cancelBoop` (DELETE `/api/v1/boops/pending` body `{"badge_uuids":[myUID,theirUID]}`), `fetchQR` (GET `/api/v1/badge/{uid}/qr.xbm`, parse XBM hex byte array from response body via sscanf loop, `malloc` result.buf; on failure result.buf=nullptr), `fetchBadgeXBM` (GET `/api/v1/badge/{uid}/info`, parse `bitmap` JSON array into `malloc`'d buf, parse `attendeeType` string → ROLE\_NUM\_\* constant in result.assignedRole); all 6 functions must close HTTP connection before returning; failure path sets result.ok=false, result.buf=nullptr where applicable

- [x] T008 [US4] Create `firmware/Firmware-0308-modular/BadgeStorage.h` (copy from `specs/004-modularize-arduino-firmware/contracts/BadgeStorage.h`) and implement `firmware/Firmware-0308-modular/BadgeStorage.cpp` — define file-scope `Preferences prefs` instance; `saveQR(bits, len)`: `prefs.begin("badge",false)`, `prefs.putBytes("qr", bits, len)`, `prefs.putInt("qrlen", len)`, `prefs.end()`; `loadQR(outBuf, outLen)`: begin read-only, getInt "qrlen" → if 0 return false, `malloc(len)`, getBytes "qr" into buf, set \*outBuf and \*outLen, end, return true; on failure \*outBuf=nullptr \*outLen=0 return false; `savePaired(uid, role)`: putBool "paired" true, putString "uid" uid, putInt "role" role; `loadState(outRole)`: getBool "paired" → if false set \*outRole=ROLE\_NUM\_ATTENDEE return false, else getInt "role" → \*outRole, return true; all NVS key strings ("paired", "uid", "role", "qr", "qrlen") private to `.cpp`

**Checkpoint**: Foundation ready — BadgeConfig.h, BadgeUID, BadgeAPI, BadgeStorage all implemented. Phase 3 can begin.

---

## Phase 3: US1 (P1) - Developer Modifies One Subsystem 🎯 MVP

**Goal**: Split remaining logic (Display, Input, IR, main orchestration) into focused modules. After this phase, the complete modular firmware exists and compiles.

**Independent Test**: Open `firmware/Firmware-0308-modular/BadgeIR.cpp` — confirm it contains IrPhase state machine, NEC TX/RX, submitPairing, and no display draws or NVS calls. Open `firmware/Firmware-0308-modular/BadgeDisplay.cpp` — confirm it contains U8G2 init, render functions, modal system, and no IR hardware calls or NVS reads.

- [x] T009 [P] [US1] Create `firmware/Firmware-0308-modular/BadgeDisplay.h` (copy from `specs/004-modularize-arduino-firmware/contracts/BadgeDisplay.h`) and implement `firmware/Firmware-0308-modular/BadgeDisplay.cpp` — include `<U8g2lib.h>` and `"BadgeConfig.h"`; define U8G2\_SSD1306\_128X64\_NONAME\_F\_HW\_I2C u8g2 instance; define displayMutex SemaphoreHandle\_t; define screenLine1/screenLine2 String globals; define screenDirty bool; define modalActive bool; define renderMode RenderMode (default MODE\_BOOT); `displayInit()`: xSemaphoreCreateMutex(); `setDisplayFlip(flip)`: u8g2.setFlipMode(flip); `setScreenText()`: update globals, screenDirty=true; `bootPrint(msg)`: u8g2.begin() if needed, clear + drawStr + send (no mutex — pre-irTask); `renderBoot()`: draw screenLine1/screenLine2 with u8g2; `renderQR()`: drawXBM for QR bitmap (extern uint8\_t\* qrBits from main sketch), SERVER\_URL + EP\_QR status line; `renderMain()`: composite screen — badge XBM or nametag (tiltNametagActive from BadgeInput extern), IR phase indicator (irStatus.phase from BadgeIR extern), joystick dot (joySquareX/Y from BadgeInput extern), button indicators (buttons[] from BadgeInput extern), modal gate check; `renderScreen()`: DISPLAY\_TAKE, route to render\*() by renderMode, DISPLAY\_GIVE, screenDirty=false; `showModal(message, leftLabel, rightLabel)`: DISPLAY\_TAKE, renderModal loop polling BTN\_LEFT/BTN\_RIGHT (read GPIO directly), set modalActive=true on entry / false on exit, DISPLAY\_GIVE, return button index; `drawXBM(x,y,w,h,bits)`: u8g2.drawXBM; `drawStringCharWrap()`: word-wrap algorithm from Firmware-0308; use extern declarations (not \#include) for IrStatus type and irStatus, tilt/joystick/button externs — declare these at top of `.cpp` to avoid circular header include

- [x] T010 [P] [US1] Create `firmware/Firmware-0308-modular/BadgeInput.h` (copy from `specs/004-modularize-arduino-firmware/contracts/BadgeInput.h`) and implement `firmware/Firmware-0308-modular/BadgeInput.cpp` — include `"BadgeConfig.h"` and `"BadgeIR.h"` (for volatile flag externs and irSetPhase); do NOT include `BadgeDisplay.h` — forward-declare `void renderScreen()` at top of `.cpp` instead; define `Button buttons[4]` with pins BTN\_UP/DOWN/LEFT/RIGHT from BadgeConfig.h and their indicator pixel coords from Firmware-0308; define NUM\_BUTTONS=4; define tiltState, tiltNametagActive, tiltHoldPending bool globals; define joySquareX, joySquareY int globals; `pollButtons()`: read each pin, 5ms debounce (lastDebounceTime + lastReading pattern from Firmware-0308), on state change LOW→HIGH call `onButtonPressed(index)`; `pollJoystick()`: analogRead JOY\_X/JOY\_Y, apply deadband, map to square domain with circular clamp, update joySquareX/Y, set screenDirty; `pollTilt()`: read TILT pin, on LOW-to-active: start 1.5s hold timer (tiltHoldPending=true), if held → tiltNametagActive=true, call `tiltFadeTransition()`; `tiltFadeTransition()`: contrast-fade out, flip display, contrast-fade in, call `renderScreen()`; `onButtonPressed(index)`: BTN\_UP → irPairingRequested=true; BTN\_DOWN → boopListening=!boopListening; BTN\_LEFT/RIGHT in PAIR\_CONSENT phase → pairingCancelRequested=true (check irStatus.phase); all logic extracted verbatim from Firmware-0308

- [x] T011 [US1] Create `firmware/Firmware-0308-modular/BadgeIR.h` (copy from `specs/004-modularize-arduino-firmware/contracts/BadgeIR.h`) and implement `firmware/Firmware-0308-modular/BadgeIR.cpp` — include `"BadgeConfig.h"`, `"BadgeDisplay.h"` (for showModal), `"BadgeAPI.h"` (for createBoop/getBoopStatus/cancelBoop), `<IRremote.hpp>`; define irStatus IrStatus global; define volatile bool boopListening=false, irPairingRequested=false, pairingCancelRequested=false; `irSetPhase(p, holdMs)`: set irStatus.phase, set irStatus.phaseUntil=millis()+holdMs if holdMs>0, set screenDirty=true (extern from BadgeDisplay); `irSetPeer(uidHex, addr, name)`: snprintf peerUID/peerRole/peerName into irStatus; `irSetDebug(msg)`: snprintf debugMsg, set screenDirty; `getRoleName(addr)`: switch on IR\_GET\_ROLE(addr) → "Attendee"/"Staff"/"Vendor"/"Speaker"; `roleNumToIR(roleNum)`: switch ROLE\_NUM\_\* → ROLE\_\* byte; `irTask(pvParameters)`: full NEC TX/RX Core 0 task body extracted verbatim from Firmware-0308 — IDLE polls irPairingRequested/boopListening, SENDING transmits uid bytes via IrSender, WAITING receives with IrReceiver timeout, INCOMING parses peer UID+role, PAIR\_CONSENT calls showModal() then submitPairing() or irSetPhase(IR\_PAIR\_CANCELLED); all timed phases use phaseUntil; `submitPairing(theirUID, theirAddr)`: call BadgeAPI::createBoop(uid\_hex, theirUID); if 202 → poll BadgeAPI::getBoopStatus loop checking pairingCancelRequested; if cancel → BadgeAPI::cancelBoop + irSetPhase(IR\_PAIR\_CANCELLED); on confirmed → irSetPeer(..., peerName) + irSetPhase(IR\_PAIRED\_OK); on failure → irSetPhase(IR\_PAIR\_FAILED); preserve exact inter-core communication contract: all volatile flag reads/writes unchanged from Firmware-0308; modalActive gate unchanged

- [x] T012 [US1] Implement `firmware/Firmware-0308-modular/Firmware-0308-modular.ino` — include all module headers (BadgeConfig.h, BadgeUID.h, BadgeDisplay.h, BadgeStorage.h, BadgeAPI.h, BadgeIR.h, BadgeInput.h); declare globals: `BadgeState badgeState = BADGE_UNPAIRED`, `int assignedRole = ROLE_NUM_ATTENDEE`, `uint8_t* qrBits = nullptr`, `uint8_t* badgeBits = nullptr`, `int qrByteCount = 0`, `int badgeByteCount = 0`; extern `uid_hex`, `renderMode`, `screenDirty`, `modalActive`; `setup()`: call read\_uid(), displayInit(), bootPrint("Booting..."), wifiConnect(), osRun(); `wifiConnect()`: WiFi.begin(WIFI\_SSID, WIFI\_PASS) with timeout, bootPrint status; `osRun()`: switch badgeState → osConnectWiFi → osUnpairedFlow / osPairedFlow / osDemoFlow; then xTaskCreatePinnedToCore(irTask,"IR",8192,NULL,1,NULL,0); set renderMode=MODE\_MAIN; `osUnpairedFlow()`: fetchQR, fetchBadgeXBM poll loop with QR\_TIMEOUT\_MS countdown, on success BadgeStorage::saveQR + BadgeStorage::savePaired, set badgeState=BADGE\_PAIRED; on timeout set badgeState=BADGE\_DEMO; `osPairedFlow()`: BadgeStorage::loadQR → qrBits/qrByteCount, BadgeStorage::loadState → assignedRole, attempt online refresh; `osDemoFlow()`: set renderMode=MODE\_MAIN, irTask will show IR\_UNAVAIL on BTN\_UP; `loop()`: if modalActive return; pollButtons(); pollJoystick(); pollTilt(); if irStatus.phaseUntil && millis()>irStatus.phaseUntil → irSetPhase(IR\_IDLE); if screenDirty → renderScreen(); total file ≤ 150 lines

**Checkpoint**: Full modular firmware implemented. Each concern is isolated in its own file. Compile check in Phase 4.

---

## Phase 4: Polish & Verification

**Purpose**: Compile gate + behavior parity validation against Firmware-0308.

- [x] T013 Run `arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3 firmware/Firmware-0308-modular`; fix any compilation errors (type mismatches, missing includes, extern declaration conflicts, forward declaration issues) until compile succeeds with zero errors

- [ ] T014 Flash `firmware/Firmware-0308-modular` to badge hardware and step through all 7 behavior-parity checks from `specs/004-modularize-arduino-firmware/quickstart.md`: (1) boot splash + WiFi connect/fail, (2) unpaired QR display + countdown timer, (3) paired tilt nametag with 1.5s hold gate, (4) IR TX (BTN\_UP → arrows → waiting → consent modal → "Paired!"/timeout), (5) IR RX (BTN\_DOWN hold → listening → consent → paired), (6) demo mode IR unavail on BTN\_UP, (7) NVS reboot: post-pair reboot restores DEMO state + cached QR

- [x] T015 [P] Create `firmware/Firmware-0308-modular/README.md` covering: prerequisites (Arduino IDE 2.x or arduino-cli, ESP32 Arduino core 3.x, library versions for U8G2/IRremote/ArduinoJson 6.x), configuration (copy BadgeConfig.h.example → BadgeConfig.h, fill in WiFi SSID/PASS + SERVER\_URL + BYPASS + THIS\_BADGE\_ROLE), build via arduino-cli (`arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3 firmware/Firmware-0308-modular`), build via Arduino IDE (open .ino, select "Seeed XIAO ESP32S3", Verify then Upload), flash via arduino-cli (`arduino-cli upload --fqbn ... --port /dev/cu.usbmodem* firmware/Firmware-0308-modular`), module map table (one row per file: what it owns, what to edit it for), behavior-parity checklist (7 steps from quickstart.md); keep it factual and concise — no marketing language

- [x] T016 [P] Rewrite root `README.md` — replace the current 6-line stub with a useful orientation doc: one-line description of what the badge is and does; repo structure table covering `firmware/Firmware-0308-modular/` (Arduino C++, active — link to its README), `firmware/micropython-build/` (MicroPython port, experimental), `hardware/DELTA/` (KiCad PCB, ESP32-S3-MINI-1 XIAO), `registrationScanner/` (Quart + Temporal backend), `assets/`, `docs/`; hardware summary (ESP32-S3-MINI-1, SSD1309 OLED, IR TX/RX, buttons, joystick, tilt); quick-start pointer directing readers to `firmware/Firmware-0308-modular/README.md` for build and flash; keep it factual and concise — no marketing language

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 (Setup)**: No dependencies — start immediately
- **Phase 2 (Foundational)**: Depends on Phase 1 — BLOCKS Phase 3
- **Phase 3 (US1)**: Depends on Phase 2 completion
  - T009 (Display) and T010 (Input) can run in parallel once Phase 2 is done
  - T011 (IR) depends on T009 (needs Display for showModal) — sequential after T009
  - T012 (main .ino) depends on T008–T011 — must be last in Phase 3
- **Phase 4 (Polish)**: Depends on Phase 3 completion; T013 before T014; T015 can run in parallel with T013/T014

### User Story Delivery Points

- **US2 (config)**: Delivered by T004 (Phase 2) — BadgeConfig.h with all constants
- **US3 (BadgeAPI)**: Delivered by T006–T007 (Phase 2) — full API SDK module
- **US4 (BadgeStorage)**: Delivered by T008 (Phase 2) — standalone NVS module
- **US1 (module isolation)**: Delivered by T005 + T009–T012 (Phase 3) — complete modular split
- **SC-001 through SC-006**: Verified by T013 (compile) + T014 (flash + parity)

### Within Phase 3

- T009 (Display) and T010 (Input): **parallel** — different files, no mutual dependency
- T011 (IR): sequential after T009 (BadgeDisplay.h must exist for showModal call)
- T012 (main .ino): sequential after T008, T009, T010, T011

---

## Parallel Execution Example: Phase 3

```
# Start T009 and T010 simultaneously (both depend only on Phase 2):
Task A: Implement BadgeDisplay.h + BadgeDisplay.cpp
Task B: Implement BadgeInput.h + BadgeInput.cpp

# Once both complete → start T011:
Task C: Implement BadgeIR.h + BadgeIR.cpp (needs showModal from Display, API from Phase 2)

# Once T011 (and T008 from Phase 2) complete → start T012:
Task D: Implement Firmware-0308-modular.ino
```

---

## Implementation Strategy

### MVP (Single Developer, Sequential)

1. Complete Phase 1 (Setup) — 3 tasks
2. Complete Phase 2 (Foundational) — 5 tasks sequentially
3. T009 + T010 in parallel, then T011, then T012 (Phase 3) — 4 tasks
4. T013 compile check → fix errors → T014 flash verify

### Correctness Priority

Source of truth for all extracted logic: `firmware/Firmware-0308/Firmware-0308.ino`
When in doubt about any function body, copy verbatim from Firmware-0308 and adjust only
the includes/externs. Do not rewrite logic; refactor structure only.

---

## Notes

- **No new features**: All module `.cpp` files extract existing logic from Firmware-0308.ino — do not add, change, or optimize any behavior
- **Circular include**: BadgeDisplay ↔ BadgeInput — resolved by forward-declaring `void renderScreen()` in BadgeInput.cpp instead of including BadgeDisplay.h (per research.md Q2)
- **BadgeAPI runs on Core 0**: createBoop/getBoopStatus/cancelBoop are called from irTask (8 KB stack); stack budget is unchanged from Firmware-0308 (same HTTP calls, just different call site)
- **Buffer ownership**: BadgeAPI::fetchQR and fetchBadgeXBM malloc; caller (main sketch) frees; BadgeStorage::loadQR mallocs; caller frees (per research.md Q5)
- **Credentials**: BadgeConfig.h is in .gitignore; use BadgeConfig.h.example for the committed stub
- **Compile check is the build gate**: arduino-cli compile must succeed before flash
