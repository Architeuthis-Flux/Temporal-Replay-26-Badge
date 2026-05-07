# Quickstart: MicroPython Port — Temporal Badge DELTA

**Feature**: `001-micropython-port` | **Date**: 2026-03-10

Get from zero to a running badge in under 20 minutes on macOS (Apple Silicon).

---

## Prerequisites

Install once:
```bash
brew install cmake ninja python@3.11 git
pip3 install esptool mpremote
```

Verify Python 3.11+:
```bash
python3 --version   # must be 3.11+
```

---

## Step 1: Clone and enter the firmware directory

```bash
git clone https://github.com/AlexLynd/Temporal-Badge.git
cd Temporal-Badge
git checkout 001-micropython-port
cd firmware/micropython-build
```

---

## Step 2: Configure WiFi credentials (optional for BYPASS mode)

For BYPASS mode (no WiFi needed): skip this step. The badge runs without a network.

For live mode:
```bash
cat > creds.py << 'EOF'
SSID       = "YourNetwork"
PASS       = "YourPassword"
SERVER_URL = "https://your-server.example.com"
EOF
```

`creds.py` is gitignored. Never commit it.

---

## Step 3: Configure BYPASS mode

Edit `config.py` and set:
```python
BYPASS = True    # skip all network calls; use mock bitmaps
```

Change to `False` when you have live WiFi + server credentials.

---

## Step 4: Build firmware

```bash
./build.sh
```

First run: downloads MicroPython, ESP-IDF, micropython-lib (~2–4 GB, 5–15 min depending on connection). Subsequent runs are fast (< 2 min, nothing to re-download).

**Expected output**:
```
[build.sh] Building MicroPython for TEMPORAL_BADGE_DELTA...
...
[build.sh] ✓ firmware-conference.bin
-rw-r--r--  1 ...  1.6M  firmware-conference.bin
```

Binary must be ≤ 4 MB. Exit code 0 = success.

---

## Step 5: Flash badge

Connect the badge via USB-C. Then:
```bash
./flash.sh
```

This:
1. Erases flash
2. Writes `firmware.bin`
3. Copies VFS files: `config.py`, `boot.py`, `main.py`, `ir_nec.py`, `badge_sdk.py`, `graphics.py`
4. Copies `creds.py` if present

After flash, the badge reboots automatically.

---

## Step 6: Open REPL

```bash
mpremote connect /dev/cu.usbmodem*
```

You should see:
```
MicroPython v1.24.0 on ...; TEMPORAL_BADGE_DELTA with ESP32S3
>>>
```

The OLED should show the boot splash and start the boot sequence.

---

## Step 7: Verify boot sequence (BYPASS mode)

Observe the OLED:
1. Splash with Temporal wordmark + "Starting..."
2. Badge ID (12-char hex) displayed
3. "Connected (bypass)" after BYPASS_DELAY_MS
4. Mock QR bitmap fills screen for 10 seconds
5. Mock nametag bitmap loads
6. Main UI: base graphic, joystick dot, tilt indicator, button dots

In the REPL:
```python
import machine
print(machine.unique_id().hex())  # verify badge ID
```

---

## Step 8: Test IR pairing (two badges)

On badge A (sender):
- Press BTN_UP
- OLED shows "IR TX: sending UID..." then "IR TX: sent!"

On badge B (receiver):
- OLED shows "IR: UID 1/6" through "IR: UID 6/6"
- Then "RX UID: <12-char hex of badge A's ID>"

---

## Update credentials without rebuilding

```bash
mpremote fs cp creds.py :creds.py
mpremote reset
```

This copies the new `creds.py` to the badge VFS and reboots. No firmware rebuild needed.

---

## Common troubleshooting

| Symptom | Fix |
|---------|-----|
| `esptool` not found | `pip3 install esptool` |
| `/dev/cu.usbmodem*` not found | Check USB-C cable (data, not charge-only); try different port |
| Build fails on mpy-cross | Confirm macOS + Apple Silicon; `CFLAGS_EXTRA="-Wno-gnu-folding-constant"` is in build.sh automatically |
| OLED blank after flash | Verify I2C address is 0x3C; check I2C_SDA=5 I2C_SCL=6 in config.py |
| `import ssd1306` fails | ssd1306 is frozen into firmware; if this fails the build had an error — reflash |
| Badge ID all zeros | `machine.unique_id()` returned fewer than 6 bytes; report hardware issue |
| IR TX: no signal on receiver | Ensure badges are facing each other, < 2m apart; IR_TX_PIN=3 (D2) |
