#!/usr/bin/env python3
"""
╔══════════════════════════════════════════════════════════════╗
║  W.O.P.R.  BADGE PROGRAMMING TERMINAL  v1.0                ║
║  "SHALL WE PLAY A GAME?"                                   ║
╚══════════════════════════════════════════════════════════════╝

Pygame GUI for factory-flashing Temporal Replay 26 badges.
Builds once, then loops: detect → erase → flash → wait for unplug.

Usage:
    python3 flash_loop_gui.py [env] [--port /dev/cu.usbmodem101]
"""

import argparse
import glob
import os
import random
import signal
import subprocess
import sys
import threading
import time

import pygame

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
SCANLINE_A  = 18          # scanline overlay alpha

WIN_W, WIN_H = 900, 640
FPS = 30
FONT_SIZE = 16
LINE_H = 20
MAX_LINES = (WIN_H - 120) // LINE_H  # visible log lines

# ── State machine ────────────────────────────────────────────────────────────
S_BUILDING   = "BUILDING"
S_WAITING    = "AWAITING TARGET"
S_ERASING    = "ERASING"
S_FLASHING   = "FLASHING"
S_SUCCESS    = "PROGRAMMING COMPLETE"
S_FAILED     = "FLASH FAILURE"
S_UNPLUG     = "REMOVE DEVICE"
S_QUIT       = "DISCONNECT"


class FlashStation:
    def __init__(self, env: str, port: str):
        self.env = env
        self.port = port
        self.factory_img = os.path.join(SCRIPT_DIR, f"factory_{env}_16MB.bin")
        self.state = S_BUILDING
        self.count = 0
        self.lines: list[tuple[str, tuple]] = []   # (text, colour)
        self.lock = threading.Lock()
        self.worker: threading.Thread | None = None
        self.running = True

    # ── logging ───────────────────────────────────────────────────────────
    def log(self, text: str, colour=GREEN):
        with self.lock:
            for line in text.splitlines():
                self.lines.append((line, colour))

    def get_lines(self) -> list[tuple[str, tuple]]:
        with self.lock:
            return list(self.lines[-MAX_LINES:])

    # ── port helpers ──────────────────────────────────────────────────────
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
                subprocess.run(["kill", pid], capture_output=True, timeout=5)
        except Exception:
            pass

    # ── subprocess runner with live output ────────────────────────────────
    def run_cmd(self, cmd: list[str], label: str) -> bool:
        self.log(f"  > {' '.join(cmd)}", DIM_GREEN)
        try:
            proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1
            )
            assert proc.stdout is not None
            for line in proc.stdout:
                stripped = line.rstrip()
                if stripped:
                    self.log(f"    {stripped}", DIM_GREEN)
            proc.wait()
            if proc.returncode != 0:
                self.log(f"  ✗ {label} exited {proc.returncode}", RED)
                return False
            return True
        except Exception as e:
            self.log(f"  ✗ {label} exception: {e}", RED)
            return False

    # ── main worker thread ────────────────────────────────────────────────
    def start(self):
        self.worker = threading.Thread(target=self._loop, daemon=True)
        self.worker.start()

    def _loop(self):
        # ── Build ─────────────────────────────────────────────────────────
        self.state = S_BUILDING
        self.log("")
        self.log("╔═══════════════════════════════════════════════════════╗", AMBER)
        self.log("║   W.O.P.R.  BADGE PROGRAMMING TERMINAL  v1.0        ║", AMBER)
        self.log("║   GREETINGS, PROFESSOR FALKEN.                       ║", AMBER)
        self.log("╚═══════════════════════════════════════════════════════╝", AMBER)
        self.log("")
        self.log("LOGON: TEMPORAL REPLAY-26 BADGE FACTORY PROGRAMMER", WHITE)
        self.log(f"TARGET ENV:  {self.env}", WHITE)
        self.log(f"SERIAL PORT: {self.port}", WHITE)
        self.log("")
        self.log("▶ COMPILING FACTORY IMAGE ...", AMBER)
        ok = self.run_cmd(
            [os.path.join(SCRIPT_DIR, "make_factory.sh"), self.env],
            "make_factory.sh"
        )
        if not ok:
            self.log("✗ FACTORY BUILD FAILED — ABORT", RED)
            self.state = S_FAILED
            return

        if not os.path.isfile(self.factory_img):
            self.log(f"✗ IMAGE NOT FOUND: {self.factory_img}", RED)
            self.state = S_FAILED
            return

        sz_mb = os.path.getsize(self.factory_img) / (1024 * 1024)
        self.log(f"✓ IMAGE READY: {sz_mb:.1f} MB", GREEN)
        self.log("")

        # ── Flash loop ────────────────────────────────────────────────────
        while self.running:
            self.state = S_WAITING
            self.log("━" * 55, DIM_GREEN)
            self.log("  AWAITING TARGET ON " + self.port + " ...", AMBER)
            self.log("  CONNECT BADGE TO BEGIN PROGRAMMING", AMBER)
            self.log("━" * 55, DIM_GREEN)

            while not self.port_exists() and self.running:
                time.sleep(0.4)
            if not self.running:
                break

            time.sleep(1.0)   # settle
            self.kill_port_holders()
            time.sleep(0.5)

            self.count += 1
            self.log("")
            self.log(f"■ TARGET #{self.count} ACQUIRED", WHITE)

            # ── Erase ────────────────────────────────────────────────────
            self.state = S_ERASING
            self.log("▶ INITIATING FULL FLASH ERASE ...", AMBER)
            self.run_cmd(
                [ESPTOOL, "--port", self.port, "--chip", "esp32s3",
                 "erase-flash"],
                "erase-flash"
            )

            # After erase the ESP resets — wait for port to reappear
            self.log("  AWAITING PORT RE-ENUMERATION ...", DIM_GREEN)
            for _ in range(30):
                if self.port_exists():
                    break
                time.sleep(0.5)
            if not self.port_exists():
                self.log("✗ PORT DID NOT REAPPEAR AFTER ERASE — SKIP", RED)
                self.state = S_FAILED
                continue
            time.sleep(1.0)

            # ── Flash ────────────────────────────────────────────────────
            self.state = S_FLASHING
            self.log("▶ WRITING FACTORY IMAGE @ 0x0 ...", AMBER)
            ok = self.run_cmd(
                [ESPTOOL, "--port", self.port, "--chip", "esp32s3",
                 "write-flash", "0x0", self.factory_img],
                "write-flash"
            )

            if ok:
                self.state = S_SUCCESS
                self.log(f"✓ TARGET #{self.count} PROGRAMMING COMPLETE", GREEN)
                try:
                    pygame.mixer.Sound(CHACHING_PATH).play()
                except Exception:
                    pass  # no sound if file missing or mixer unavailable
            else:
                self.state = S_FAILED
                self.log(f"✗ TARGET #{self.count} FLASH FAILURE — REMOVE AND RETRY", RED)

            # ── Wait for unplug ──────────────────────────────────────────
            self.state = S_UNPLUG
            self.log("  REMOVE DEVICE TO CONTINUE ...", DIM_AMBER)
            while self.port_exists() and self.running:
                time.sleep(0.4)
            self.log("  DEVICE REMOVED.", DIM_GREEN)

        self.state = S_QUIT


# ── Pygame rendering ─────────────────────────────────────────────────────────

def build_scanline_overlay(w: int, h: int) -> pygame.Surface:
    overlay = pygame.Surface((w, h), pygame.SRCALPHA)
    for y in range(0, h, 3):
        pygame.draw.line(overlay, (0, 0, 0, SCANLINE_A), (0, y), (w, y))
    return overlay


def draw_header(screen: pygame.Surface, font: pygame.font.Font,
                station: FlashStation, tick: int):
    # ── top bar ──────────────────────────────────────────────────────────
    pygame.draw.rect(screen, DARK_GREEN, (0, 0, WIN_W, 50))
    pygame.draw.line(screen, DIM_GREEN, (0, 50), (WIN_W, 50))

    title = font.render("W.O.P.R.  BADGE PROGRAMMING TERMINAL", True, AMBER)
    screen.blit(title, (20, 8))

    ts = time.strftime("%H:%M:%S")
    clock_surf = font.render(ts, True, DIM_GREEN)
    screen.blit(clock_surf, (WIN_W - clock_surf.get_width() - 20, 8))

    sub = font.render(
        f"ENV: {station.env}   PORT: {station.port}   FLASHED: {station.count}",
        True, DIM_GREEN
    )
    screen.blit(sub, (20, 30))

    # ── status bar ───────────────────────────────────────────────────────
    pygame.draw.rect(screen, DARK_GREEN, (0, 55, WIN_W, 30))
    pygame.draw.line(screen, DIM_GREEN, (0, 85), (WIN_W, 85))

    state = station.state
    if state == S_WAITING:
        blink = (tick // 15) % 2 == 0
        col = AMBER if blink else DARK_GREEN
    elif state in (S_ERASING, S_FLASHING, S_BUILDING):
        col = AMBER
    elif state == S_SUCCESS:
        col = GREEN
    elif state == S_FAILED:
        col = RED
    else:
        col = DIM_GREEN

    # Spinner for active states
    spinner = ""
    if state in (S_BUILDING, S_ERASING, S_FLASHING, S_WAITING):
        frames = ["◐", "◓", "◑", "◒"]
        spinner = frames[(tick // 8) % len(frames)] + " "

    status_surf = font.render(f"  STATUS: {spinner}{state}", True, col)
    screen.blit(status_surf, (20, 60))


def draw_log(screen: pygame.Surface, font: pygame.font.Font,
             station: FlashStation, scroll_offset: int):
    lines = station.get_lines()
    y = 95
    total = len(lines)
    visible_start = max(0, total - MAX_LINES - scroll_offset)
    visible_end = max(0, total - scroll_offset)
    visible = lines[visible_start:visible_end]

    for text, colour in visible:
        surf = font.render(text, True, colour)
        screen.blit(surf, (16, y))
        y += LINE_H


def draw_footer(screen: pygame.Surface, font: pygame.font.Font, tick: int):
    y = WIN_H - 28
    pygame.draw.line(screen, DIM_GREEN, (0, y), (WIN_W, y))
    pygame.draw.rect(screen, DARK_GREEN, (0, y, WIN_W, 28))
    hint = font.render(
        "  [Q] QUIT   [SCROLL] ↑↓ / MOUSEWHEEL   \"A STRANGE GAME.\"",
        True, DIM_GREEN
    )
    screen.blit(hint, (10, y + 5))


def add_noise(screen: pygame.Surface, intensity: int = 3):
    """Faint random pixel noise for CRT feel."""
    for _ in range(intensity):
        x = random.randint(0, WIN_W - 1)
        y = random.randint(0, WIN_H - 1)
        screen.set_at((x, y), DIM_GREEN)


def main():
    parser = argparse.ArgumentParser(description="W.O.P.R. Badge Flash Terminal")
    parser.add_argument("env", nargs="?", default="echo",
                        help="PlatformIO env (default: echo)")
    parser.add_argument("--port", default=os.environ.get("PORT", "/dev/cu.usbmodem101"),
                        help="Serial port (default: /dev/cu.usbmodem101)")
    args = parser.parse_args()

    if not os.path.isfile(ESPTOOL):
        print(f"ERROR: esptool not found at {ESPTOOL}", file=sys.stderr)
        sys.exit(1)

    station = FlashStation(args.env, args.port)

    pygame.init()
    screen = pygame.display.set_mode((WIN_W, WIN_H))
    pygame.display.set_caption("W.O.P.R. — Badge Programming Terminal")
    clock = pygame.time.Clock()

    # Try to use a monospace font
    mono_candidates = ["SF Mono", "Menlo", "Monaco", "Courier New", "Courier"]
    font = None
    for name in mono_candidates:
        path = pygame.font.match_font(name)
        if path:
            font = pygame.font.Font(path, FONT_SIZE)
            break
    if font is None:
        font = pygame.font.SysFont("monospace", FONT_SIZE)

    scanlines = build_scanline_overlay(WIN_W, WIN_H)

    station.start()

    tick = 0
    scroll_offset = 0
    auto_scroll = True

    try:
        while True:
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    raise KeyboardInterrupt
                elif event.type == pygame.KEYDOWN:
                    if event.key in (pygame.K_q, pygame.K_ESCAPE):
                        raise KeyboardInterrupt
                    elif event.key == pygame.K_UP:
                        scroll_offset = min(scroll_offset + 3,
                                            max(0, len(station.lines) - MAX_LINES))
                        auto_scroll = False
                    elif event.key == pygame.K_DOWN:
                        scroll_offset = max(0, scroll_offset - 3)
                        if scroll_offset == 0:
                            auto_scroll = True
                    elif event.key == pygame.K_END:
                        scroll_offset = 0
                        auto_scroll = True
                    elif event.key == pygame.K_HOME:
                        scroll_offset = max(0, len(station.lines) - MAX_LINES)
                        auto_scroll = False
                elif event.type == pygame.MOUSEWHEEL:
                    if event.y > 0:   # scroll up
                        scroll_offset = min(scroll_offset + 3,
                                            max(0, len(station.lines) - MAX_LINES))
                        auto_scroll = False
                    elif event.y < 0:  # scroll down
                        scroll_offset = max(0, scroll_offset - 3)
                        if scroll_offset == 0:
                            auto_scroll = True

            if auto_scroll:
                scroll_offset = 0

            screen.fill(BG)
            draw_header(screen, font, station, tick)
            draw_log(screen, font, station, scroll_offset)
            draw_footer(screen, font, tick)
            add_noise(screen, intensity=6)
            screen.blit(scanlines, (0, 0))

            pygame.display.flip()
            clock.tick(FPS)
            tick += 1

    except KeyboardInterrupt:
        pass
    finally:
        station.running = False
        pygame.quit()
        print(f"\nFlashed {station.count} badge(s). THE ONLY WINNING MOVE IS NOT TO PLAY.")


if __name__ == "__main__":
    main()
