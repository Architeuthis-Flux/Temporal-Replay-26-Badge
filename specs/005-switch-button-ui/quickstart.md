# Quickstart: Building & Testing the Firmware Modularization

**Branch**: `005-switch-button-ui`

---

## Prerequisites

- `firmware/Firmware-0308-modular/BadgeConfig.h` present (copy from `.example`, fill in WiFi + server URL)
- `arduino-cli` installed (`./setup.sh` if not)
- Badge connected via USB

---

## Build

```bash
cd firmware/Firmware-0308-modular/
./build.sh
```

Expected: `build/Firmware-0308-modular.ino.bin` produced, zero errors, zero new warnings.

---

## Flash & Verify

```bash
./flash.sh
# or in one step:
./build.sh --flash
```

Open serial monitor:
```bash
arduino-cli monitor -p /dev/cu.usbmodem*
```

---

## BYPASS Mode (no WiFi / server needed)

Set `const bool BYPASS = true;` in `BadgeConfig.h` before building. The badge skips all HTTP calls and uses mock data. All display and input features work normally in BYPASS mode.

---

## Manual Verification Checklist

This is a pure refactor. Every behavior that worked before must still work.

### Boot Sequence

- [ ] Badge powers on, displays UID on boot screen
- [ ] Connects to WiFi (or shows "WiFi failed" and "Press > to retry" — acceptable on no-network bench test)
- [ ] Runs QR fetch or badge refresh flow (serial shows HTTP requests)
- [ ] Lands on **menu screen** after boot completes

### Menu Navigation

- [ ] Menu shows three items: "Boop", "QR Code", "WiFi / Pair"
- [ ] Push joystick **down** → selection moves down (wraps at bottom)
- [ ] Push joystick **up** → selection moves up (wraps at top)
- [ ] Release joystick to center → selection stops moving (no drift)
- [ ] Footer hint text visible: `joy:nav  v:select  >:back`

### Menu Actions

- [ ] Select **"Boop"** + press BTN_DOWN → enters main screen with IR LISTENING state ("Hold to pair / Waiting...")
- [ ] Select **"QR Code"** + press BTN_DOWN → displays QR bitmap full screen
- [ ] Select **"WiFi / Pair"** + press BTN_DOWN → runs WiFi reconnect + pairing flow, returns to menu

### Button Navigation

- [ ] BTN_UP from any screen → returns to menu
- [ ] BTN_RIGHT from any screen → returns to menu
- [ ] BTN_LEFT → toggles QR view on/off

### Tilt Nametag

- [ ] Hold badge face-up for 1.5 s → display fades, flips 180°, shows badge XBM full screen
- [ ] Return badge to normal orientation → display fades back, resumes menu

### Joystick Dot (Main Screen)

- [ ] Navigate to main screen (select Boop)
- [ ] Move joystick in any direction → small square dot moves correspondingly within circle boundary
- [ ] Center joystick → dot returns to center

### IR Boop Flow

- [ ] Select Boop, press BTN_DOWN → screen shows IR listening state
- [ ] Press any button → pairing cancel requested (returns to menu or idle)

### WiFi Re-pair

- [ ] Select "WiFi / Pair" from menu → reconnects WiFi if needed, runs pairing flow
- [ ] If paired: refreshes badge XBM and returns to menu
- [ ] If unpaired: displays QR and polls for association

### Serial Log Sanity

- [ ] No stack overflow messages
- [ ] No assertion failures
- [ ] IR task starts on Core 0 (`IR task started on Core 0`)
- [ ] No `guru meditation` errors
