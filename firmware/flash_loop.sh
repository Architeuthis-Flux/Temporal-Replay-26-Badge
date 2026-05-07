#!/usr/bin/env bash
# Build a full factory image (firmware + filesystem w/ doom WAD), then
# erase-flash + write the whole image in a loop for each badge plugged in.
#
# Usage: ./flash_loop.sh [env]    (env defaults to "echo")
#   PORT=/dev/cu.usbmodem101 ./flash_loop.sh   to override the port
set -euo pipefail

ENV="${1:-echo-dev}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PIO="$HOME/.platformio/penv/bin/pio"
ESPTOOL="$HOME/.platformio/penv/bin/esptool"
PORT="${PORT:-/dev/cu.usbmodem101}"

if [[ ! -x "$ESPTOOL" ]]; then
  echo "ERROR: esptool not found at $ESPTOOL" >&2
  exit 1
fi

# ── Resolve ffat offset from partitions CSV ───────────────────────────────
resolve_ffat_offset() {
  local env="$1" csv_name="" base_csv="" in_env=0 in_base=0
  while IFS= read -r line; do
    [[ "$line" =~ ^\[env:${env}\] ]] && { in_env=1; in_base=0; continue; }
    [[ "$line" =~ ^\[base\] ]] && { in_base=1; in_env=0; continue; }
    [[ "$line" =~ ^\[ ]] && { in_env=0; in_base=0; continue; }
    if [[ "$line" =~ board_build\.partitions[[:space:]]*=[[:space:]]*(.+) ]]; then
      local val="${BASH_REMATCH[1]%%[[:space:]]*}"
      [[ $in_env -eq 1 ]] && csv_name="$val"
      [[ $in_base -eq 1 && -z "$base_csv" ]] && base_csv="$val"
    fi
  done < "$SCRIPT_DIR/platformio.ini"
  csv_name="${csv_name:-$base_csv}"
  [[ -z "$csv_name" ]] && return
  local csv="$SCRIPT_DIR/$csv_name"
  [[ -f "$csv" ]] || return
  awk -F, '/^[[:space:]]*#/{next} /^[[:space:]]*$/{next}
    { gsub(/^[[:space:]]+|[[:space:]]+$/,"",$1); gsub(/^[[:space:]]+|[[:space:]]+$/,"",$4)
      if($1=="ffat"){print $4; exit} }' "$csv"
}

# ── Step 1: Build firmware + filesystem parts ─────────────────────────────
echo "==> Building firmware env=$ENV"
"$PIO" run -e "$ENV" -d "$SCRIPT_DIR"
echo "==> Building filesystem env=$ENV"
"$PIO" run -e "$ENV" -t buildfs -d "$SCRIPT_DIR"

BUILD_DIR="$SCRIPT_DIR/.pio/build/$ENV"
for f in bootloader.bin partitions.bin firmware.bin; do
  [[ -f "$BUILD_DIR/$f" ]] || { echo "ERROR: missing $BUILD_DIR/$f" >&2; exit 1; }
done

FFAT_OFFSET="$(resolve_ffat_offset "$ENV")"
FS_IMAGE=""
for candidate in fatfs.bin littlefs.bin spiffs.bin; do
  [[ -f "$BUILD_DIR/$candidate" ]] && { FS_IMAGE="$BUILD_DIR/$candidate"; break; }
done

FLASH_ARGS=(
  0x0     "$BUILD_DIR/bootloader.bin"
  0x8000  "$BUILD_DIR/partitions.bin"
  0x10000 "$BUILD_DIR/firmware.bin"
)
if [[ -n "$FS_IMAGE" && -n "$FFAT_OFFSET" ]]; then
  FLASH_ARGS+=("$FFAT_OFFSET" "$FS_IMAGE")
  echo "==> FS: ${FS_IMAGE##*/} @ $FFAT_OFFSET"
fi
echo "==> Build complete — ${#FLASH_ARGS[@]} flash args"

# ── Step 2: Flash loop ───────────────────────────────────────────────────
COUNT=0
while true; do
  echo ""
  echo "============================================"
  echo "  Waiting for badge on $PORT ..."
  echo "  (Plug in a badge — Ctrl-C to quit)"
  echo "============================================"

  # Wait for the port to appear
  while [[ ! -e "$PORT" ]]; do
    sleep 0.5
  done

  # Small settle so the USB-serial enumerates fully
  sleep 1

  # Kill anything holding the port (serial monitors, serial_log.py, etc.)
  pkill -f "serial_log.py" 2>/dev/null || true
  pkill -f "miniterm" 2>/dev/null || true
  pkill -f "monitor.*$PORT" 2>/dev/null || true
  lsof -t "$PORT" 2>/dev/null | xargs kill 2>/dev/null || true
  sleep 0.5

  COUNT=$((COUNT + 1))
  echo "==> Badge #$COUNT — erasing flash on $PORT"
  "$ESPTOOL" --port "$PORT" --chip esp32s3 erase-flash || true

  # After erase the ESP resets — wait for the port to come back
  echo "==> Waiting for $PORT to reappear after erase..."
  for _ in {1..30}; do
    [[ -e "$PORT" ]] && break
    sleep 0.5
  done
  if [[ ! -e "$PORT" ]]; then
    echo "*** Badge #$COUNT — port didn't come back after erase, skipping"
    continue
  fi
  sleep 1

  echo "==> Badge #$COUNT — flashing parts"
  if "$ESPTOOL" --port "$PORT" --chip esp32s3 write-flash -z \
      "${FLASH_ARGS[@]}"; then
    echo "==> Badge #$COUNT flashed OK!"
  else
    echo "*** Badge #$COUNT flash FAILED — unplug and retry"
  fi

  # Wait for the badge to be unplugged before looping
  echo "==> Unplug badge to continue..."
  while [[ -e "$PORT" ]]; do
    sleep 0.5
  done
  echo "==> Badge unplugged."
done
