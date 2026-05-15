#!/usr/bin/env python3
"""
╔══════════════════════════════════════════════════════════════╗
║  RAINBOW FLASH TERMINAL  v3.0                               ║
║  "SHALL WE PLAY A GAME?"                                    ║
╚══════════════════════════════════════════════════════════════╝

Pygame GUI for factory-flashing Temporal Replay 26 badges.
Rainbow/pink HSV theme, per-badge coloring, text selection,
hardened flash error recovery, progress bars, serial monitor.

Usage:
    python3 flash_loop_gui_Kevin_is_extra.py [env] [--port PORT ...]

Auto-creates a venv on first run — no manual setup needed.
"""

# ── Auto-bootstrap venv (before any third-party imports) ──────────────────────
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
VENV_DIR = os.path.join(SCRIPT_DIR, ".venv-flashgui")
VENV_PYTHON = os.path.join(VENV_DIR, "bin", "python3")
_REQUIRED_PKGS = ["pygame", "pyserial"]


def _ensure_venv():
    # Check if we're specifically in the flashgui venv (not just any venv,
    # since conda/micromamba base envs fool the sys.prefix check)
    real_exe = os.path.realpath(sys.executable)
    real_venv = os.path.realpath(VENV_DIR)
    if real_exe.startswith(real_venv + os.sep):
        return
    if os.environ.get("_FLASHGUI_BOOTSTRAPPED"):
        return

    if not os.path.isfile(VENV_PYTHON):
        print(f"Creating venv at {VENV_DIR} ...")
        subprocess.check_call([sys.executable, "-m", "venv", VENV_DIR])
        subprocess.check_call([VENV_PYTHON, "-m", "pip", "install",
                               "--upgrade", "pip", *_REQUIRED_PKGS])
    else:
        rc = subprocess.call(
            [VENV_PYTHON, "-c", "import pygame, serial"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if rc != 0:
            print("Venv missing packages, reinstalling ...")
            subprocess.check_call([VENV_PYTHON, "-m", "pip", "install",
                                   *_REQUIRED_PKGS])

    os.environ["_FLASHGUI_BOOTSTRAPPED"] = "1"
    os.execv(VENV_PYTHON, [VENV_PYTHON] + sys.argv)


_ensure_venv()

# ── Third-party imports (safe after venv bootstrap) ───────────────────────────
import argparse
import atexit
import colorsys
from typing import Callable
import glob
import random
import re
import threading
import time

import pygame
import serial

# ── Paths ─────────────────────────────────────────────────────────────────────
ESPTOOL = os.path.expanduser("~/.platformio/penv/bin/esptool")
CHACHING_PATH = os.path.expanduser("~/dev/slot_pwn/cha_ching.mp3")
# Optional global lock (--esptool-lock) when one-at-a-time is safer on weak hubs.
ESPTOOL_SERIAL_LOCK = threading.Lock()

# ── HSV Color Engine ──────────────────────────────────────────────────────────


def hsv_rgb(h: float, s: float = 1.0, v: float = 1.0) -> tuple:
    r, g, b = colorsys.hsv_to_rgb(h % 1.0, max(0.0, min(1.0, s)),
                                   max(0.0, min(1.0, v)))
    return (int(r * 255), int(g * 255), int(b * 255))


def port_base_hue(idx: int, total_ports: int) -> float:
    if total_ports <= 1:
        return 0.9
    return (0.9 + idx / total_ports) % 1.0


STATE_HSV_MAP: dict[str, tuple] = {}


def state_color(base_hue: float, state: str, tick: int) -> tuple:
    params = STATE_HSV_MAP.get(state)
    if params is None:
        return hsv_rgb(base_hue, 0.4, 0.5)
    h, s, v = params
    if h is None:
        h = base_hue
    if state == S_WAITING:
        v = 0.3 + 0.2 * (0.5 + 0.5 * ((tick % 60) / 60.0))
    return hsv_rgb(h, s, v)


def rainbow_line_color(line_idx: int, n_hues: int, tick: int) -> tuple:
    hue = ((line_idx / max(1, n_hues * 4)) + tick * 0.0005) % 1.0
    return hsv_rgb(hue, 0.7, 0.9)


# ── Theme ─────────────────────────────────────────────────────────────────────
BG = (0, 0, 0)
HEADER_BG = (20, 5, 15)
PANEL_BG = (10, 2, 8)
PINK = (255, 100, 180)
DIM_PINK = (180, 60, 120)
DARK_PINK = (60, 15, 40)
HOT_PINK = (255, 40, 130)
WHITE = (240, 220, 255)
RED = (255, 50, 50)
DIM_RED = (160, 30, 30)
AMBER = (255, 176, 0)
DIM_AMBER = (180, 120, 0)

FPS = 30
FONT_SIZE = 14
LINE_H = 18

# ── State machine ─────────────────────────────────────────────────────────────
S_BUILDING = "BUILDING"
S_WAITING = "AWAITING TARGET"
S_ERASING = "ERASING"
S_FLASHING = "FLASHING"
S_FLASHING_FS = "FLASHING FS"
S_SUCCESS = "COMPLETE"
S_FAILED = "FAILED"
S_UNPLUG = "REMOVE DEVICE"
S_QUIT = "OFFLINE"

STATE_HSV_MAP.update({
    S_WAITING:     (None, 0.3, 0.5),
    S_BUILDING:    (0.08, 1.0, 0.9),
    S_ERASING:     (0.08, 1.0, 0.9),
    S_FLASHING:    (None, 1.0, 0.9),
    S_FLASHING_FS: (None, 0.8, 0.8),
    S_SUCCESS:     (None, 0.6, 1.0),
    S_FAILED:      (0.0, 1.0, 1.0),
    S_UNPLUG:      (None, 0.2, 0.4),
    S_QUIT:        (None, 0.1, 0.3),
})

IGNORE_PORTS = {"/dev/cu.usbmodem101"}
BATTERY_FULL_RE = re.compile(r'\[POWER\]\s*BATTERY_FULL')
ANSI_RE = re.compile(r'\x1b\[[0-9;]*[A-Za-z]|\[[\dA-Z;]*[mKHJ]')
ESPTOOL_PROGRESS_LINE_RE = re.compile(
    r'Writing at 0x[0-9a-fA-F]+.*\d+%'
    r'|Compressing \d+'
    r'|\[K'
)


def _parse_esptool_progress_percent(clean: str) -> int | None:
    """Esptool v4 uses '(12 %)'; v5 uses bar + '97.4%' — parse both."""
    if "Writing at" not in clean and "Verifying" not in clean:
        return None
    m = re.search(r'\(\s*(\d+(?:\.\d+)?)\s*%\s*\)', clean)
    if m:
        return min(100, int(float(m.group(1))))
    nums = re.findall(r'(\d+(?:\.\d+)?)\s*%', clean)
    if nums:
        return min(100, int(float(nums[-1])))
    return None


def _is_esptool_traceback_noise(clean: str) -> bool:
    """Hide Python stack frames from esptool failures — keep human summaries."""
    if clean.startswith('  File "'):
        return True
    if "/tool-esptoolpy/" in clean:
        return True
    if "During handling of the above exception" in clean:
        return True
    if clean.strip() in ("StopIteration", "^"):
        return True
    if re.match(r"^\s+\^+\s*$", clean):
        return True
    return False


# ── Helpers ───────────────────────────────────────────────────────────────────

def _parse_ini_sections(ini_path: str) -> dict[str, dict[str, str]]:
    sections: dict[str, dict[str, str]] = {}
    current = None
    with open(ini_path) as f:
        for line in f:
            stripped = line.strip()
            if stripped.startswith("[") and "]" in stripped:
                current = stripped[1:stripped.index("]")]
                sections.setdefault(current, {})
                continue
            if (current and "=" in stripped
                    and not stripped.startswith("#")
                    and not stripped.startswith(";")):
                key, val = stripped.split("=", 1)
                sections[current][key.strip()] = val.strip()
    return sections


def resolve_ffat_offset(env: str) -> str | None:
    ini_path = os.path.join(SCRIPT_DIR, "platformio.ini")
    sections = _parse_ini_sections(ini_path)
    csv_name = None
    visited: set[str] = set()
    current: str | None = f"env:{env}"
    while current and current not in visited:
        visited.add(current)
        sec = sections.get(current, {})
        if "board_build.partitions" in sec:
            csv_name = sec["board_build.partitions"]
            break
        extends = sec.get("extends", "")
        current = extends if extends else None

    if not csv_name:
        csv_name = sections.get("base", {}).get("board_build.partitions")
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


def spinner_char(tick: int) -> str:
    return ["◐", "◓", "◑", "◒"][(tick // 8) % 4]


def clipboard_copy(text: str):
    try:
        subprocess.run(["pbcopy"], input=text.encode("utf-8"),
                       check=False, timeout=2)
    except Exception:
        pass


# ── Shared log ────────────────────────────────────────────────────────────────

class SharedLog:
    def __init__(self):
        self.lines: list[tuple[str, tuple, int | None]] = []
        self.lock = threading.Lock()

    def add(self, text: str, colour, port_idx: int | None = None):
        with self.lock:
            for line in text.splitlines():
                clean = line.replace('\x00', '')
                self.lines.append((clean, colour, port_idx))

    def get_lines(self, n: int) -> list[tuple[str, tuple, int | None]]:
        with self.lock:
            return list(self.lines[-n:])

    @property
    def total(self) -> int:
        with self.lock:
            return len(self.lines)


# ── Flash station (one per port) ─────────────────────────────────────────────

class FlashStation:
    def __init__(self, env: str, port: str, idx: int,
                 shared_log: SharedLog, opts: argparse.Namespace,
                 peer_count: Callable[[], int]):
        self.env = env
        self.port = port
        self.idx = idx
        self.short = port.rsplit("/", 1)[-1]
        self.shared_log = shared_log
        self.opts = opts
        self.peer_count = peer_count
        self.state = S_WAITING
        self.count = 0
        self.running = True
        self.enabled = True
        self.worker: threading.Thread | None = None
        self.post_sync = False
        self._serial: serial.Serial | None = None
        self.progress = 0
        self.flash_start_time: float | None = None
        self.last_flash_duration: float | None = None

    @property
    def tag(self) -> str:
        return f"[{self.short}]"

    def log(self, text: str, colour=None):
        if colour is None:
            colour = PINK
        self.shared_log.add(f"{self.tag} {text}", colour, self.idx)

    def port_exists(self) -> bool:
        return os.path.exists(self.port)

    def close(self):
        s = self._serial
        self._serial = None
        if s is not None:
            try:
                s.close()
            except Exception:
                pass

    def stop(self):
        self.running = False
        self.close()

    def kill_port_holders(self):
        for pattern in ["serial_log.py", "miniterm", f"monitor.*{self.port}"]:
            subprocess.run(["pkill", "-f", pattern],
                           capture_output=True, timeout=5)
        try:
            result = subprocess.run(["lsof", "-t", self.port],
                                    capture_output=True, text=True, timeout=5)
            for pid in result.stdout.strip().split():
                if pid:
                    subprocess.run(["kill", pid],
                                   capture_output=True, timeout=5)
        except Exception:
            pass

    def _open_serial(self, baudrate=115200, timeout=0.1) -> serial.Serial | None:
        self.close()
        try:
            self._serial = serial.Serial(self.port, baudrate, timeout=timeout)
            return self._serial
        except Exception as e:
            self.log(f"  Serial open failed: {e}", DIM_AMBER)
            return None

    def dtr_rts_reset(self, into_download: bool = False):
        try:
            s = self._open_serial()
            if s is None:
                return
            if into_download:
                s.dtr = False; s.rts = True;  time.sleep(0.1)
                s.dtr = True;  s.rts = False; time.sleep(0.05)
                s.dtr = False
            else:
                s.rts = True;  time.sleep(0.1)
                s.rts = False
            self.close()
        except Exception as e:
            self.log(f"  DTR/RTS toggle failed: {e}", DIM_AMBER)
            self.close()

    def _wait_for_port(self, timeout_s: float = 10.0) -> bool:
        deadline = time.time() + timeout_s
        while time.time() < deadline and self.running:
            if self.port_exists():
                return True
            time.sleep(0.5)
        return self.port_exists()

    def _post_flash_reset(self):
        self.log("  REBOOTING BADGE ...", DIM_PINK)
        time.sleep(0.5)
        for attempt in range(2):
            if not self._wait_for_port(10.0):
                self.log(f"  PORT NOT FOUND (attempt {attempt + 1})",
                         DIM_AMBER)
                continue
            self.dtr_rts_reset(into_download=False)
            time.sleep(1.0)
            if self._wait_for_port(5.0):
                self.log("  RESET SUCCESSFUL", DIM_PINK)
                return
            self.log(f"  PORT LOST AFTER RESET (attempt {attempt + 1})",
                     DIM_AMBER)
        self.log("  RESET ATTEMPTS EXHAUSTED — may need manual reset",
                 AMBER)

    def _run_post_sync(self) -> None:
        try:
            sys.path.insert(0, os.path.join(SCRIPT_DIR, "scripts"))
            import badge_sync
        except Exception as e:
            self.log(f"  POST-SYNC SKIPPED (import failed: {e})", DIM_AMBER)
            return
        data_dir = os.path.join(SCRIPT_DIR, "data")
        time.sleep(2.0)
        self.log("  POST-FLASH SYNC ...", DIM_PINK)
        try:
            res = badge_sync.sync(self.port, data_dir,
                                  push_missing=True, push_stale=True,
                                  clear_extras=False, timeout=10.0)
            ok_col = hsv_rgb(0.3, 0.7, 1.0)
            self.log(
                f"  SYNC: pushed={len(res.pushed)} "
                f"skipped={len(res.skipped)} "
                f"errors={len(res.errors)}",
                ok_col if res.ok else AMBER,
            )
            for err in res.errors[:3]:
                self.log(f"    ! {err}", DIM_AMBER)
        except Exception as e:
            self.log(f"  POST-SYNC FAILED: {e}", DIM_AMBER)

    def _fs_flash_baud(self) -> int:
        if getattr(self.opts, "fs_baud", 0) and self.opts.fs_baud > 0:
            return self.opts.fs_baud
        peers = max(1, self.peer_count())
        if peers > 1:
            return min(self.opts.baud, 460800)
        return self.opts.baud

    def _esptool_cmd(self, before: str,
                     after: str = "no_reset",
                     baud: int | None = None) -> list[str]:
        b = baud if baud is not None else self.opts.baud
        cmd = [
            ESPTOOL, "--chip", "esp32s3", "--port", self.port,
            "--baud", str(b),
            "--before", before, "--after", after,
            "write_flash", "-z",
            "--flash-mode", "dio", "--flash-freq", "80m",
            "--flash-size", "detect",
        ]
        if self.opts.verify:
            cmd.append("--verify")
        return cmd

    def run_cmd(self, cmd: list[str], label: str,
                parse_progress: bool = False) -> bool:
        self.log(f"> {' '.join(cmd)}", DIM_PINK)
        if parse_progress:
            self.progress = 0
        use_lock = getattr(self.opts, "esptool_serial_lock", False)
        lock = ESPTOOL_SERIAL_LOCK if use_lock else None

        try:
            if lock:
                lock.acquire()
            proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1
            )
            assert proc.stdout is not None
            tb_mode = False
            tb_note = False
            for line in proc.stdout:
                stripped = line.rstrip().replace('\x00', '')
                clean = ANSI_RE.sub('', stripped).strip()
                if not clean:
                    continue
                if clean.startswith("Traceback"):
                    tb_mode = True
                    if not tb_note:
                        self.log(
                            "  (long Python traceback from esptool "
                            "omitted — summary below)",
                            DIM_AMBER)
                        tb_note = True
                    continue
                if tb_mode:
                    if _is_esptool_traceback_noise(clean):
                        continue
                    if (clean.startswith("  ")
                            and not clean.startswith('  File "')):
                        continue
                    if "fatal error occurred" in clean.lower():
                        tb_mode = False
                        self.log(f"  {clean}", RED)
                        continue
                    tb_mode = False
                if parse_progress:
                    pct = _parse_esptool_progress_percent(clean)
                    if pct is not None:
                        self.progress = pct
                    if ESPTOOL_PROGRESS_LINE_RE.search(clean):
                        continue
                self.log(f"  {clean}", DIM_PINK)
            proc.wait()
            if proc.returncode != 0:
                self.log(f"✗ {label} exited {proc.returncode}", RED)
                return False
            if parse_progress:
                self.progress = 100
            return True
        except Exception as e:
            self.log(f"✗ {label}: {e}", RED)
            return False
        finally:
            if lock:
                lock.release()

    def _serial_monitor(self):
        battery_full_sent = False
        try:
            s = self._open_serial(timeout=0.5)
            if s is None:
                return
            while self.running and self.port_exists():
                try:
                    raw = s.readline()
                    if not raw:
                        continue
                    line = raw.decode(errors="replace").rstrip()
                    line = line.replace('\x00', '').replace('\ufffd', '')
                    if line:
                        self.log(f"  {line}", DIM_PINK)
                    if not battery_full_sent and BATTERY_FULL_RE.search(line):
                        battery_full_sent = True
                        self.log("  BATTERY FULL — DISPLAYING ON OLED",
                                 HOT_PINK)
                        try:
                            s.write(b'\x03\x03')
                            time.sleep(0.2)
                            for cmd_bytes in [
                                b'oled_clear()\r\n',
                                b'oled_set_cursor(0, 20)\r\n',
                                b'oled_print("Battery full!")\r\n',
                                b'oled_show()\r\n',
                            ]:
                                s.write(cmd_bytes)
                                time.sleep(0.05)
                        except Exception as e:
                            self.log(f"  OLED write failed: {e}", DIM_AMBER)
                except serial.SerialException:
                    break
                except Exception:
                    break
        finally:
            self.close()

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
            self.progress = 0
            while self.running:
                if self.enabled and self.port_exists():
                    break
                time.sleep(0.4)
            if not self.running:
                break
            try:
                self._flash_cycle()
            finally:
                self.close()
        self.state = S_QUIT

    def _flash_cycle(self):
        time.sleep(1.0)
        self.kill_port_holders()
        time.sleep(0.5)

        self.count += 1
        self.log(f"■ TARGET #{self.count} ACQUIRED", WHITE)

        # ── Serial probe ──────────────────────────────────────────────
        needs_dtr_reset = True
        try:
            s = self._open_serial(timeout=2)
            if s:
                for _ in range(5):
                    s.write(b'\x03')
                    time.sleep(0.05)
                time.sleep(0.3)
                s.write(b'\r\n')
                time.sleep(0.5)
                resp = s.read(s.in_waiting or 1024).decode(errors='replace')
                self.close()
                if ('>>>' in resp or 'MicroPython' in resp
                        or 'Temporal' in resp):
                    self.log("  BADGE RESPONSIVE — SKIPPING DTR/RTS RESET",
                             DIM_PINK)
                    needs_dtr_reset = False
                else:
                    self.log("  NO REPL RESPONSE — WILL DTR/RTS RESET",
                             DIM_PINK)
        except Exception as e:
            self.log(f"  SERIAL PROBE FAILED: {e}", DIM_PINK)
            self.close()

        if needs_dtr_reset:
            self.log("  DTR/RTS → DOWNLOAD MODE ...", DIM_PINK)
            self.dtr_rts_reset(into_download=True)
            time.sleep(1.0)
            if not self._wait_for_port(10.0):
                self.log("✗ PORT LOST AFTER RESET — SKIP", RED)
                self.state = S_FAILED
                return
            time.sleep(0.5)

        # ── Erase (optional) ─────────────────────────────────────────
        self.close()  # ensure port is free for esptool
        if self.opts.erase_all:
            self.state = S_ERASING
            self.log("▶ ERASING ENTIRE FLASH ...", AMBER)
            erase_cmd = [
                ESPTOOL, "--chip", "esp32s3", "--port", self.port,
                "--baud", str(self.opts.baud), "erase_flash",
            ]
            if not self.run_cmd(erase_cmd, "erase_flash"):
                self.log("✗ ERASE FAILED", RED)
                self.state = S_FAILED
                return

        # ── Build paths ──────────────────────────────────────────────
        build_dir = os.path.join(SCRIPT_DIR, ".pio", "build", self.env)
        boot_app0 = os.path.expanduser(
            "~/.platformio/packages/framework-arduinoespressif32"
            "/tools/partitions/boot_app0.bin")
        ffat_offset = resolve_ffat_offset(self.env)
        before_mode = "no_reset" if needs_dtr_reset else "default_reset"

        self.flash_start_time = time.time()
        fw_ok = True
        fs_ok = True

        # ── Step 1: Flash firmware ───────────────────────────────────
        if not self.opts.fs_only:
            self.state = S_FLASHING
            self.log("▶ WRITING FIRMWARE ...", AMBER)
            fw_cmd = self._esptool_cmd(before_mode) + [
                "0x0000", os.path.join(build_dir, "bootloader.bin"),
                "0x8000", os.path.join(build_dir, "partitions.bin"),
                "0xe000", boot_app0,
                "0x10000", os.path.join(build_dir, "firmware.bin"),
            ]
            fw_ok = self.run_cmd(fw_cmd, "write-firmware",
                                 parse_progress=True)

        # ── Step 2: Flash filesystem ─────────────────────────────────
        fs_path = None
        if not self.opts.firmware_only and ffat_offset:
            for candidate in ["fatfs.bin", "littlefs.bin", "spiffs.bin"]:
                p = os.path.join(build_dir, candidate)
                if os.path.isfile(p):
                    fs_path = p
                    break

        if fs_path:
            self.state = S_FLASHING_FS
            self.log("▶ WRITING FILESYSTEM ...", AMBER)
            self.progress = 0
            fs_before = ("no_reset" if not self.opts.fs_only
                         else before_mode)
            fs_baud = self._fs_flash_baud()
            if fs_baud != self.opts.baud:
                self.log(f"  FS flash baud {fs_baud} "
                         "(lower for multi-port / fatfs reliability)",
                         DIM_AMBER)
            fs_cmd = self._esptool_cmd(fs_before,
                                       after="no_reset_stub",
                                       baud=fs_baud) + [
                ffat_offset, fs_path]
            fs_ok = self.run_cmd(fs_cmd, "write-filesystem",
                                 parse_progress=True)
            if not fs_ok:
                if self.progress >= 90:
                    self.log(f"  FS WRITE REACHED {self.progress}% "
                             "— TREATING AS SUCCESS", AMBER)
                    fs_ok = True
                else:
                    self.log("  RETRYING FILESYSTEM WRITE ...", AMBER)
                    self.kill_port_holders()
                    time.sleep(1.5)
                    self.progress = 0
                    fs_ok = self.run_cmd(
                        fs_cmd, "write-filesystem (retry)",
                        parse_progress=True)
                    if not fs_ok and self.progress >= 90:
                        self.log(f"  FS RETRY REACHED {self.progress}%"
                                 " — TREATING AS SUCCESS", AMBER)
                        fs_ok = True
                    elif not fs_ok:
                        self.log("  FS WRITE ERROR — CONTINUING "
                                 "(often benign at ~95%)", AMBER)

        elapsed = time.time() - self.flash_start_time
        self.last_flash_duration = elapsed

        # ── Step 3: Reset ────────────────────────────────────────────
        if fw_ok:
            self._post_flash_reset()

            if self.post_sync and self.port_exists():
                self._run_post_sync()

            self.state = S_SUCCESS
            status = "COMPLETE" if fs_ok else "COMPLETE (fs warning)"
            self.log(f"✓ #{self.count} {status}  [{elapsed:.1f}s]",
                     hsv_rgb(0.3, 0.8, 1.0))
            try:
                pygame.mixer.Sound(CHACHING_PATH).play()
            except Exception:
                pass
        else:
            self.state = S_FAILED
            self.log(f"✗ #{self.count} FLASH FAILURE  [{elapsed:.1f}s]",
                     RED)

        # ── Wait for unplug (with serial monitor) ────────────────────
        self.state = S_UNPLUG
        self.log("  REMOVE DEVICE TO CONTINUE ...", DIM_AMBER)

        if self.opts.monitor_serial and fw_ok and self.port_exists():
            mon = threading.Thread(target=self._serial_monitor, daemon=True)
            mon.start()

        while self.port_exists() and self.running:
            time.sleep(0.4)
        self.log("  DEVICE REMOVED.", DIM_PINK)


# ── Port manager ──────────────────────────────────────────────────────────────

class PortManager:
    def __init__(self, env: str, shared_log: SharedLog,
                 build_done: threading.Event, build_ok: list,
                 opts: argparse.Namespace):
        self.env = env
        self.shared_log = shared_log
        self.build_done = build_done
        self.build_ok = build_ok
        self.opts = opts
        self.stations: list[FlashStation] = []
        self.known_ports: set[str] = set()
        self.lock = threading.Lock()
        self.running = True
        self.scanning = True

    def _live_station_count(self) -> int:
        with self.lock:
            return max(1, len(self.stations))

    def seed_ports(self, ports: list[str]):
        for port in ports:
            self._add_port(port)

    def _add_port(self, port: str):
        with self.lock:
            if port in self.known_ports or port in IGNORE_PORTS:
                return
            self.known_ports.add(port)
            idx = len(self.stations)
            s = FlashStation(self.env, port, idx, self.shared_log,
                             self.opts, peer_count=self._live_station_count)
            s.post_sync = self.opts.post_sync
            self.stations.append(s)
            self.shared_log.add(f"▶ NEW PORT DETECTED: {port}", AMBER)
            if self.build_done.is_set() and self.build_ok[0]:
                s.start()

    def get_stations(self) -> list[FlashStation]:
        with self.lock:
            return list(self.stations)

    def start_all_ready(self):
        with self.lock:
            for s in self.stations:
                if s.worker is None:
                    s.start()

    def scan_once(self):
        if not self.scanning:
            return
        current = set(glob.glob("/dev/cu.usbmodem*"))
        for port in sorted(current - self.known_ports):
            self._add_port(port)

    def scan_loop(self):
        while self.running:
            self.scan_once()
            time.sleep(2.0)

    def start_scanner(self):
        t = threading.Thread(target=self.scan_loop, daemon=True)
        t.start()

    def shutdown(self):
        self.running = False
        with self.lock:
            for s in self.stations:
                s.stop()
            for s in self.stations:
                if s.worker and s.worker.is_alive():
                    s.worker.join(timeout=2.0)


# ── Rendering helpers ─────────────────────────────────────────────────────────

def build_scanline_overlay(w: int, h: int) -> pygame.Surface:
    overlay = pygame.Surface((w, h), pygame.SRCALPHA)
    for y in range(0, h, 3):
        pygame.draw.line(overlay, (0, 0, 0, 18), (0, y), (w, y))
    return overlay


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Rainbow Flash Terminal v3.0")
    parser.add_argument("env", nargs="?", default="echo-expanded",
                        help="PlatformIO env (default: echo-expanded)")
    parser.add_argument("--port", nargs="+", default=None,
                        help="Serial port(s) to flash")
    parser.add_argument("--post-sync", action="store_true",
                        help="Run badge_sync after flash")
    parser.add_argument("--no-build", action="store_true",
                        help="Skip build, flash last compiled binaries")
    parser.add_argument("--firmware-only", action="store_true",
                        help="Write only firmware, skip filesystem")
    parser.add_argument("--fs-only", action="store_true",
                        help="Write only filesystem, skip firmware")
    parser.add_argument("--baud", type=int, default=921600,
                        help="Flash baud rate (default: 921600)")
    parser.add_argument("--fs-baud", type=int, default=0,
                        dest="fs_baud",
                        help="Filesystem write baud only "
                             "(0=auto: min(main,460800) if 2+ ports)")
    parser.add_argument("--esptool-lock", action="store_true",
                        dest="esptool_serial_lock",
                        help="Run esptool one port at a time (use if a hub "
                             "drops concurrent flashes)")
    parser.add_argument("--verify", action="store_true",
                        help="Verify flash after write")
    parser.add_argument("--erase-all", action="store_true",
                        help="Erase entire flash before writing")
    parser.add_argument("--hues", type=int, default=64,
                        help="HSV hue steps for rainbow (16-360)")
    parser.add_argument("--no-monitor-serial", dest="monitor_serial",
                        action="store_false", default=True,
                        help="Disable post-flash serial monitor")
    args = parser.parse_args()
    args.hues = max(16, min(360, args.hues))

    pio_path = os.path.expanduser("~/.platformio/penv/bin/pio")
    if not os.path.isfile(pio_path):
        print(f"ERROR: pio not found at {pio_path}", file=sys.stderr)
        sys.exit(1)

    if args.port:
        ports = args.port
    else:
        ports = sorted(p for p in glob.glob("/dev/cu.usbmodem*")
                       if p not in IGNORE_PORTS)
        if not ports:
            print("No ports found — will auto-detect when plugged in",
                  file=sys.stderr)
            ports = []
    env = args.env
    n_hues = args.hues

    # ── Window layout ─────────────────────────────────────────────────
    header_h = 55
    footer_h = 28
    win_w = 1100
    min_log_h = 420
    port_panel_h = max(60, len(ports) * 32 + 12)
    win_h = header_h + port_panel_h + min_log_h + footer_h
    max_log_lines = (win_h - header_h - port_panel_h
                     - footer_h - 10) // LINE_H

    shared_log = SharedLog()

    pygame.init()
    screen = pygame.display.set_mode((win_w, win_h), pygame.RESIZABLE)
    pygame.display.set_caption("RAINBOW FLASH TERMINAL v3.0")
    clock = pygame.time.Clock()

    mono_candidates = [
        "SF Mono", "Menlo", "Monaco", "Courier New", "Courier"]
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

    char_w = font.size("X")[0]
    scanlines = build_scanline_overlay(win_w, win_h)

    # ── Banner ────────────────────────────────────────────────────────
    shared_log.add("", PINK)
    banner = [
        "╔═══════════════════════════════════════════════════════════╗",
        "║   RAINBOW FLASH TERMINAL  v3.0                           ║",
        "║   SHALL WE PLAY A GAME?                                  ║",
        "╚═══════════════════════════════════════════════════════════╝",
    ]
    for i, line in enumerate(banner):
        shared_log.add(line, rainbow_line_color(i, n_hues, 0))
    shared_log.add("", PINK)
    shared_log.add(f"TARGET ENV:   {env}", WHITE)
    port_str = ', '.join(ports) if ports else "(auto-detect)"
    shared_log.add(f"SERIAL PORTS: {port_str}", WHITE)
    flags = []
    if args.no_build:      flags.append("--no-build")
    if args.firmware_only:  flags.append("--firmware-only")
    if args.fs_only:        flags.append("--fs-only")
    if args.verify:         flags.append("--verify")
    if args.erase_all:      flags.append("--erase-all")
    if flags:
        shared_log.add(f"FLAGS:        {' '.join(flags)}", WHITE)
    shared_log.add("", PINK)

    # ── Build ─────────────────────────────────────────────────────────
    build_done = threading.Event()
    build_ok = [False]
    pio = os.path.expanduser("~/.platformio/penv/bin/pio")

    toggles = {"no_build": args.no_build}

    def run_build_cmd(cmd, label):
        shared_log.add(f"  > {' '.join(cmd)}", DIM_PINK)
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1)
        assert proc.stdout is not None
        for line in proc.stdout:
            stripped = ANSI_RE.sub('', line.rstrip()).strip()
            if stripped:
                shared_log.add(f"    {stripped}", DIM_PINK)
        proc.wait()
        if proc.returncode != 0:
            shared_log.add(f"✗ {label} exited {proc.returncode}", RED)
            return False
        return True

    def do_build():
        if toggles["no_build"]:
            shared_log.add("▶ SKIPPING BUILD (--no-build)", AMBER)
            build_ok[0] = True
            build_done.set()
            return
        shared_log.add("▶ COMPILING FIRMWARE + FILESYSTEM ...", AMBER)
        try:
            if not run_build_cmd(
                    [pio, "run", "-e", env, "-d", SCRIPT_DIR],
                    "pio build"):
                shared_log.add("✗ FIRMWARE BUILD FAILED — ABORT", RED)
                build_done.set()
                return
            if not run_build_cmd(
                    [pio, "run", "-e", env, "-t", "buildfs",
                     "-d", SCRIPT_DIR], "pio buildfs"):
                shared_log.add("✗ FILESYSTEM BUILD FAILED — ABORT", RED)
                build_done.set()
                return
            shared_log.add("✓ BUILD COMPLETE — READY TO FLASH",
                           hsv_rgb(0.3, 0.8, 1.0))
            shared_log.add("", PINK)
            build_ok[0] = True
        except Exception as e:
            shared_log.add(f"✗ BUILD EXCEPTION: {e}", RED)
        build_done.set()

    build_thread = threading.Thread(target=do_build, daemon=True)
    build_thread.start()

    # ── Port manager ──────────────────────────────────────────────────
    port_mgr = PortManager(env, shared_log, build_done, build_ok,
                           opts=args)
    port_mgr.seed_ports(ports)
    atexit.register(port_mgr.shutdown)

    def start_stations():
        build_done.wait()
        if build_ok[0]:
            stations = port_mgr.get_stations()
            shared_log.add(
                f"▶ LAUNCHING {len(stations)} FLASH STATION(S)", AMBER)
            shared_log.add("━" * 58, DIM_PINK)
            port_mgr.start_all_ready()
            port_mgr.start_scanner()

    starter = threading.Thread(target=start_stations, daemon=True)
    starter.start()

    # ── Text selection state ──────────────────────────────────────────
    sel_anchor: tuple[int, int] | None = None
    sel_end: tuple[int, int] | None = None
    sel_dragging = False
    log_y_global = 0

    def pos_to_line_char(mx, my, log_y, n_visible):
        li = max(0, min((my - log_y) // LINE_H, n_visible - 1))
        ci = max(0, (mx - 16) // char_w)
        return (li, ci)

    def get_selected_text(visible_lines):
        if sel_anchor is None or sel_end is None:
            return ""
        a, b = sel_anchor, sel_end
        if a > b:
            a, b = b, a
        parts = []
        for i in range(a[0], min(b[0] + 1, len(visible_lines))):
            text = visible_lines[i][0]
            if i == a[0] and i == b[0]:
                parts.append(text[a[1]:b[1]])
            elif i == a[0]:
                parts.append(text[a[1]:])
            elif i == b[0]:
                parts.append(text[:b[1]])
            else:
                parts.append(text)
        return "\n".join(parts)

    # ── Event loop ────────────────────────────────────────────────────
    tick = 0
    scroll_offset = 0
    auto_scroll = True
    prev_station_count = 0
    selected_port_idx = 0

    try:
        while True:
            stations = port_mgr.get_stations()
            n_stations = len(stations)

            if n_stations != prev_station_count:
                prev_station_count = n_stations
                port_panel_h = max(60, n_stations * 32 + 12)
                win_h = (header_h + port_panel_h
                         + min_log_h + footer_h)
                max_log_lines = (win_h - header_h - port_panel_h
                                 - footer_h - 10) // LINE_H
                screen = pygame.display.set_mode(
                    (win_w, win_h), pygame.RESIZABLE)
                scanlines = build_scanline_overlay(win_w, win_h)

            mods = pygame.key.get_mods()
            cmd_ctrl = bool(mods & (pygame.KMOD_META | pygame.KMOD_CTRL))

            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    raise KeyboardInterrupt

                elif event.type == pygame.VIDEORESIZE:
                    win_w, win_h = event.w, event.h
                    max_log_lines = (win_h - header_h - port_panel_h
                                     - footer_h - 10) // LINE_H
                    screen = pygame.display.set_mode(
                        (win_w, win_h), pygame.RESIZABLE)
                    scanlines = build_scanline_overlay(win_w, win_h)

                elif event.type == pygame.KEYDOWN:
                    k = event.key
                    if k in (pygame.K_q, pygame.K_ESCAPE):
                        raise KeyboardInterrupt
                    elif k == pygame.K_UP:
                        scroll_offset = min(
                            scroll_offset + 3,
                            max(0, shared_log.total - max_log_lines))
                        auto_scroll = False
                    elif k == pygame.K_DOWN:
                        scroll_offset = max(0, scroll_offset - 3)
                        if scroll_offset == 0:
                            auto_scroll = True
                    elif k == pygame.K_END:
                        scroll_offset = 0
                        auto_scroll = True
                    elif k == pygame.K_HOME:
                        scroll_offset = max(
                            0, shared_log.total - max_log_lines)
                        auto_scroll = False
                    elif pygame.K_1 <= k <= pygame.K_9:
                        idx = k - pygame.K_1
                        if idx < n_stations:
                            stations[idx].toggle()
                            selected_port_idx = idx
                    elif k == pygame.K_c and cmd_ctrl:
                        vis = _get_visible(shared_log, max_log_lines,
                                           scroll_offset)
                        text = get_selected_text(vis)
                        if text:
                            clipboard_copy(text)
                            shared_log.add(
                                f"  COPIED {len(text)} chars", DIM_PINK)
                    elif k == pygame.K_a and cmd_ctrl:
                        vis = _get_visible(shared_log, max_log_lines,
                                           scroll_offset)
                        sel_anchor = (0, 0)
                        sel_end = (max(0, len(vis) - 1), 200)
                    elif k == pygame.K_r and not cmd_ctrl:
                        shared_log.add("▶ REBUILD REQUESTED", AMBER)

                        def _rebuild():
                            do_build()
                            if build_ok[0]:
                                port_mgr.start_all_ready()

                        threading.Thread(target=_rebuild,
                                         daemon=True).start()
                    elif k == pygame.K_f and not cmd_ctrl:
                        idx = selected_port_idx
                        if idx < n_stations:
                            shared_log.add(
                                f"▶ FORCE RESET: "
                                f"{stations[idx].short}", AMBER)
                            threading.Thread(
                                target=stations[idx].dtr_rts_reset,
                                args=(False,), daemon=True).start()
                    elif k == pygame.K_b and not cmd_ctrl:
                        toggles["no_build"] = not toggles["no_build"]
                        lbl = "SKIP" if toggles["no_build"] else "ON"
                        shared_log.add(f"  BUILD: {lbl}", AMBER)
                    elif k == pygame.K_v and not cmd_ctrl:
                        args.verify = not args.verify
                        lbl = "ON" if args.verify else "OFF"
                        shared_log.add(f"  VERIFY: {lbl}", AMBER)
                    elif k == pygame.K_p and not cmd_ctrl:
                        port_mgr.scanning = not port_mgr.scanning
                        lbl = "ON" if port_mgr.scanning else "PAUSED"
                        shared_log.add(f"  AUTO-DETECT: {lbl}", AMBER)

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

                elif (event.type == pygame.MOUSEBUTTONDOWN
                      and event.button == 1):
                    mx, my = event.pos
                    if my >= log_y_global:
                        vis = _get_visible(shared_log, max_log_lines,
                                           scroll_offset)
                        sel_anchor = pos_to_line_char(
                            mx, my, log_y_global, len(vis))
                        sel_end = sel_anchor
                        sel_dragging = True

                elif event.type == pygame.MOUSEMOTION and sel_dragging:
                    mx, my = event.pos
                    vis = _get_visible(shared_log, max_log_lines,
                                       scroll_offset)
                    sel_end = pos_to_line_char(
                        mx, my, log_y_global, len(vis))

                elif (event.type == pygame.MOUSEBUTTONUP
                      and event.button == 1):
                    sel_dragging = False
                    if sel_anchor == sel_end:
                        sel_anchor = None
                        sel_end = None

            if auto_scroll:
                scroll_offset = 0

            screen.fill(BG)

            # ── HEADER ────────────────────────────────────────────────
            pygame.draw.rect(screen, HEADER_BG,
                             (0, 0, win_w, header_h))
            pygame.draw.line(screen, DARK_PINK,
                             (0, header_h), (win_w, header_h))

            title_col = rainbow_line_color(tick // 4, n_hues, tick)
            title_surf = font_big.render(
                "RAINBOW FLASH TERMINAL", True, title_col)
            screen.blit(title_surf, (20, 6))

            ts = time.strftime("%H:%M:%S")
            clock_surf = font.render(ts, True, DIM_PINK)
            screen.blit(clock_surf,
                         (win_w - clock_surf.get_width() - 20, 8))

            total_flashed = sum(s.count for s in stations)
            active_count = sum(
                1 for s in stations
                if s.state in (S_ERASING, S_FLASHING, S_FLASHING_FS))
            plugged_count = sum(
                1 for s in stations if s.port_exists())

            summary = font.render(
                f"ENV: {env}   "
                f"PORTS: {n_stations}   "
                f"PLUGGED: {plugged_count}   "
                f"ACTIVE: {active_count}   "
                f"TOTAL FLASHED: {total_flashed}",
                True, DIM_PINK)
            screen.blit(summary, (20, 34))

            # ── PORT STATUS PANEL ─────────────────────────────────────
            panel_y = header_h + 1
            pygame.draw.rect(screen, PANEL_BG,
                             (0, panel_y, win_w, port_panel_h))
            pygame.draw.line(screen, DARK_PINK,
                             (0, panel_y + port_panel_h),
                             (win_w, panel_y + port_panel_h))

            py = panel_y + 6
            for i, s in enumerate(stations):
                bhue = port_base_hue(i, max(n_stations, 1))

                if not s.enabled:
                    col = DIM_RED
                    status_text = "DISABLED"
                    sp = ""
                else:
                    col = state_color(bhue, s.state, tick)
                    status_text = s.state
                    sp = ""
                    if s.state in (S_ERASING, S_FLASHING,
                                   S_FLASHING_FS, S_WAITING):
                        sp = spinner_char(tick) + " "

                dot = (hsv_rgb(bhue, 0.8, 1.0)
                       if s.port_exists() else DIM_RED)
                if not s.enabled:
                    dot = (60, 60, 60)
                pygame.draw.circle(screen, dot, (30, py + 8), 5)

                time_str = ""
                if (s.state in (S_FLASHING, S_FLASHING_FS, S_ERASING)
                        and s.flash_start_time):
                    el = time.time() - s.flash_start_time
                    time_str = f"  {el:.1f}s"
                elif s.last_flash_duration is not None:
                    time_str = f"  LAST: {s.last_flash_duration:.1f}s"

                line_text = (
                    f"[{i + 1}] {s.short:<22s}  "
                    f"{sp}{status_text:<20s}  "
                    f"FLASHED: {s.count}{time_str}")
                surf = font.render(line_text, True, col)
                screen.blit(surf, (44, py))

                flash_prog_states = (
                    S_FLASHING, S_FLASHING_FS, S_ERASING)
                if s.enabled and s.state in flash_prog_states:
                    strip_w = 168
                    bar_x = win_w - strip_w - 8
                    pygame.draw.rect(
                        screen, PANEL_BG,
                        (bar_x - 4, py - 1, strip_w + 8, LINE_H + 2))
                    bar_w, bar_h = 108, 11
                    bar_y = py + 5
                    pygame.draw.rect(
                        screen, (40, 10, 30),
                        (bar_x, bar_y, bar_w, bar_h))
                    fill = max(0, min(100, s.progress))
                    fill_w = int(bar_w * fill / 100) if fill else 0
                    if fill_w:
                        pygame.draw.rect(
                            screen, hsv_rgb(bhue, 0.9, 0.9),
                            (bar_x, bar_y, fill_w, bar_h))
                    if s.state == S_ERASING and fill == 0:
                        pct_label = "erase"
                    else:
                        pct_label = f"{fill}%" if fill else "…"
                    pct_surf = font.render(pct_label, True, col)
                    screen.blit(pct_surf, (bar_x + bar_w + 6, py + 2))

                py += 32

            # ── LOG AREA ──────────────────────────────────────────────
            log_y = panel_y + port_panel_h + 4
            log_y_global = log_y

            visible = _get_visible(shared_log, max_log_lines,
                                   scroll_offset)

            # Selection highlight
            if sel_anchor is not None and sel_end is not None:
                a, b = sel_anchor, sel_end
                if a > b:
                    a, b = b, a
                hl = pygame.Surface((win_w, LINE_H), pygame.SRCALPHA)
                hl.fill((80, 20, 60, 140))
                for li in range(a[0], min(b[0] + 1, len(visible))):
                    screen.blit(hl, (0, log_y + li * LINE_H))

            y = log_y
            for vi, (text, _colour, pidx) in enumerate(visible):
                if pidx is not None and pidx < n_stations:
                    bhue = port_base_hue(pidx, max(n_stations, 1))
                    rc = state_color(bhue, stations[pidx].state, tick)
                else:
                    rc = rainbow_line_color(vi, n_hues, tick)
                surf = font.render(text, True, rc)
                screen.blit(surf, (16, y))
                y += LINE_H

            # ── FOOTER ────────────────────────────────────────────────
            fy = win_h - footer_h
            pygame.draw.line(screen, DARK_PINK, (0, fy), (win_w, fy))
            pygame.draw.rect(screen, HEADER_BG,
                             (0, fy, win_w, footer_h))

            scan_lbl = "ON" if port_mgr.scanning else "PAUSED"
            build_lbl = "SKIP" if toggles["no_build"] else "ON"
            verify_lbl = "ON" if args.verify else "OFF"
            hint = font.render(
                f"  [Q]uit [1-9]toggle [R]ebuild "
                f"[F]reset [B]uild:{build_lbl} "
                f"[V]erify:{verify_lbl} [P]scan:{scan_lbl} "
                f"Ctrl+C:copy Ctrl+A:sel",
                True, DIM_PINK)
            screen.blit(hint, (10, fy + 5))

            # ── Noise pixels (rainbow) ────────────────────────────────
            for _ in range(6):
                rx = random.randint(0, win_w - 1)
                ry = random.randint(0, win_h - 1)
                screen.set_at(
                    (rx, ry),
                    rainbow_line_color(rx + ry, n_hues, tick))

            screen.blit(scanlines, (0, 0))
            pygame.display.flip()
            clock.tick(FPS)
            tick += 1

    except KeyboardInterrupt:
        pass
    finally:
        port_mgr.shutdown()
        pygame.quit()
        total = sum(s.count for s in port_mgr.get_stations())
        n = len(port_mgr.get_stations())
        print(f"\nFlashed {total} badge(s) across {n} port(s). "
              "THE ONLY WINNING MOVE IS NOT TO PLAY.")


def _get_visible(shared_log: SharedLog, max_lines: int,
                 scroll_offset: int) -> list[tuple[str, tuple, int | None]]:
    all_lines = shared_log.get_lines(max_lines + scroll_offset)
    total = len(all_lines)
    vis_start = max(0, total - max_lines - scroll_offset)
    vis_end = max(0, total - scroll_offset)
    return all_lines[vis_start:vis_end]


if __name__ == "__main__":
    main()
