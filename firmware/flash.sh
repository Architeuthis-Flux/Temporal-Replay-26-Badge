#!/usr/bin/env bash
# Build + flash the badge firmware via PlatformIO + esptool.
#
# Usage: ./flash.sh [env]    (env defaults to "echo")
#   PORT=/dev/ttyACMx ./flash.sh   to override the serial port
#
# Steps:
#   1. Stop any running serial_log.py so it isn't holding the port.
#   2. Build the requested PlatformIO env.
#   3. Flash bootloader + partitions + firmware via esptool's write-flash.
#
# Direct esptool calls because esptool 5.x's PlatformIO wrapper has CLI
# breakage (see CLAUDE.md gotchas). Using the penv-bundled esptool.
set -euo pipefail

ENV="${1:-echo}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PIO="$HOME/.platformio/penv/bin/pio"
ESPTOOL="$HOME/.platformio/penv/bin/esptool"
PORT="${PORT:-/dev/ttyACM0}"

if [[ ! -x "$PIO" ]]; then
  echo "ERROR: PlatformIO CLI not found at $PIO" >&2
  echo "       Run ignition/setup.sh first." >&2
  exit 1
fi
if [[ ! -x "$ESPTOOL" ]]; then
  echo "ERROR: esptool not found at $ESPTOOL" >&2
  exit 1
fi

echo "==> [1/3] Stopping serial_log.py (if running)"
pkill -f serial_log.py 2>/dev/null || true
# Small settle so the OS releases /dev/ttyACM0
sleep 1

echo "==> [2/3] Building env=$ENV"
"$PIO" run -e "$ENV" -d "$SCRIPT_DIR"

BUILD_DIR="$SCRIPT_DIR/.pio/build/$ENV"
for f in bootloader.bin partitions.bin firmware.bin; do
  if [[ ! -f "$BUILD_DIR/$f" ]]; then
    echo "ERROR: missing $BUILD_DIR/$f" >&2
    exit 1
  fi
done

echo "==> [3/3] Waiting for $PORT"
for _ in {1..15}; do
  [[ -e "$PORT" ]] && break
  sleep 1
done
if [[ ! -e "$PORT" ]]; then
  echo "ERROR: $PORT not present (badge unplugged or in bootloader-only mode)" >&2
  exit 1
fi

echo "==> Flashing $PORT"
"$ESPTOOL" --port "$PORT" --chip esp32s3 write-flash \
  0x0     "$BUILD_DIR/bootloader.bin" \
  0x8000  "$BUILD_DIR/partitions.bin" \
  0x10000 "$BUILD_DIR/firmware.bin"

echo "==> Done."
