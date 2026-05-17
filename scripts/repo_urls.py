"""repo_urls.py — Single source of truth for all GitHub repo-derived URLs.

To migrate to a new GitHub org/repo, change ONLY REPO_SLUG (and optionally
DEFAULT_BRANCH). Every derived URL below recomputes automatically.

Override via environment variables for fork/CI support:
  TEMPORAL_BADGE_REPO_SLUG    — e.g. "temporal-community/badge.temporal.io"
  TEMPORAL_BADGE_REPO_BRANCH  — e.g. "main"

Usage:
    from repo_urls import FIRMWARE_RAW_BASE, COMMUNITY_RAW_BASE, ...
"""

from __future__ import annotations

import os

# ── Identity ──────────────────────────────────────────────────────────────────
# To migrate: comment out the active line, uncomment the target line.
# Also update REPO_OWNER_SLUG in firmware/src/infra/RepoUrls.h to match.

REPO_SLUG = os.environ.get(
    "TEMPORAL_BADGE_REPO_SLUG",
    # Current (pre-migration) repo:
    "Architeuthis-Flux/Temporal-Replay-26-Badge",
    # Temporal-hosted repo (uncomment to switch):
    # "temporal-community/badge.temporal.io",
)
DEFAULT_BRANCH = os.environ.get("TEMPORAL_BADGE_REPO_BRANCH", "main")

# ── Derived URLs (do not edit) ────────────────────────────────────────────────

RAW_BASE = f"https://raw.githubusercontent.com/{REPO_SLUG}/{DEFAULT_BRANCH}"

# Base raw URL for firmware/initial_filesystem/ — used in manifest.json
# and community_apps.json file entry "url" fields.
FIRMWARE_RAW_BASE = f"{RAW_BASE}/firmware/initial_filesystem"

# Base raw URL for community/<app-id>/ PR-contributed apps.
COMMUNITY_RAW_BASE = f"{RAW_BASE}/community"

# Stable download URL prefix for GitHub release assets.
RELEASE_DOWNLOAD_BASE = (
    f"https://github.com/{REPO_SLUG}/releases/latest/download"
)

# Community Apps registry JSON URL — baked into firmware via BadgeConfig.h
# and shown as the default in settings.txt.example.
COMMUNITY_APPS_URL = f"{RAW_BASE}/registry/community_apps.json"
