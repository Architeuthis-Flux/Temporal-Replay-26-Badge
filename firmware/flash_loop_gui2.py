#!/usr/bin/env python3
"""
╔══════════════════════════════════════════════════════════════╗
║  W.O.P.R.  BADGE PROGRAMMING TERMINAL  v2.0                ║
║  "SHALL WE PLAY A GAME?"                                   ║
╚══════════════════════════════════════════════════════════════╝

Pygame GUI for factory-flashing Temporal Replay 26 badges.
Supports multiple simultaneous ports. Builds once, then each port
independently loops: detect → erase → flash → wait for unplug.

Usage:
    python3 flash_loop_gui2.py [env] [--port PORT ...]

Examples:
    python3 flash_loop_gui2.py
    python3 flash_loop_gui2.py echo --port /dev/cu.usbmodem101 /dev/cu.usbmodem2101
    python3 flash_loop_gui2.py --port /dev/cu.usbmodem101
"""

import argparse
import glob
import os
import random
import subprocess
import sys
import threading
import time

import pygame
import serial

# ── Paths ─────────────────────────────────────────────────────────────────────
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ESPTOOL = os.path.expanduser("~/.platformio/penv/bin/esptool")
CHACHING_PATH = os.path.expanduser("~/dev/slot_pwn/cha_ching.mp3")

# ── Theme: green phosphor CRT ────────────────────────────────────────────────
BG          = (0, 0, 0)
GREEN       = (0, 255, 65)
DIM_GREEN   = (0, 160, 40)
DARK_GREEN  = (0, 60, 15)
AMBER       = (255, 176, 0)
DIM_AMBER   = (180, 120, 0)
RED         = (255, 50, 50)
DIM_RED     = (160, 30, 30)
WHITE       = (200, 255, 200)
CYAN        = (0, 220, 220)
SCANLINE_A  = 18

FPS = 30
FONT_SIZE = 14
LINE_H = 18

# ── State machine ────────────────────────────────────────────────────────────
S_BUILDING   = "BUILDING"
S_WAITING    = "AWAITING TARGET"
S_ERASING    = "ERASING"
S_FLASHING   = "FLASHING"
S_SUCCESS    = "COMPLETE"
S_FAILED     = "FAILED"
S_UNPLUG     = "REMOVE DEVICE"
S_QUIT       = "OFFLINE"

PORT_COLOURS = [GREEN, CYAN, AMBER, (180, 100, 255), (255, 140, 80)]

# Ports to never flash (e.g. serial monitor port)
IGNORE_PORTS = {"/dev/cu.usbmodem101"}


def _parse_ini_sections(ini_path: str) -> dict[str, dict[str, str]]:
    """Parse platformio.ini into {section_name: {key: value}}."""
    sections: dict[str, dict[str, str]] = {}
    current = None
    with open(ini_path) as f:
        for line in f:
            stripped = line.strip()
            if stripped.startswith("[") and "]" in stripped:
                current = stripped[1:stripped.index("]")]
                sections.setdefault(current, {})
                continue
            if current and "=" in stripped and not stripped.startswith("#") and not stripped.startswith(";"):
                key, val = stripped.split("=", 1)
                sections[current][key.strip()] = val.strip()
    return sections


def resolve_ffat_offset(env: str) -> str | None:
    """Read platformio.ini, follow 'extends' chains, and find the ffat
    partition offset from the partitions CSV."""
    ini_path = os.path.join(SCRIPT_DIR, "platformio.ini")
    sections = _parse_ini_sections(ini_path)

    # Walk extends chain to find board_build.partitions
    csv_name = None
    visited = set()
    current = f"env:{env}"
    while current and current not in visited:
        visited.add(current)
        sec = sections.get(current, {})
        if "board_build.partitions" in sec:
            csv_name = sec["board_build.partitions"]
            break
        extends = sec.get("extends", "")
        if extends:
            # "env:echo" or "base"
            current = extends
        else:
            break

    # Fallback to [base] section
    if not csv_name:
        base = sections.get("base", {})
        csv_name = base.get("board_build.partitions")

    if not csv_name:
        return None
    csv_path = os.path.join(SCRIPT_DIR, csv_name)
    if not os.path.isfile(csv_path):
        return None
    with open(csv_path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("#") or not line:
                continue
            cols = [c.strip() for c in line.split(",")]
            if len(cols) >= 4 and cols[0] == "ffat":
                return cols[3]
    return None


# ── Shared log ───────────────────────────────────────────────────────────────

class SharedLog:
    def __init__(self):
        self.lines: list[tuple[str, tuple]] = []
        self.lock = threading.Lock()

    def add(self, text: str, colour):
        with self.lock:
            for line in text.splitlines():
                self.lines.append((line, colour))

    def get_lines(self, n: int) -> list[tuple[str, tuple]]:
        with self.lock:
            return list(self.lines[-n:])

    @property
    def total(self) -> int:
        with self.lock:
            return len(self.lines)


# ── Flash station (one per port) ─────────────────────────────────────────────

class FlashStation:
    def __init__(self, env: str, port: str, idx: int,
                 shared_log: SharedLog):
        self.env = env
        self.port = port
        self.idx = idx
        self.short = port.rsplit("/", 1)[-1]
        self.shared_log = shared_log
        self.tag_colour = PORT_COLOURS[idx % len(PORT_COLOURS)]
        self.state = S_WAITING
        self.count = 0
        self.running = True
        self.enabled = True
        self.worker: threading.Thread | None = None
        # `--post-sync` toggles the optional badge_sync run after
        # firmware/fatfs write completes. PortManager copies its own
        # `post_sync` flag onto each new station before .start().
        self.post_sync = False

    @property
    def tag(self) -> str:
        return f"[{self.short}]"

    def log(self, text: str, colour=None):
        if colour is None:
            colour = self.tag_colour
        self.shared_log.add(f"{self.tag} {text}", colour)

    def port_exists(self) -> bool:
        return os.path.exists(self.port)

    def kill_port_holders(self):
        for pattern in ["serial_log.py", "miniterm", f"monitor.*{self.port}"]:
            subprocess.run(["pkill", "-f", pattern],
                           capture_output=True, timeout=5)
        try:
            result = subprocess.run(["lsof", "-t", self.port],
                                    capture_output=True, text=True, timeout=5)
            for pid in result.stdout.strip().split():
                if pid:
                    subprocess.run(["kill", pid], capture_output=True, timeout=5)
        except Exception:
            pass

    def dtr_rts_reset(self, into_download: bool = False):
        """Toggle DTR/RTS via pyserial to reset the ESP32-S3.
        If into_download=True, enters download mode.
        If False, just hard-resets into normal boot."""
        try:
            s = serial.Serial(self.port, 115200, timeout=0.1)
            if into_download:
                s.dtr = False; s.rts = True;  time.sleep(0.1)
                s.dtr = True;  s.rts = False; time.sleep(0.05)
                s.dtr = False
            else:
                s.rts = True;  time.sleep(0.1)
                s.rts = False
            s.close()
        except Exception as e:
            self.log(f"  DTR/RTS toggle failed: {e}", DIM_AMBER)

    def _run_post_sync(self) -> None:
        """Optional post-flash diff sync against firmware/data/.

        Lets the operator reflash firmware on a badge that already has
        local files without wiping them — anything still missing/stale
        gets pushed via badge_sync's MicroPython raw-REPL transport."""
        try:
            sys.path.insert(0, os.path.join(SCRIPT_DIR, "scripts"))
            import badge_sync  # noqa: WPS433 — late import keeps GUI startup fast
        except Exception as e:
            self.log(f"  POST-SYNC SKIPPED (import failed: {e})", DIM_AMBER)
            return
        data_dir = os.path.join(SCRIPT_DIR, "data")
        # Wait for badge to enumerate its REPL banner.
        time.sleep(2.0)
        self.log("  POST-FLASH SYNC ...", DIM_GREEN)
        try:
            res = badge_sync.sync(self.port, data_dir,
                                  push_missing=True, push_stale=True,
                                  clear_extras=False, timeout=10.0)
            self.log(
                f"  SYNC: pushed={len(res.pushed)} skipped={len(res.skipped)} "
                f"errors={len(res.errors)}",
                GREEN if res.ok else AMBER,
            )
            for err in res.errors[:3]:
                self.log(f"    ! {err}", DIM_AMBER)
        except Exception as e:
            self.log(f"  POST-SYNC FAILED: {e}", DIM_AMBER)

    def run_cmd(self, cmd: list[str], label: str) -> bool:
        self.log(f"> {' '.join(cmd)}", DIM_GREEN)
        try:
            proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1
            )
            assert proc.stdout is not None
            for line in proc.stdout:
                stripped = line.rstrip()
                if stripped:
                    self.log(f"  {stripped}", DIM_GREEN)
            proc.wait()
            if proc.returncode != 0:
                self.log(f"✗ {label} exited {proc.returncode}", RED)
                return False
            return True
        except Exception as e:
            self.log(f"✗ {label}: {e}", RED)
            return False

    def start(self):
        self.worker = threading.Thread(target=self._loop, daemon=True)
        self.worker.start()

    def toggle(self):
        self.enabled = not self.enabled
        label = "ENABLED" if self.enabled else "DISABLED"
        self.log(f"PORT {label}", AMBER if self.enabled else DIM_RED)

    def _loop(self):
        while self.running:
            self.state = S_WAITING

            # Wait until enabled AND port appears
            while self.running:
                if self.enabled and self.port_exists():
                    break
                time.sleep(0.4)
            if not self.running:
                break

            time.sleep(1.0)
            self.kill_port_holders()
            time.sleep(0.5)

            self.count += 1
            self.log(f"■ TARGET #{self.count} ACQUIRED", WHITE)

            # ── Try to reach the badge over serial first ─────────────────
            needs_dtr_reset = True
            try:
                s = serial.Serial(self.port, 115200, timeout=2)
                # Ctrl-C to break out of any running code, then check
                for _ in range(5):
                    s.write(b'\x03')
                    time.sleep(0.05)
                time.sleep(0.3)
                s.write(b'\r\n')
                time.sleep(0.5)
                resp = s.read(s.in_waiting or 1024).decode(errors='replace')
                s.close()
                if '>>>' in resp or 'MicroPython' in resp or 'Temporal' in resp:
                    self.log("  BADGE RESPONSIVE — SKIPPING DTR/RTS RESET", DIM_GREEN)
                    needs_dtr_reset = False
                else:
                    self.log("  NO REPL RESPONSE — WILL DTR/RTS RESET", DIM_GREEN)
            except Exception as e:
                self.log(f"  SERIAL PROBE FAILED: {e}", DIM_GREEN)

            if needs_dtr_reset:
                self.log("  DTR/RTS → DOWNLOAD MODE ...", DIM_GREEN)
                self.dtr_rts_reset(into_download=True)
                time.sleep(1.0)
                for _ in range(20):
                    if self.port_exists():
                        break
                    time.sleep(0.5)
                if not self.port_exists():
                    self.log("✗ PORT LOST AFTER RESET — SKIP", RED)
                    self.state = S_FAILED
                    continue
                time.sleep(0.5)

            # ── Flash firmware + filesystem in one esptool call ──────────
            self.state = S_FLASHING
            self.log("▶ WRITING FIRMWARE + FILESYSTEM ...", AMBER)
            build_dir = os.path.join(SCRIPT_DIR, ".pio", "build", self.env)
            boot_app0 = os.path.expanduser(
                "~/.platformio/packages/framework-arduinoespressif32"
                "/tools/partitions/boot_app0.bin")
            ffat_offset = resolve_ffat_offset(self.env)

            before_mode = "no_reset" if needs_dtr_reset else "default-reset"
            cmd = [
                ESPTOOL, "--chip", "esp32s3", "--port", self.port,
                "--baud", "921600",
                "--before", before_mode, "--after", "hard-reset",
                "write-flash", "-z",
                "--flash-mode", "dio", "--flash-freq", "80m",
                "--flash-size", "detect",
                "0x0000", os.path.join(build_dir, "bootloader.bin"),
                "0x8000", os.path.join(build_dir, "partitions.bin"),
                "0xe000", boot_app0,
                "0x10000", os.path.join(build_dir, "firmware.bin"),
            ]
            # Add filesystem image if available
            if ffat_offset:
                for candidate in ["fatfs.bin", "littlefs.bin", "spiffs.bin"]:
                    fs_path = os.path.join(build_dir, candidate)
                    if os.path.isfile(fs_path):
                        cmd.extend([ffat_offset, fs_path])
                        break
            ok = self.run_cmd(cmd, "write-flash")

            if ok:
                # Reboot into new firmware
                self.log("  REBOOTING BADGE ...", DIM_GREEN)
                time.sleep(0.5)
                for _ in range(20):
                    if self.port_exists():
                        break
                    time.sleep(0.5)
                if self.port_exists():
                    self.dtr_rts_reset(into_download=False)

                # Optional post-flash filesystem sync. The fatfs.bin
                # write above is the canonical baseline, but badges
                # we're upgrading (rather than factory-flashing) may
                # already have local files — `--post-sync` runs the
                # diff engine to push anything still missing/stale
                # without wiping user uploads. Default: off.
                if getattr(self, "post_sync", False) and self.port_exists():
                    self._run_post_sync()

                self.state = S_SUCCESS
                self.log(f"✓ #{self.count} PROGRAMMING COMPLETE", GREEN)
                try:
                    pygame.mixer.Sound(CHACHING_PATH).play()
                except Exception:
                    pass
            else:
                self.state = S_FAILED
                self.log(f"✗ #{self.count} FLASH FAILURE", RED)

            # ── Wait for unplug ──────────────────────────────────────────
            self.state = S_UNPLUG
            self.log("  REMOVE DEVICE TO CONTINUE ...", DIM_AMBER)
            while self.port_exists() and self.running:
                time.sleep(0.4)
            self.log("  DEVICE REMOVED.", DIM_GREEN)

        self.state = S_QUIT


# ── Port manager — auto-discovers new USB serial ports ──────────────────────

class PortManager:
    """Background scanner that discovers new /dev/cu.usbmodem* ports and
    creates FlashStation instances for them on the fly."""

    def __init__(self, env: str, shared_log: SharedLog,
                 build_done: threading.Event, build_ok: list,
                 post_sync: bool = False):
        self.env = env
        self.shared_log = shared_log
        self.build_done = build_done
        self.build_ok = build_ok
        self.stations: list[FlashStation] = []
        self.known_ports: set[str] = set()
        self.lock = threading.Lock()
        self.running = True
        self.post_sync = post_sync

    def seed_ports(self, ports: list[str]):
        """Register initial ports (from CLI args) before scanning."""
        for port in ports:
            self._add_port(port)

    def _add_port(self, port: str):
        with self.lock:
            if port in self.known_ports or port in IGNORE_PORTS:
                return
            self.known_ports.add(port)
            idx = len(self.stations)
            s = FlashStation(self.env, port, idx, self.shared_log)
            s.post_sync = self.post_sync
            self.stations.append(s)
            self.shared_log.add(
                f"▶ NEW PORT DETECTED: {port}", AMBER)
            # Start immediately if build is done
            if self.build_done.is_set() and self.build_ok[0]:
                s.start()

    def get_stations(self) -> list[FlashStation]:
        with self.lock:
            return list(self.stations)

    def start_all_ready(self):
        """Called once after build completes to start all seeded stations."""
        with self.lock:
            for s in self.stations:
                if s.worker is None:
                    s.start()

    def scan_once(self):
        """Discover any new /dev/cu.usbmodem* ports."""
        current = set(glob.glob("/dev/cu.usbmodem*"))
        for port in sorted(current - self.known_ports):
            self._add_port(port)

    def scan_loop(self):
        """Background thread: scan every 2 seconds."""
        while self.running:
            self.scan_once()
            time.sleep(2.0)

    def start_scanner(self):
        t = threading.Thread(target=self.scan_loop, daemon=True)
        t.start()


# ── Rendering helpers ─────────────────────────────────────────────────────────

def build_scanline_overlay(w: int, h: int) -> pygame.Surface:
    overlay = pygame.Surface((w, h), pygame.SRCALPHA)
    for y in range(0, h, 3):
        pygame.draw.line(overlay, (0, 0, 0, SCANLINE_A), (0, y), (w, y))
    return overlay


def state_colour(state: str, tick: int) -> tuple:
    if state == S_WAITING:
        return AMBER if (tick // 15) % 2 == 0 else DARK_GREEN
    if state in (S_ERASING, S_FLASHING, S_BUILDING):
        return AMBER
    if state == S_SUCCESS:
        return GREEN
    if state == S_FAILED:
        return RED
    return DIM_GREEN


def spinner_char(tick: int) -> str:
    return ["◐", "◓", "◑", "◒"][(tick // 8) % 4]


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="W.O.P.R. Badge Flash Terminal")
    parser.add_argument("env", nargs="?", default="echo-dev",
                        help="PlatformIO env (default: echo-dev)")
    parser.add_argument("--port", nargs="+", default=None,
                        help="Serial port(s) to flash (default: auto-detect all)")
    parser.add_argument("--post-sync", action="store_true",
                        help="After firmware+fatfs.bin write, run "
                             "badge_sync to push anything still missing/stale "
                             "via MicroPython raw REPL. Useful for upgrade "
                             "flashes where you want to preserve user files.")
    args = parser.parse_args()

    pio_path = os.path.expanduser("~/.platformio/penv/bin/pio")
    if not os.path.isfile(pio_path):
        print(f"ERROR: pio not found at {pio_path}", file=sys.stderr)
        sys.exit(1)

    # Auto-detect all connected cu.usbmodem ports, or use explicit list
    if args.port:
        ports = args.port
    else:
        ports = sorted(p for p in glob.glob("/dev/cu.usbmodem*")
                       if p not in IGNORE_PORTS)
        if not ports:
            print("No /dev/cu.usbmodem* ports found — plug in a badge or use --port",
                  file=sys.stderr)
            # Start with empty list; auto-discovery will pick them up
            ports = []
    env = args.env

    header_h = 55
    footer_h = 28
    win_w = 1000
    min_log_h = 420
    port_panel_h = max(60, len(ports) * 28 + 12)
    win_h = header_h + port_panel_h + min_log_h + footer_h
    max_log_lines = (win_h - header_h - port_panel_h - footer_h - 10) // LINE_H

    shared_log = SharedLog()

    pygame.init()
    screen = pygame.display.set_mode((win_w, win_h))
    pygame.display.set_caption("W.O.P.R. — Badge Programming Terminal")
    clock = pygame.time.Clock()

    mono_candidates = ["SF Mono", "Menlo", "Monaco", "Courier New", "Courier"]
    font = None
    for name in mono_candidates:
        path = pygame.font.match_font(name)
        if path:
            font = pygame.font.Font(path, FONT_SIZE)
            break
    if font is None:
        font = pygame.font.SysFont("monospace", FONT_SIZE)

    font_big = None
    for name in mono_candidates:
        path = pygame.font.match_font(name)
        if path:
            font_big = pygame.font.Font(path, FONT_SIZE + 2)
            break
    if font_big is None:
        font_big = pygame.font.SysFont("monospace", FONT_SIZE + 2)

    scanlines = build_scanline_overlay(win_w, win_h)

    # ── Banner ────────────────────────────────────────────────────────────
    shared_log.add("", GREEN)
    shared_log.add("╔═══════════════════════════════════════════════════════════╗", AMBER)
    shared_log.add("║   W.O.P.R.  BADGE PROGRAMMING TERMINAL  v2.0            ║", AMBER)
    shared_log.add("║   GREETINGS, PROFESSOR FALKEN.                           ║", AMBER)
    shared_log.add("╚═══════════════════════════════════════════════════════════╝", AMBER)
    shared_log.add("", GREEN)
    shared_log.add(f"TARGET ENV:   {env}", WHITE)
    shared_log.add(f"SERIAL PORTS: {', '.join(ports)}", WHITE)
    shared_log.add("", GREEN)

    # ── Build firmware + filesystem (pio handles flashing per-port) ──────
    build_done = threading.Event()
    build_ok = [False]
    pio = os.path.expanduser("~/.platformio/penv/bin/pio")

    def run_build_cmd(cmd, label):
        shared_log.add(f"  > {' '.join(cmd)}", DIM_GREEN)
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1
        )
        assert proc.stdout is not None
        for line in proc.stdout:
            stripped = line.rstrip()
            if stripped:
                shared_log.add(f"    {stripped}", DIM_GREEN)
        proc.wait()
        if proc.returncode != 0:
            shared_log.add(f"✗ {label} exited {proc.returncode}", RED)
            return False
        return True

    def do_build():
        shared_log.add("▶ COMPILING FIRMWARE + FILESYSTEM ...", AMBER)
        try:
            if not run_build_cmd(
                    [pio, "run", "-e", env, "-d", SCRIPT_DIR], "pio build"):
                shared_log.add("✗ FIRMWARE BUILD FAILED — ABORT", RED)
                build_done.set()
                return

            if not run_build_cmd(
                    [pio, "run", "-e", env, "-t", "buildfs", "-d", SCRIPT_DIR],
                    "pio buildfs"):
                shared_log.add("✗ FILESYSTEM BUILD FAILED — ABORT", RED)
                build_done.set()
                return

            shared_log.add("✓ BUILD COMPLETE — READY TO FLASH", GREEN)
            shared_log.add("", GREEN)
            build_ok[0] = True
        except Exception as e:
            shared_log.add(f"✗ BUILD EXCEPTION: {e}", RED)
        build_done.set()

    build_thread = threading.Thread(target=do_build, daemon=True)
    build_thread.start()

    # ── Port manager (auto-discovers new ports) ───────────────────────────
    port_mgr = PortManager(env, shared_log, build_done, build_ok,
                           post_sync=args.post_sync)
    port_mgr.seed_ports(ports)

    def start_stations():
        build_done.wait()
        if build_ok[0]:
            stations = port_mgr.get_stations()
            shared_log.add(
                f"▶ LAUNCHING {len(stations)} FLASH STATION(S)", AMBER)
            shared_log.add("━" * 58, DIM_GREEN)
            port_mgr.start_all_ready()
            # Start scanning for new ports
            port_mgr.start_scanner()

    starter = threading.Thread(target=start_stations, daemon=True)
    starter.start()

    # ── Event loop ────────────────────────────────────────────────────────
    tick = 0
    scroll_offset = 0
    auto_scroll = True

    prev_station_count = 0

    try:
        while True:
            stations = port_mgr.get_stations()

            # ── Dynamic resize when stations change ───────────────────────
            if len(stations) != prev_station_count:
                prev_station_count = len(stations)
                port_panel_h = max(60, len(stations) * 28 + 12)
                win_h = header_h + port_panel_h + min_log_h + footer_h
                max_log_lines = (win_h - header_h - port_panel_h
                                 - footer_h - 10) // LINE_H
                screen = pygame.display.set_mode((win_w, win_h))
                scanlines = build_scanline_overlay(win_w, win_h)

            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    raise KeyboardInterrupt
                elif event.type == pygame.KEYDOWN:
                    if event.key in (pygame.K_q, pygame.K_ESCAPE):
                        raise KeyboardInterrupt
                    elif event.key == pygame.K_UP:
                        scroll_offset = min(
                            scroll_offset + 3,
                            max(0, shared_log.total - max_log_lines))
                        auto_scroll = False
                    elif event.key == pygame.K_DOWN:
                        scroll_offset = max(0, scroll_offset - 3)
                        if scroll_offset == 0:
                            auto_scroll = True
                    elif event.key == pygame.K_END:
                        scroll_offset = 0
                        auto_scroll = True
                    elif pygame.K_1 <= event.key <= pygame.K_9:
                        idx = event.key - pygame.K_1
                        if idx < len(stations):
                            stations[idx].toggle()
                    elif event.key == pygame.K_HOME:
                        scroll_offset = max(
                            0, shared_log.total - max_log_lines)
                        auto_scroll = False
                elif event.type == pygame.MOUSEWHEEL:
                    if event.y > 0:
                        scroll_offset = min(
                            scroll_offset + 3,
                            max(0, shared_log.total - max_log_lines))
                        auto_scroll = False
                    elif event.y < 0:
                        scroll_offset = max(0, scroll_offset - 3)
                        if scroll_offset == 0:
                            auto_scroll = True

            if auto_scroll:
                scroll_offset = 0

            screen.fill(BG)

            # ── HEADER ────────────────────────────────────────────────────
            pygame.draw.rect(screen, DARK_GREEN, (0, 0, win_w, header_h))
            pygame.draw.line(screen, DIM_GREEN, (0, header_h),
                             (win_w, header_h))

            title = font_big.render(
                "W.O.P.R.  BADGE PROGRAMMING TERMINAL", True, AMBER)
            screen.blit(title, (20, 6))

            ts = time.strftime("%H:%M:%S")
            clock_surf = font.render(ts, True, DIM_GREEN)
            screen.blit(clock_surf,
                         (win_w - clock_surf.get_width() - 20, 8))

            total_flashed = sum(s.count for s in stations)
            active_count = sum(
                1 for s in stations
                if s.state in (S_ERASING, S_FLASHING))
            plugged_count = sum(1 for s in stations if s.port_exists())

            summary = font.render(
                f"ENV: {env}   "
                f"PORTS: {len(stations)}   "
                f"PLUGGED: {plugged_count}   "
                f"ACTIVE: {active_count}   "
                f"TOTAL FLASHED: {total_flashed}",
                True, DIM_GREEN
            )
            screen.blit(summary, (20, 34))

            # ── PORT STATUS PANEL ─────────────────────────────────────────
            panel_y = header_h + 1
            pygame.draw.rect(screen, (0, 10, 0),
                             (0, panel_y, win_w, port_panel_h))
            pygame.draw.line(screen, DIM_GREEN,
                             (0, panel_y + port_panel_h),
                             (win_w, panel_y + port_panel_h))

            py = panel_y + 6
            for i, s in enumerate(stations):
                if not s.enabled:
                    col = DIM_RED
                    status_text = "DISABLED"
                    sp = ""
                else:
                    col = state_colour(s.state, tick)
                    status_text = s.state
                    sp = ""
                    if s.state in (S_ERASING, S_FLASHING, S_WAITING):
                        sp = spinner_char(tick) + " "

                dot_col = GREEN if s.port_exists() else DIM_RED
                if not s.enabled:
                    dot_col = (60, 60, 60)
                pygame.draw.circle(screen, dot_col, (30, py + 8), 5)

                key_label = f"[{i + 1}]"
                line_text = (
                    f"{key_label} {s.short:<22s}  "
                    f"{sp}{status_text:<20s}  "
                    f"FLASHED: {s.count}"
                )
                surf = font.render(line_text, True, col)
                screen.blit(surf, (44, py))
                py += 28

            # ── LOG AREA ──────────────────────────────────────────────────
            log_y = panel_y + port_panel_h + 4
            all_lines = shared_log.get_lines(
                max_log_lines + scroll_offset)
            total_lines = len(all_lines)
            vis_start = max(0, total_lines - max_log_lines - scroll_offset)
            vis_end = max(0, total_lines - scroll_offset)
            visible = all_lines[vis_start:vis_end]

            y = log_y
            for text, colour in visible:
                surf = font.render(text, True, colour)
                screen.blit(surf, (16, y))
                y += LINE_H

            # ── FOOTER ────────────────────────────────────────────────────
            fy = win_h - footer_h
            pygame.draw.line(screen, DIM_GREEN, (0, fy), (win_w, fy))
            pygame.draw.rect(screen, DARK_GREEN, (0, fy, win_w, footer_h))
            hint = font.render(
                "  [Q] QUIT   [1-9] TOGGLE PORT   "
                "[↑↓] SCROLL   AUTO-DETECT: ON",
                True, DIM_GREEN
            )
            screen.blit(hint, (10, fy + 5))

            # ── CRT effects ───────────────────────────────────────────────
            for _ in range(6):
                rx = random.randint(0, win_w - 1)
                ry = random.randint(0, win_h - 1)
                screen.set_at((rx, ry), DIM_GREEN)
            screen.blit(scanlines, (0, 0))

            pygame.display.flip()
            clock.tick(FPS)
            tick += 1

    except KeyboardInterrupt:
        pass
    finally:
        port_mgr.running = False
        stations = port_mgr.get_stations()
        for s in stations:
            s.running = False
        pygame.quit()
        total = sum(s.count for s in stations)
        print(f"\nFlashed {total} badge(s) across {len(stations)} port(s). "
              "THE ONLY WINNING MOVE IS NOT TO PLAY.")


if __name__ == "__main__":
    main()
