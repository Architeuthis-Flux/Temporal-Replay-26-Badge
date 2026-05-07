#!/usr/bin/env bash
# setup.sh — First-time setup for Temporal Badge firmware toolchain.
#
# Installs: PlatformIO, Temporal CLI, Python worker deps.
# Safe to re-run — skips anything already installed.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BOLD=$(tput bold 2>/dev/null || echo ""); RESET=$(tput sgr0 2>/dev/null || echo "")
GREEN="\033[32m"; YELLOW="\033[33m"; RED="\033[31m"; NC="\033[0m"
ok()   { echo -e "  ${GREEN}✓${NC}  $*"; }
skip() { echo -e "  ${YELLOW}–${NC}  $*  (already installed)"; }
fail() { echo -e "  ${RED}✗${NC}  $*" >&2; exit 1; }
step() { echo; echo "  ${BOLD}$*${RESET}"; }

echo; echo "  ${BOLD}Temporal Badge — Setup${RESET}"; echo "  ══════════════════════"

# Python
step "Python"
PYTHON=""
for cmd in python3 python; do
    if command -v "$cmd" &>/dev/null; then
        v=$("$cmd" -c "import sys; print(sys.version_info.major, sys.version_info.minor)")
        [[ "${v%% *}" -ge 3 && "${v##* }" -ge 9 ]] && PYTHON="$cmd" && break
    fi
done
[[ -z "$PYTHON" ]] && fail "Python 3.9+ required. Install from https://python.org"
skip "Python ($("$PYTHON" --version))"

# PlatformIO
step "PlatformIO"
PIO_BIN="${HOME}/.platformio/penv/bin/pio"
if [[ -x "$PIO_BIN" ]] && "$PIO_BIN" --version &>/dev/null; then
    skip "PlatformIO ($("$PIO_BIN" --version))"
else
    echo "  Installing PlatformIO..."
    curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py -o /tmp/get-platformio.py
    "$PYTHON" /tmp/get-platformio.py
    rm -f /tmp/get-platformio.py
    ok "PlatformIO installed"
fi

# Temporal CLI
step "Temporal CLI"
if command -v temporal &>/dev/null; then
    skip "temporal ($(temporal --version 2>/dev/null | head -1))"
elif command -v brew &>/dev/null; then
    echo "  Installing via Homebrew..."; brew install temporal; ok "Temporal CLI installed"
else
    echo "  Installing Temporal CLI..."
    TDIR="${HOME}/.local/bin"; mkdir -p "$TDIR"
    curl -sSf https://temporal.download/cli.sh | sh -s -- --install-dir "$TDIR"
    ok "Temporal CLI installed to $TDIR"
    echo "  Add to your shell profile: export PATH=\"\$HOME/.local/bin:\$PATH\""
fi

# Virtualenv
step "Virtualenv"
VENV_DIR="$SCRIPT_DIR/.venv"
if [[ -d "$VENV_DIR" ]] && "$VENV_DIR/bin/python" -c "import temporalio, rich, serial" &>/dev/null; then
    skip "Virtualenv already set up at build_and_flash/.venv"
else
    if [[ ! -d "$VENV_DIR" ]]; then
        echo "  Creating virtualenv..."
        "$PYTHON" -m venv "$VENV_DIR"
    fi
    echo "  Installing dependencies into virtualenv..."
    "$VENV_DIR/bin/pip" install -q -r "$SCRIPT_DIR/flash_worker/requirements.txt"
    ok "Virtualenv ready at build_and_flash/.venv  (temporalio, rich, pyserial)"
fi

# Vendor upstream MicroPython sources for full module surface (network, ssl,
# bluetooth, _thread, etc.). Safe to skip if it fails (offline checkout) — the
# embed port still builds without these and the gating flags default to off.
step "MicroPython sources"
FW_DIR="$SCRIPT_DIR/../firmware"
FETCH_SCRIPT="$FW_DIR/scripts/fetch_micropython_sources.py"
if [[ -x "$FETCH_SCRIPT" ]]; then
    if "$PYTHON" "$FETCH_SCRIPT"; then
        ok "Upstream MicroPython sources vendored (or already up to date)"
    else
        echo -e "  ${YELLOW}–${NC}  fetch_micropython_sources.py failed (offline?). Network/BT/thread modules will stay disabled until you re-run this step."
    fi
fi

# Settings file
step "Configuration"
SETTINGS="$FW_DIR/settings.txt"; EXAMPLE="$FW_DIR/settings.txt.example"
if [[ -f "$SETTINGS" ]]; then
    skip "firmware/settings.txt already exists"
elif [[ -f "$EXAMPLE" ]]; then
    cp "$EXAMPLE" "$SETTINGS"; ok "Created firmware/settings.txt from example"
    echo; echo "  Edit ${BOLD}firmware/settings.txt${RESET} with your WiFi credentials before building."
fi

echo; echo "  ${BOLD}Setup complete.${RESET}"
echo; echo "  Next: edit firmware/settings.txt, then run ${BOLD}./start.sh${RESET}"
echo
