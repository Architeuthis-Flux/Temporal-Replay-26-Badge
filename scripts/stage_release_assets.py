#!/usr/bin/env python3
"""Stage release-bundled assets and verify them against registry.json.

Reads `release-assets/manifest.json` and, for each entry:

  1. Sources the file from `local_path` if it exists, otherwise
     downloads `fallback_url`. (Either may be set; both being absent
     is an error.)
  2. Computes SHA-256 and size.
  3. If the entry sets `registry_id`, looks up the matching asset in
     `registry/registry.json` and verifies its `sha256` / `size` fields
     match. Mismatches abort with a clear, actionable message.
  4. Stages the file under `artifacts/release/<name>` so the GitHub
     Action can upload it.

Designed to be called both locally (`--check` for verification only,
no network or staging) and from CI (no flags — does the full job).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Pull repo-derived URL constants from the central module so migrating
# the repo requires editing only scripts/repo_urls.py.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from repo_urls import RELEASE_DOWNLOAD_BASE as _RELEASE_DOWNLOAD_BASE  # noqa: E402
MANIFEST_PATH = REPO_ROOT / "release-assets" / "manifest.json"
REGISTRY_PATH = REPO_ROOT / "registry" / "registry.json"
STAGE_DIR = REPO_ROOT / "artifacts" / "release"


class StageError(RuntimeError):
    pass


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url: str, dest: Path) -> None:
    print(f"  downloading {url}")
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "TemporalBadge-OTA/1.0 (release staging)"},
    )
    with urllib.request.urlopen(req, timeout=60) as resp:
        if resp.status != 200:
            raise StageError(f"GET {url} returned HTTP {resp.status}")
        dest.parent.mkdir(parents=True, exist_ok=True)
        with dest.open("wb") as out:
            shutil.copyfileobj(resp, out)


def resolve_source(entry: dict, allow_network: bool) -> Path:
    name = entry["name"]
    local_rel = entry.get("local_path") or ""
    fallback = entry.get("fallback_url") or ""

    if local_rel:
        local = (REPO_ROOT / local_rel).resolve()
        if local.exists():
            print(f"  using local: {local.relative_to(REPO_ROOT)}")
            return local

    if not fallback:
        raise StageError(
            f"asset '{name}': local_path missing and no fallback_url. "
            f"Either commit the file at {local_rel!r} or set fallback_url."
        )

    if not allow_network:
        raise StageError(
            f"asset '{name}': local_path missing and --check forbids "
            f"network. Either provide {local_rel!r} locally, or run "
            f"without --check to allow downloading from {fallback}."
        )

    cache_path = REPO_ROOT / ".asset-cache" / name
    if cache_path.exists():
        print(f"  using cached download: {cache_path.relative_to(REPO_ROOT)}")
        return cache_path
    download(fallback, cache_path)
    return cache_path


def verify_registry(entry: dict, sha256: str, size: int) -> None:
    rid = entry.get("registry_id") or ""
    if not rid:
        return

    if not REGISTRY_PATH.exists():
        raise StageError(
            f"asset '{entry['name']}' references registry_id={rid!r} but "
            f"{REGISTRY_PATH} does not exist."
        )

    with REGISTRY_PATH.open("r", encoding="utf-8") as f:
        registry = json.load(f)

    matching = [a for a in registry.get("assets", []) if a.get("id") == rid]
    if not matching:
        raise StageError(
            f"asset '{entry['name']}' references registry_id={rid!r} but "
            f"no matching entry exists in {REGISTRY_PATH.relative_to(REPO_ROOT)}."
        )
    target = matching[0]

    problems: list[str] = []

    declared_sha = (target.get("sha256") or "").lower()
    if declared_sha and declared_sha != sha256:
        problems.append(
            f"  registry sha256 = {declared_sha}\n"
            f"  actual   sha256 = {sha256}"
        )

    declared_size = target.get("size")
    if declared_size and int(declared_size) != size:
        problems.append(
            f"  registry size = {declared_size}\n"
            f"  actual   size = {size}"
        )

    expected_url = f"{_RELEASE_DOWNLOAD_BASE}/{entry['name']}"
    declared_url = target.get("url") or ""
    if declared_url and declared_url != expected_url:
        problems.append(
            f"  registry url = {declared_url}\n"
            f"  expected url = {expected_url}\n"
            f"  (the staging script assumes assets ship via the stable "
            f"/releases/latest/download/<name> URL pattern.)"
        )

    if problems:
        raise StageError(
            f"asset '{entry['name']}' (registry id {rid}) drifted from "
            f"registry/registry.json:\n" + "\n".join(problems) +
            f"\n\nFix: update registry/registry.json with the new sha256 / "
            f"size / url, or roll the file back to the version the registry "
            f"expects."
        )

    print(f"  verified against registry: {rid}")


def stage(check_only: bool) -> int:
    if not MANIFEST_PATH.exists():
        print(f"[stage] no manifest at {MANIFEST_PATH}, nothing to do")
        return 0

    with MANIFEST_PATH.open("r", encoding="utf-8") as f:
        manifest = json.load(f)

    assets = manifest.get("assets") or []
    if not assets:
        print("[stage] manifest has no assets, nothing to do")
        return 0

    if not check_only:
        if STAGE_DIR.exists():
            shutil.rmtree(STAGE_DIR)
        STAGE_DIR.mkdir(parents=True)

    failures: list[str] = []
    for entry in assets:
        name = entry.get("name") or "?"
        print(f"[stage] {name}")
        try:
            src = resolve_source(entry, allow_network=not check_only)
            sha = sha256_of(src)
            size = src.stat().st_size
            print(f"  size  = {size} bytes ({size / 1024 / 1024:.2f} MB)")
            print(f"  sha256= {sha}")
            verify_registry(entry, sha, size)
            if not check_only:
                dest = STAGE_DIR / name
                shutil.copy2(src, dest)
                print(f"  staged -> {dest.relative_to(REPO_ROOT)}")
        except StageError as exc:
            failures.append(f"[{name}] {exc}")
            print(f"  ERROR: {exc}")

    if failures:
        print()
        print("=" * 64)
        print("Release-asset staging failed:")
        for line in failures:
            print(f"  {line}")
        print("=" * 64)
        return 1

    print()
    if check_only:
        print(f"[stage] OK ({len(assets)} asset(s) verified, no files written)")
    else:
        print(f"[stage] OK ({len(assets)} asset(s) staged into {STAGE_DIR.relative_to(REPO_ROOT)})")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="Verify the manifest against local files + registry without "
             "writing artifacts or downloading anything.",
    )
    args = parser.parse_args()
    try:
        return stage(check_only=args.check)
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
