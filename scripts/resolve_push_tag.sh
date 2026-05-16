#!/usr/bin/env bash
# resolve_push_tag.sh — Decide the release tag for a push-to-main run of
# the release-firmware workflow.
#
# Behavior:
#   - Read firmware/VERSION.
#   - If the tag v<VERSION> does NOT already exist on origin, reuse it
#     (the maintainer bumped VERSION manually for this push).
#   - If it DOES exist, auto-bump the patch component, commit the
#     updated VERSION with `[skip ci]`, push it, and use the new tag.
#
# Writes `tag=` and `semver=` to $GITHUB_OUTPUT so the workflow can
# hand the result to sync_firmware_release.sh (which then force-points
# the tag at the latest main HEAD, builds, and publishes the release).
#
# Idempotent: re-running on the same HEAD with a freshly-bumped VERSION
# is a no-op because the new tag won't exist remotely yet.
#
# Required env:
#   GITHUB_OUTPUT — set by GitHub Actions
# Optional env:
#   DEFAULT_BRANCH (default: main)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_FILE="${ROOT}/firmware/VERSION"
BUMP="${ROOT}/firmware/scripts/bump_version.sh"
DEFAULT_BRANCH="${DEFAULT_BRANCH:-main}"

[[ -f "${VERSION_FILE}" ]] || { echo "missing ${VERSION_FILE}" >&2; exit 1; }
[[ -x "${BUMP}" ]] || { echo "bump_version.sh missing or not executable" >&2; exit 1; }

git -C "$ROOT" config user.name "github-actions[bot]"
git -C "$ROOT" config user.email "41898282+github-actions[bot]@users.noreply.github.com"

git -C "$ROOT" fetch --tags --prune origin
git -C "$ROOT" fetch origin "${DEFAULT_BRANCH}"
git -C "$ROOT" checkout "${DEFAULT_BRANCH}"
git -C "$ROOT" pull --ff-only origin "${DEFAULT_BRANCH}"

CUR="$(tr -d '[:space:]' < "${VERSION_FILE}")"
if [[ ! "${CUR}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "firmware/VERSION '${CUR}' is not semver" >&2
    exit 1
fi

# Tag presence is the source of truth. We check the remote refspec so a
# stale local-only tag doesn't trick us into thinking a release exists.
tag_exists_on_origin() {
    local tag="$1"
    git -C "$ROOT" ls-remote --tags --exit-code origin "refs/tags/${tag}" >/dev/null 2>&1
}

TAG="v${CUR}"
if tag_exists_on_origin "${TAG}"; then
    echo "tag ${TAG} already exists on origin — auto-bumping patch"
    "${BUMP}" patch
    CUR="$(tr -d '[:space:]' < "${VERSION_FILE}")"
    TAG="v${CUR}"
    if tag_exists_on_origin "${TAG}"; then
        # Extremely unlikely (race against a manual tag push), but
        # surface it instead of silently overwriting someone's tag.
        echo "error: auto-bumped to ${TAG}, but that tag also exists on origin" >&2
        exit 1
    fi
    git -C "$ROOT" commit -m "chore: bump firmware to ${TAG} [skip ci]"
    git -C "$ROOT" push origin "HEAD:${DEFAULT_BRANCH}"
    git -C "$ROOT" pull --ff-only origin "${DEFAULT_BRANCH}"
elif tag_exists_on_origin "${CUR}"; then
    # A legacy un-prefixed tag (e.g. "0.2.9") exists but the canonical
    # v-prefixed form ("v0.2.9") does not. CI switched to the vX.X.X
    # convention; treat the v-prefixed tag as the newer canonical form and
    # create it on this run without bumping the version.
    echo "legacy tag ${CUR} (no v-prefix) found on origin — migrating to canonical ${TAG}"
else
    echo "tag ${TAG} not yet on origin — using current firmware/VERSION"
fi

{
    echo "tag=${TAG}"
    echo "semver=${CUR}"
} >> "${GITHUB_OUTPUT}"

echo "Resolved push tag: ${TAG} (semver=${CUR})"
