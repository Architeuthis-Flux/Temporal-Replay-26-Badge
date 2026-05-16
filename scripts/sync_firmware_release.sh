#!/usr/bin/env bash
# sync_firmware_release.sh — Align firmware/VERSION with a release tag and
# point the tag at the current default-branch HEAD.
#
# Used by .github/workflows/release-firmware.yml so every release build
# (including workflow_dispatch re-runs) ships a binary whose FIRMWARE_VERSION
# matches the GitHub release tag, built from the latest main.
#
# Usage:
#   ./scripts/sync_firmware_release.sh <tag>   # e.g. v0.2.9 or 0.2.9
#
# Requires: git, firmware/scripts/bump_version.sh, push access to origin.

set -euo pipefail

usage() {
    sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

[[ $# -eq 1 ]] || usage 1
[[ "$1" == "-h" || "$1" == "--help" ]] && usage 0

TAG="$1"
SEMVER="${TAG#v}"
if [[ ! "$SEMVER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "error: release tag must be semver (optional leading v): ${TAG}" >&2
    exit 1
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_FILE="${ROOT}/firmware/VERSION"
DEFAULT_BRANCH="${DEFAULT_BRANCH:-main}"

git -C "$ROOT" config user.name "github-actions[bot]"
git -C "$ROOT" config user.email "41898282+github-actions[bot]@users.noreply.github.com"

git -C "$ROOT" fetch origin "${DEFAULT_BRANCH}"
git -C "$ROOT" checkout "${DEFAULT_BRANCH}"
git -C "$ROOT" pull --ff-only origin "${DEFAULT_BRANCH}"

CUR="$(tr -d '[:space:]' < "${VERSION_FILE}" 2>/dev/null || echo "0.0.0")"
if [[ "${CUR}" != "${SEMVER}" ]]; then
    echo "firmware/VERSION ${CUR} != release ${SEMVER} — bumping"
    "${ROOT}/firmware/scripts/bump_version.sh" "${SEMVER}"
    git -C "$ROOT" commit -m "chore: bump firmware to v${SEMVER} [skip ci]"
    git -C "$ROOT" push origin "HEAD:${DEFAULT_BRANCH}"
    git -C "$ROOT" pull --ff-only origin "${DEFAULT_BRANCH}"
else
    echo "firmware/VERSION already ${SEMVER}"
fi

echo "Moving tag ${TAG} -> $(git -C "$ROOT" rev-parse --short HEAD) on ${DEFAULT_BRANCH}"
git -C "$ROOT" tag -fa "${TAG}" -m "${TAG}"
git -C "$ROOT" push -f origin "${TAG}"

if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    {
        echo "tag=${TAG}"
        echo "semver=${SEMVER}"
    } >> "${GITHUB_OUTPUT}"
fi

echo "Release sync complete: tag=${TAG} semver=${SEMVER} commit=$(git -C "$ROOT" rev-parse HEAD)"
