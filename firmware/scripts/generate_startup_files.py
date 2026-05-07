#!/usr/bin/env python3
"""
Generate StartupFilesData.h from initial_filesystem/

Scans the initial_filesystem/ directory tree and embeds every file as a C
constant (raw string literal for text, uint8_t array for binary).  Each file
gets an FNV-1a hash and a hash-history array so the runtime provisioning can
detect user-modified files vs. old firmware defaults.

Usage:
  - Automatic: listed in platformio.ini as  extra_scripts = pre:scripts/generate_startup_files.py
  - Manual:    python3 scripts/generate_startup_files.py
"""

import json
import os
import sys
from pathlib import Path


# ── Configuration ────────────────────────────────────────────────────────────

BINARY_EXTENSIONS = {'.bin', '.bmp', '.png', '.jpg', '.jpeg', '.gif', '.ico', '.raw', '.pbm', '.xbm', '.fb'}
PROTECTED_FILENAMES = set()
SKIP_DIR_NAMES = {'__pycache__'}
SKIP_EXTENSIONS = {'.pyc', '.pyo', '.wad'}


# ── FNV-1a (must match the C++ implementation) ──────────────────────────────

def fnv1a32(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


# ── Hash history persistence ────────────────────────────────────────────────

def load_hash_history(path: Path) -> dict:
    if path.exists():
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    return {}


def save_hash_history(path: Path, history: dict) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(dict(sorted(history.items())), f, indent=2)
        f.write("\n")


def update_history(history: dict, key: str, current_hash: int) -> list:
    current_hex = f"0x{current_hash:08X}"
    existing = history.get(key, [])
    deduped = [h for h in existing if h.lower() != current_hex.lower()]
    updated = [current_hex] + deduped
    history[key] = updated
    return updated


# ── C code generation helpers ────────────────────────────────────────────────

def is_binary(path: Path) -> bool:
    return path.suffix.lower() in BINARY_EXTENSIONS


def text_to_c_raw_string(content: str) -> str:
    if ')"' in content:
        delimiter = "==="
        attempt = 0
        while f'){delimiter}"' in content and attempt < 10:
            delimiter += "="
            attempt += 1
        return f'R"{delimiter}({content}){delimiter}"'
    return f'R"({content})"'


def bytes_to_c_array(data: bytes) -> str:
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk))
    return ",\n".join(lines)


def rel_path_to_var(rel_path: str) -> str:
    name = rel_path.lstrip('/').replace('/', '_').replace('.', '_').replace('-', '_')
    return f"STARTUP_{name.upper()}"


# ── Timestamp-based skip ────────────────────────────────────────────────────

def needs_regeneration(initial_fs_dir: Path, output_file: Path, script_file: Path = None) -> bool:
    if not output_file.exists():
        return True
    out_mtime = output_file.stat().st_mtime
    if script_file is not None and script_file.exists() and script_file.stat().st_mtime > out_mtime:
        return True
    for p in initial_fs_dir.rglob('*'):
        if p.stat().st_mtime > out_mtime:
            return True
    return False


# ── Main generation ─────────────────────────────────────────────────────────

def generate(project_dir: Path, script_file: Path = None):
    initial_fs_dir = project_dir / 'initial_filesystem'
    output_file = project_dir / 'src' / 'micropython' / 'StartupFilesData.h'
    history_file = project_dir / 'scripts' / 'startup_hash_history.json'

    if not initial_fs_dir.exists():
        print(f"[generate_startup_files] WARNING: {initial_fs_dir} not found, generating empty header")
        output_file.write_text(
            '#pragma once\n'
            '#include <stdint.h>\n\n'
            'struct StartupFileInfo { const char* path; const char* content;\n'
            '    uint32_t contentLen; const uint32_t* knownHashes;\n'
            '    int hashCount; uint8_t flags; };\n\n'
            'static const StartupFileInfo kStartupFiles[] = { {nullptr,nullptr,0,nullptr,0,0} };\n'
            'static const int kStartupFileCount = 0;\n'
            'static const char* kStartupDirs[] = { nullptr };\n'
            'static const int kStartupDirCount = 0;\n'
        )
        return

    if not needs_regeneration(initial_fs_dir, output_file, script_file):
        print("[generate_startup_files] Up to date, skipping")
        return

    print("[generate_startup_files] Scanning initial_filesystem/ ...")

    history = load_hash_history(history_file)
    files = []
    dirs = set()
    current_paths = set()

    for path in sorted(initial_fs_dir.rglob('*')):
        rel_parts = path.relative_to(initial_fs_dir).parts
        if any(part.startswith('.') or part in SKIP_DIR_NAMES for part in rel_parts):
            continue
        if path.suffix.lower() in SKIP_EXTENSIONS:
            print(f"  SKIP (not embedded): {path.name}")
            continue
        if path.is_dir():
            rel = '/' + path.relative_to(initial_fs_dir).as_posix()
            dirs.add(rel)
            continue

        rel_path = '/' + path.relative_to(initial_fs_dir).as_posix()
        current_paths.add(rel_path)
        protected = path.name in PROTECTED_FILENAMES
        content_bytes = path.read_bytes()
        content_hash = fnv1a32(content_bytes)
        all_hashes = update_history(history, rel_path, content_hash)
        binary = is_binary(path)

        files.append({
            'rel_path': rel_path,
            'var': rel_path_to_var(rel_path),
            'bytes': content_bytes,
            'text': None if binary else content_bytes.decode('utf-8', errors='replace'),
            'binary': binary,
            'hash': content_hash,
            'all_hashes': all_hashes,
            'protected': protected,
            'size': len(content_bytes),
        })

        tag = " [PROTECTED]" if protected else ""
        print(f"  {rel_path} ({len(content_bytes)} bytes, 0x{content_hash:08X}{tag})")

    history = {key: value for key, value in history.items() if key in current_paths}
    save_hash_history(history_file, history)

    # ── Build the header ─────────────────────────────────────────────────────

    L = []  # output lines
    L.append('#pragma once')
    L.append('// ═══════════════════════════════════════════════════════════════════════')
    L.append('// AUTO-GENERATED by scripts/generate_startup_files.py')
    L.append('// DO NOT EDIT — modify files in initial_filesystem/ and rebuild.')
    L.append('// ═══════════════════════════════════════════════════════════════════════')
    L.append('')
    L.append('#include <stdint.h>')
    L.append('')
    L.append('#define STARTUP_FILE_PROTECTED  (1 << 0)')
    L.append('')
    L.append('struct StartupFileInfo {')
    L.append('    const char* path;')
    L.append('    const char* content;')
    L.append('    uint32_t contentLen;')
    L.append('    const uint32_t* knownHashes;')
    L.append('    int hashCount;')
    L.append('    uint8_t flags;')
    L.append('};')
    L.append('')

    # ── File contents ────────────────────────────────────────────────────────

    L.append('// ─── Embedded file contents ──────────────────────────────────────────')
    L.append('')

    for f in files:
        v = f['var']
        if f['binary']:
            L.append(f'static const uint8_t {v}_DATA[] = {{')
            L.append(bytes_to_c_array(f['bytes']))
            L.append('};')
            L.append(f'static const uint32_t {v}_LEN = sizeof({v}_DATA);')
        else:
            L.append(f'static const char {v}_DATA[] = {text_to_c_raw_string(f["text"])};')
        hashes_str = ', '.join(f['all_hashes'])
        count = len(f['all_hashes'])
        L.append(f'static const uint32_t {v}_HASHES[{count}] = {{ {hashes_str} }};')
        L.append('')

    # ── File table ───────────────────────────────────────────────────────────

    L.append('// ─── File table ───────────────────────────────────────────────────────')
    L.append('')
    L.append('static const StartupFileInfo kStartupFiles[] = {')

    for f in files:
        v = f['var']
        flags = 'STARTUP_FILE_PROTECTED' if f['protected'] else '0'
        if f['binary']:
            content_expr = f'(const char*){v}_DATA'
            size_expr = f'{v}_LEN'
        else:
            content_expr = f'{v}_DATA'
            size_expr = f'sizeof({v}_DATA) - 1'
        count = len(f['all_hashes'])
        L.append(f'    {{ "{f["rel_path"]}", {content_expr}, {size_expr}, {v}_HASHES, {count}, {flags} }},')

    L.append('};')
    L.append(f'static const int kStartupFileCount = {len(files)};')
    L.append('')

    # ── Managed directories ──────────────────────────────────────────────────

    sorted_dirs = sorted(dirs)
    L.append('// ─── Managed directories ─────────────────────────────────────────────')
    L.append('')
    if sorted_dirs:
        L.append('static const char* kStartupDirs[] = {')
        for d in sorted_dirs:
            L.append(f'    "{d}",')
        L.append('};')
    else:
        L.append('static const char* kStartupDirs[] = { nullptr };')
    L.append(f'static const int kStartupDirCount = {len(sorted_dirs)};')
    L.append('')

    # ── Write output ─────────────────────────────────────────────────────────

    output_content = '\n'.join(L).rstrip() + '\n'

    if output_file.exists() and output_file.read_text(encoding='utf-8') == output_content:
        print(f"[generate_startup_files] {output_file.name} content unchanged")
        return

    output_file.write_text(output_content, encoding='utf-8')
    print(f"[generate_startup_files] Generated {output_file.name} ({len(files)} files, {len(sorted_dirs)} dirs)")


# ── Entry points ─────────────────────────────────────────────────────────────

try:
    Import("env")  # type: ignore  # PlatformIO SCons global
    _proj = Path(env.subst("$PROJECT_DIR"))  # type: ignore
    _script = _proj / "scripts" / "generate_startup_files.py"
    generate(_proj, _script if _script.exists() else None)
except NameError:
    # Running standalone (python3 scripts/generate_startup_files.py)
    _sf = Path(__file__)
    generate(_sf.parent.parent, _sf)
