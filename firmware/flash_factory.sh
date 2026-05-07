#!/usr/bin/env bash
# Flash the 16MB factory image to every ESP32-S3 badge connected via USB.
#
# Usage:
#   ./flash_factory.sh [env] [--image PATH] [--baud BAUD] [--serial]
#     env           PlatformIO env (default: echo) — picks factory_<env>_16MB.bin
#     --image PATH  override factory image path
#     --baud BAUD   esptool baud rate (default: 921600)
#     --serial      flash one device at a time instead of in parallel
#     --ports a,b   restrict to a comma-separated list of /dev/tty* ports
#
# Detection: any USB serial with VID:PID = 303A:1001 (ESP32-S3 native USB).
# Each port is flashed in its own background esptool process, with logs
# streamed to firmware/logs/factory_<sanitized-port>.log.  Exits non-zero
# if any flash fails.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ESPTOOL="$HOME/.platformio/penv/bin/esptool"
LOG_DIR="$SCRIPT_DIR/logs"

ENV="echo"
IMAGE=""
BAUD="921600"
PARALLEL=1
PORT_FILTER=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --image)  IMAGE="$2"; shift 2 ;;
    --baud)   BAUD="$2";  shift 2 ;;
    --serial) PARALLEL=0; shift ;;
    --ports)  PORT_FILTER="$2"; shift 2 ;;
    -h|--help)
      sed -n '2,18p' "$0"
      exit 0
      ;;
    -*)
      echo "ERROR: unknown flag $1" >&2
      exit 2
      ;;
    *) ENV="$1"; shift ;;
  esac
done

if [[ -z "$IMAGE" ]]; then
  IMAGE="$SCRIPT_DIR/factory_${ENV}_16MB.bin"
fi

if [[ ! -x "$ESPTOOL" ]]; then
  echo "ERROR: esptool not found at $ESPTOOL — run ignition/setup.sh" >&2
  exit 1
fi
if [[ ! -f "$IMAGE" ]]; then
  echo "ERROR: image $IMAGE not found — run ./make_factory.sh first" >&2
  exit 1
fi

# ── Port detection ────────────────────────────────────────────────────────────
# Walk /sys/class/tty for ttyACM*/ttyUSB* nodes whose USB parent has the
# ESP32-S3 native VID:PID = 303A:1001.  No pyserial dependency.
detect_ports() {
  local out=()
  shopt -s nullglob
  for tty_path in /sys/class/tty/ttyACM* /sys/class/tty/ttyUSB*; do
    local name="${tty_path##*/}"
    local dev_path="/sys/class/tty/$name/device"
    [[ -e "$dev_path" ]] || continue
    # Walk up until we find idVendor (USB device node).
    local up="$dev_path"
    local vid="" pid=""
    for _ in 1 2 3 4 5; do
      if [[ -f "$up/idVendor" && -f "$up/idProduct" ]]; then
        vid="$(<"$up/idVendor")"
        pid="$(<"$up/idProduct")"
        break
      fi
      up="$(readlink -f "$up/..")"
    done
    if [[ "${vid,,}" == "303a" && "${pid,,}" == "1001" ]]; then
      out+=("/dev/$name")
    fi
  done
  shopt -u nullglob
  printf '%s\n' "${out[@]}" | sort -u
}

# Build port list.
if [[ -n "$PORT_FILTER" ]]; then
  IFS=',' read -r -a PORTS <<< "$PORT_FILTER"
else
  mapfile -t PORTS < <(detect_ports)
fi

if [[ "${#PORTS[@]}" -eq 0 ]]; then
  echo "ERROR: no ESP32-S3 (VID:PID 303A:1001) ports detected." >&2
  echo "       Check USB cables / hub power and try again." >&2
  exit 1
fi

echo "==> Image:    $IMAGE"
echo "==> Baud:     $BAUD"
echo "==> Detected ${#PORTS[@]} badge(s):"
for p in "${PORTS[@]}"; do echo "      $p"; done

# Free the ports — serial loggers compete for the same /dev/ttyACMx.
pkill -f serial_log.py 2>/dev/null || true

mkdir -p "$LOG_DIR"

# ── Status reporting ─────────────────────────────────────────────────────────
# Parse esptool's per-port log into a one-line state. esptool 5.x prints
# progress lines like "Writing at 0x002b6e79 [...]  97.9% 1441792/1473299
# bytes..." (one per chunk when stdout is a pipe), so we tail the file each
# refresh and pick out the latest meaningful event.
sanitize_port() {
  local s="${1//\//_}"
  echo "${s#_}"
}

# Echoes a short status string. Optional 2nd arg is the exit code if the
# flash already finished (lets us downgrade "running" → "done"/"FAILED").
status_for_log() {
  local log="$1"
  local exit_rc="${2-}"

  if [[ -n "$exit_rc" ]]; then
    if [[ "$exit_rc" == "0" ]]; then echo "done ✓"; return; fi
    echo "FAILED (exit $exit_rc)"
    return
  fi

  if [[ ! -f "$log" ]]; then echo "queued"; return; fi

  if grep -q "Hard resetting" "$log" 2>/dev/null; then echo "resetting"; return; fi
  if grep -qE "FATAL ERROR|A fatal error|Failed to" "$log" 2>/dev/null; then
    echo "FAILED"; return
  fi
  if grep -q "Hash of data verified" "$log" 2>/dev/null; then echo "verified"; return; fi
  if grep -q "Verifying written data" "$log" 2>/dev/null; then echo "verifying..."; return; fi

  # Latest "Writing at ... XX.X%" — extract the percent.
  local writing_line
  writing_line=$(grep "^Writing at" "$log" 2>/dev/null | tail -n1 || true)
  if [[ -n "$writing_line" ]]; then
    local pct
    pct=$(printf '%s' "$writing_line" | grep -oE '[0-9]+\.[0-9]+%' | tail -n1)
    echo "writing ${pct:-?}"
    return
  fi

  if grep -q "Compressed " "$log" 2>/dev/null; then echo "uploading"; return; fi
  if grep -q "Stub flasher running" "$log" 2>/dev/null; then echo "stub ready"; return; fi
  if grep -q "Connecting" "$log" 2>/dev/null; then echo "connecting..."; return; fi
  echo "starting..."
}

# Render a single dashboard frame: one line per port. Caller handles cursor
# movement; we just emit N lines, each clamped to terminal width.
render_frame() {
  local term_w=80
  if command -v tput >/dev/null 2>&1 && [[ -t 1 ]]; then
    term_w=$(tput cols 2>/dev/null || echo 80)
  fi
  local port log st line
  for port in "${PORTS[@]}"; do
    log="$LOG_DIR/factory_$(sanitize_port "$port").log"
    if [[ -n "${pid_exit[$port]+set}" ]]; then
      st=$(status_for_log "$log" "${pid_exit[$port]}")
    else
      st=$(status_for_log "$log")
    fi
    line=$(printf '  %-15s %s' "$port" "$st")
    # Truncate to terminal width and clear to EOL so leftovers from a
    # longer previous line don't bleed through after redraw.
    printf '%.*s\033[K\n' "$term_w" "$line"
  done
}

# Dashboard loop — runs in foreground while child esptools work in the
# background. Each child writes its exit code to <log>.exit on completion,
# so we don't race with bash's job-table reaping (and wait || true would
# silently turn failures into success, which we don't want).
declare -A pid_exit
TTY=0; [[ -t 1 ]] && TTY=1

flash_one_bg() {
  local port="$1"
  local log="$LOG_DIR/factory_$(sanitize_port "$port").log"
  local exitfile="$log.exit"
  : > "$log"
  rm -f "$exitfile"
  (
    "$ESPTOOL" \
        --chip esp32s3 \
        --port "$port" \
        --baud "$BAUD" \
        --before default-reset \
        --after hard-reset \
        write-flash \
          --flash-mode keep \
          --flash-freq keep \
          --flash-size keep \
          0x0 "$IMAGE" \
        >"$log" 2>&1
    echo $? > "$exitfile"
  ) &
}

# Hide the cursor during the dashboard for less flicker; restore on exit.
restore_cursor() {
  if [[ "$TTY" -eq 1 ]]; then printf '\033[?25h'; fi
}
trap restore_cursor EXIT INT TERM

if [[ "$PARALLEL" -eq 1 && "${#PORTS[@]}" -gt 1 ]]; then
  echo "==> Flashing ${#PORTS[@]} badge(s) in parallel:"
else
  echo "==> Flashing ${#PORTS[@]} badge(s) one at a time:"
fi
[[ "$TTY" -eq 1 ]] && printf '\033[?25l'   # hide cursor

run_with_dashboard() {
  local first=1
  while true; do
    if [[ "$TTY" -eq 1 && $first -eq 0 ]]; then
      # Move cursor up N lines to overwrite the previous frame.
      printf '\033[%dA' "${#PORTS[@]}"
    fi
    first=0
    render_frame

    # Sweep for completed children via the .exit sentinel files.
    local pending=0 p exitfile
    for p in "${PORTS[@]}"; do
      if [[ -z "${pid_exit[$p]+set}" ]]; then
        exitfile="$LOG_DIR/factory_$(sanitize_port "$p").log.exit"
        if [[ -f "$exitfile" ]]; then
          pid_exit["$p"]="$(cat "$exitfile")"
        else
          pending=1
        fi
      fi
    done
    [[ "$pending" -eq 0 ]] && break

    if [[ "$TTY" -eq 1 ]]; then sleep 0.25; else sleep 2; fi
  done
}

FAIL=0
if [[ "$PARALLEL" -eq 1 && "${#PORTS[@]}" -gt 1 ]]; then
  for port in "${PORTS[@]}"; do
    flash_one_bg "$port"
  done
  run_with_dashboard
  for port in "${PORTS[@]}"; do
    [[ "${pid_exit[$port]:-1}" -ne 0 ]] && FAIL=$((FAIL + 1))
  done
else
  # Serial: dashboard still shows the active port's progress.
  for port in "${PORTS[@]}"; do
    flash_one_bg "$port"
    run_with_dashboard
    [[ "${pid_exit[$port]:-1}" -ne 0 ]] && FAIL=$((FAIL + 1))
  done
fi

restore_cursor
trap - EXIT INT TERM

# Dump the tail of any failed log so the operator doesn't have to dig
# through firmware/logs/.
if [[ "$FAIL" -gt 0 ]]; then
  echo ""
  for port in "${PORTS[@]}"; do
    if [[ "${pid_exit[$port]:-1}" -ne 0 ]]; then
      flog="$LOG_DIR/factory_$(sanitize_port "$port").log"
      echo "=== [$port] last 20 log lines ===" >&2
      tail -n 20 "$flog" 2>/dev/null | sed "s|^|[$port] |" >&2
    fi
  done
fi

if [[ "$FAIL" -gt 0 ]]; then
  echo "==> $FAIL of ${#PORTS[@]} flash(es) FAILED" >&2
  exit 1
fi
echo "==> All ${#PORTS[@]} badge(s) flashed."
