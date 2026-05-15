#!/usr/bin/env python3
"""
Regenerate registry/community_apps.json from three input sources:

  1. firmware/initial_filesystem/apps/                  (built-in apps)
  2. community/<app-id>/                                (PR'd contributions)
  3. community/external.json                            (external-URL-only)

The output schema matches registry/community_apps.schema.json (v2). Any
contributor can land a new app by either adding a folder under
community/<app-id>/ or appending an entry to community/external.json —
both are handled identically by the on-badge installer (kind:"app" or
kind:"file" entries vended through the Community Apps screen).

Usage:
  python3 scripts/regenerate_community_apps.py                # write
  python3 scripts/regenerate_community_apps.py --check        # CI mode

`--check` exits non-zero if the regenerated content differs from the
on-disk registry/community_apps.json — the GitHub Action uses this for
PR validation and re-runs without `--check` on push to main.

Validation:
  - JSON Schema (registry/community_apps.schema.json) — every emitted
    entry must validate.
  - id uniqueness across all three sources.
  - per-asset size budget: 256 KB for in-repo community apps.
  - basic Python AST parse for community/<id>/*.py.
  - rejects binary blobs > 64 KB inside community/.
"""
from __future__ import annotations

import argparse
import ast
import hashlib
import json
import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DATA_DIR = REPO_ROOT / "firmware" / "initial_filesystem"
COMMUNITY_DIR = REPO_ROOT / "community"
EXTERNAL_JSON = COMMUNITY_DIR / "external.json"
SCHEMA_PATH = REPO_ROOT / "registry" / "community_apps.schema.json"
OUTPUT_PATH = REPO_ROOT / "registry" / "community_apps.json"
REGISTRY_LEGACY = REPO_ROOT / "registry" / "registry.json"

DEFAULT_RAW_BASE = (
    "https://raw.githubusercontent.com/"
    "Architeuthis-Flux/Temporal-Replay-26-Badge/main/firmware/initial_filesystem"
)
COMMUNITY_RAW_BASE = (
    "https://raw.githubusercontent.com/"
    "Architeuthis-Flux/Temporal-Replay-26-Badge/main/community"
)

# Per-app size budget for in-repo PR'd community apps. Apps larger than
# this should be hosted externally and added through community/external.json
# instead so they don't bloat the repo's git history.
COMMUNITY_APP_SIZE_BUDGET = 256 * 1024
COMMUNITY_BINARY_FILE_LIMIT = 64 * 1024
ID_RE = re.compile(r"^[a-z0-9][a-z0-9_\-]{0,30}[a-z0-9]$")
TEXT_EXT = {
    ".py", ".md", ".txt", ".json", ".toml", ".cfg", ".ini",
}


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def is_text_file(path: Path) -> bool:
    return path.suffix.lower() in TEXT_EXT


LOOSE_FILE_DIRS = ("docs", "images", "messages")
LOOSE_ROOT_FILES = ("API_REFERENCE.md", "messages.json")
SKIP_LOOSE_EXT = {".pyc", ".pyo", ".wad", ".bin"}


def discover_builtin_loose_files() -> list[dict]:
    """Pull single-file installable assets out of
    firmware/initial_filesystem/{docs,images,messages}/ and the loose
    root files. Mirrors the kind:"file" half of generate_startup_files.py
    so the on-badge registry stays a complete catalog."""
    out: list[dict] = []
    if not DATA_DIR.is_dir():
        return out
    candidates: list[Path] = []
    for sub in LOOSE_FILE_DIRS:
        d = DATA_DIR / sub
        if d.is_dir():
            candidates.extend(p for p in d.rglob("*") if p.is_file())
    for name in LOOSE_ROOT_FILES:
        p = DATA_DIR / name
        if p.is_file():
            candidates.append(p)
    # Loose top-level apps (e.g. apps/hello.py — single-file scripts).
    apps_root = DATA_DIR / "apps"
    if apps_root.is_dir():
        for p in apps_root.iterdir():
            if (p.is_file() and p.suffix == ".py"
                    and p.name.lower() != "readme.md"):
                candidates.append(p)
    for fp in sorted(candidates):
        if fp.suffix.lower() in SKIP_LOOSE_EXT:
            continue
        rel = "/" + fp.relative_to(DATA_DIR).as_posix()
        data = fp.read_bytes()
        sha = sha256_hex(data)
        stem_id = rel.lstrip("/").replace("/", "-").replace(".", "-")[:32]
        out.append({
            "id": stem_id,
            "kind": "file",
            "name": fp.name[:47],
            "version": sha[:12],
            "dest_path": rel,
            "url": f"{DEFAULT_RAW_BASE}{rel}",
            "sha256": sha,
            "size": len(data),
            "min_free_bytes": len(data) + 1024,
            "description": f"Optional asset ({rel})"[:95],
        })
    return out


def discover_builtin_apps() -> list[dict]:
    """Walk firmware/initial_filesystem/apps/<name>/ — exactly the same
    shape generate_startup_files.py produces. We replicate the small
    slice of that logic here instead of importing it so this script is
    self-contained for CI."""
    out: list[dict] = []
    apps_root = DATA_DIR / "apps"
    if not apps_root.is_dir():
        return out
    for app_dir in sorted(p for p in apps_root.iterdir() if p.is_dir()):
        name = app_dir.name
        if name.startswith("."):
            continue
        files = sorted(p for p in app_dir.rglob("*") if p.is_file())
        if not files:
            continue
        bundle = []
        size_total = 0
        for fp in files:
            data = fp.read_bytes()
            rel = "/" + fp.relative_to(app_dir).as_posix()
            bundle.append({
                "path": rel,
                "size": len(data),
                "sha256": sha256_hex(data),
                "url": f"{DEFAULT_RAW_BASE}/apps/{name}{rel}",
            })
            size_total += len(data)
        version = hashlib.sha256(
            b"".join(f["sha256"].encode() for f in bundle)
        ).hexdigest()[:12]
        out.append({
            "id": name.replace("_", "-")[:32],
            "kind": "app",
            "name": name.replace("_", " ").title(),
            "version": version,
            "dest_dir": f"/apps/{name}",
            "size": size_total,
            "min_free_bytes": size_total + 4096,
            "description": _docstring_or(app_dir, name),
            "files": bundle,
        })
    return out


def _docstring_or(app_dir: Path, fallback_name: str) -> str:
    main_py = app_dir / "main.py"
    if main_py.is_file():
        try:
            tree = ast.parse(main_py.read_text(encoding="utf-8"))
            doc = ast.get_docstring(tree)
            if doc:
                first = doc.strip().splitlines()[0].strip()
                if first:
                    return first[:95]
        except SyntaxError:
            pass
    return fallback_name.replace("_", " ").title()


def discover_community_apps() -> list[dict]:
    """Walk community/<id>/ — same shape as built-in apps but with their
    own raw_base (community/) and stricter validation (size budget,
    Python AST parse, per-file binary cap)."""
    out: list[dict] = []
    if not COMMUNITY_DIR.is_dir():
        return out
    errors: list[str] = []
    for app_dir in sorted(p for p in COMMUNITY_DIR.iterdir() if p.is_dir()):
        name = app_dir.name
        if name.startswith("."):
            continue
        app_id = name.replace("_", "-")[:32]
        if not ID_RE.match(app_id):
            errors.append(
                f"community/{name}: folder name must match {ID_RE.pattern}")
            continue
        manifest = _read_manifest_toml(app_dir / "manifest.toml")
        files = sorted(p for p in app_dir.rglob("*")
                       if p.is_file() and p.name != "manifest.toml")
        if not files:
            errors.append(f"community/{name}: no files (skipping)")
            continue
        bundle = []
        size_total = 0
        for fp in files:
            data = fp.read_bytes()
            if len(data) > COMMUNITY_BINARY_FILE_LIMIT and not is_text_file(fp):
                errors.append(
                    f"community/{name}/{fp.relative_to(app_dir)}: "
                    f"binary blob {len(data)} B exceeds "
                    f"{COMMUNITY_BINARY_FILE_LIMIT} B cap")
                continue
            if fp.suffix == ".py":
                try:
                    ast.parse(data.decode("utf-8", errors="replace"))
                except SyntaxError as exc:
                    errors.append(
                        f"community/{name}/{fp.relative_to(app_dir)}: "
                        f"Python syntax error: {exc}")
                    continue
            rel = "/" + fp.relative_to(app_dir).as_posix()
            bundle.append({
                "path": rel,
                "size": len(data),
                "sha256": sha256_hex(data),
                "url": f"{COMMUNITY_RAW_BASE}/{name}{rel}",
            })
            size_total += len(data)
        if size_total > COMMUNITY_APP_SIZE_BUDGET:
            errors.append(
                f"community/{name}: total {size_total} B exceeds "
                f"{COMMUNITY_APP_SIZE_BUDGET} B in-repo budget — "
                f"please host externally and submit through community/external.json")
            continue
        if not bundle:
            continue
        version = hashlib.sha256(
            b"".join(f["sha256"].encode() for f in bundle)
        ).hexdigest()[:12]
        out.append({
            "id": app_id,
            "kind": "app",
            "name": manifest.get("name", name.replace("_", " ").title())[:47],
            "version": version,
            "dest_dir": f"/apps/{name}",
            "size": size_total,
            "min_free_bytes": size_total + 4096,
            "description": manifest.get(
                "description", _docstring_or(app_dir, name))[:95],
            "files": bundle,
        })
    if errors:
        for e in errors:
            print(f"[regenerate_community_apps] {e}", file=sys.stderr)
        if any("syntax error" in e or "exceeds" in e or "must match" in e
               for e in errors):
            raise SystemExit(2)
    return out


def _read_manifest_toml(path: Path) -> dict:
    """Minimal TOML reader for the small set of keys we care about
    (name / description / author / min_firmware). Avoids the runtime
    `tomllib` import on Python < 3.11."""
    if not path.is_file():
        return {}
    out: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, _, v = line.partition("=")
        v = v.strip()
        if v.startswith('"') and v.endswith('"'):
            v = v[1:-1]
        out[k.strip()] = v
    return out


def load_external_assets() -> list[dict]:
    if not EXTERNAL_JSON.is_file():
        return []
    raw = json.loads(EXTERNAL_JSON.read_text(encoding="utf-8"))
    return list(raw.get("assets", []) or [])


def merge_legacy_registry(assets: list[dict]) -> None:
    """Pull the curated `registry/registry.json` (schema v1) entries
    forward — same dedup logic as generate_startup_files.py. Lets the
    DOOM WAD and other big GitHub-release-hosted assets keep showing
    up in Community Apps without having to migrate the v1 file."""
    if not REGISTRY_LEGACY.is_file():
        return
    try:
        legacy = json.loads(REGISTRY_LEGACY.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return
    existing_dests = {a.get("dest_path") for a in assets if "dest_path" in a}
    existing_ids = {a["id"] for a in assets}
    for entry in legacy.get("assets", []) or []:
        dp = entry.get("dest_path")
        if not dp or dp in existing_dests:
            continue
        if entry.get("id") in existing_ids:
            continue
        merged = {
            "id": entry.get("id", dp.lstrip("/").replace("/", "-")[:32]),
            "kind": "file",
            "name": entry.get("name", dp.rsplit("/", 1)[-1]),
            "version": entry.get("version", "1"),
            "dest_path": dp,
            "url": entry["url"],
            "size": entry.get("size", 0),
            "min_free_bytes": entry.get(
                "min_free_bytes", int(entry.get("size", 0)) + 4096),
            "description": entry.get("description", ""),
        }
        if entry.get("sha256"):
            merged["sha256"] = entry["sha256"]
        assets.append(merged)
        existing_dests.add(dp)
        existing_ids.add(merged["id"])


def validate_unique_ids(assets: list[dict]) -> None:
    seen: dict[str, str] = {}
    for a in assets:
        aid = a.get("id", "")
        if aid in seen:
            raise SystemExit(
                f"[regenerate_community_apps] duplicate id '{aid}' "
                f"(sources: {seen[aid]} and {a.get('kind', '?')})")
        seen[aid] = a.get("kind", "?")


def validate_against_schema(payload: dict) -> None:
    """Optional JSON-Schema validation. We only run it when jsonschema
    is importable so the script stays usable on a vanilla Python install
    (CI installs jsonschema explicitly)."""
    try:
        import jsonschema  # type: ignore
    except ImportError:
        print("[regenerate_community_apps] jsonschema not installed; "
              "skipping schema validation (CI installs it)", file=sys.stderr)
        return
    schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    try:
        jsonschema.validate(payload, schema)
    except jsonschema.ValidationError as exc:
        raise SystemExit(
            f"[regenerate_community_apps] schema validation failed: "
            f"{exc.message} at {list(exc.absolute_path)}")


def build_payload() -> dict:
    assets = []
    assets.extend(discover_builtin_apps())
    assets.extend(discover_builtin_loose_files())
    assets.extend(discover_community_apps())
    assets.extend(load_external_assets())
    merge_legacy_registry(assets)
    validate_unique_ids(assets)
    payload = {
        "schema_version": 2,
        "generator": "regenerate_community_apps.py",
        "assets": assets,
    }
    validate_against_schema(payload)
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check", action="store_true",
        help="Exit non-zero if the on-disk registry differs from regenerated.")
    parser.add_argument(
        "--output", default=str(OUTPUT_PATH),
        help="Output path (default: registry/community_apps.json).")
    args = parser.parse_args()

    payload = build_payload()
    serialized = json.dumps(payload, indent=2) + "\n"

    out_path = Path(args.output)
    if args.check:
        if not out_path.exists():
            print(f"[regenerate_community_apps] {out_path} missing",
                  file=sys.stderr)
            return 1
        existing = out_path.read_text(encoding="utf-8")
        if existing != serialized:
            print(f"[regenerate_community_apps] {out_path} is stale; "
                  f"re-run without --check", file=sys.stderr)
            return 1
        print(f"[regenerate_community_apps] {out_path} OK "
              f"({len(payload['assets'])} assets)")
        return 0

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(serialized, encoding="utf-8")
    print(f"[regenerate_community_apps] wrote {out_path} "
          f"({len(payload['assets'])} assets)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
