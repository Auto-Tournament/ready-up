#!/usr/bin/env bash
# Cut a ReadyUp release: bump VERSION, commit, tag vX.Y.Z and push.
#
#   ./release.sh [major|minor|patch|X.Y.Z]      (no argument: release the current VERSION)
#
# The tag push triggers .github/workflows/build.yml, which builds libserver.so in the
# Steam Runtime sniper SDK, verifies it against the current CS2 build, packages
# readyup-X.Y.Z-linuxsteamrt64.zip (extract into game/csgo) and publishes the GitHub
# release (+ Discord announcement if the DISCORD_WEBHOOK_URL secret is set).
# Nothing is built or uploaded from this machine.
#
# Env: WATCH=0 to not follow the CI run after pushing.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

REPO="Auto-Tournament/ready-up"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

die() {
  echo -e "${RED}ERROR:${NC} $*" >&2
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Missing required command: $1"
}

read_version_file() {
  [[ -f "$ROOT_DIR/VERSION" ]] || die "VERSION file not found: $ROOT_DIR/VERSION"
  tr -d ' \t\r\n' <"$ROOT_DIR/VERSION"
}

validate_semver() {
  [[ "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
}

bump_version() {
  local current="$1"
  local bump="$2"
  local major minor patch
  IFS=. read -r major minor patch <<<"$current"
  case "$bump" in
    major) major=$((major + 1)); minor=0; patch=0 ;;
    minor) minor=$((minor + 1)); patch=0 ;;
    patch) patch=$((patch + 1)) ;;
    *) return 1 ;;
  esac
  echo "${major}.${minor}.${patch}"
}

require_cmd git
require_cmd gh

if ! gh auth status >/dev/null 2>&1; then
  die "GitHub CLI is not authenticated. Run: gh auth login"
fi

CURRENT_VERSION="$(read_version_file)"
validate_semver "$CURRENT_VERSION" || die "Invalid VERSION file SemVer: '$CURRENT_VERSION' (expected X.Y.Z)"

ARG="${1:-none}"
NEW_VERSION="$CURRENT_VERSION"
MODE="none"

if [[ "$ARG" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  NEW_VERSION="$ARG"
  MODE="explicit"
elif [[ "$ARG" == "major" || "$ARG" == "minor" || "$ARG" == "patch" ]]; then
  NEW_VERSION="$(bump_version "$CURRENT_VERSION" "$ARG")"
  MODE="$ARG"
elif [[ "$ARG" != "none" ]]; then
  die "Invalid argument: '$ARG'. Usage: ./release.sh [major|minor|patch|X.Y.Z]"
fi

validate_semver "$NEW_VERSION" || die "Invalid new SemVer: '$NEW_VERSION'"

if ! git diff --quiet || ! git diff --cached --quiet; then
  die "Working tree is not clean. Commit or stash changes before releasing."
fi

TAG="v${NEW_VERSION}"
if git rev-parse "$TAG" >/dev/null 2>&1; then
  die "Tag ${TAG} already exists."
fi
if git ls-remote --tags origin "refs/tags/${TAG}" | grep -q "${TAG}$" 2>/dev/null; then
  die "Remote tag ${TAG} already exists on origin."
fi

echo -e "${GREEN}ReadyUp release${NC}"
echo "========================================="
echo -e "${BLUE}Repository:${NC}      ${REPO}"
echo -e "${BLUE}Current version:${NC} ${CURRENT_VERSION}"
echo -e "${BLUE}New version:${NC}     ${NEW_VERSION} (${MODE})"
echo -e "${BLUE}Git branch:${NC}      $(git branch --show-current 2>/dev/null || echo unknown)"
echo ""
echo "CI builds, verifies and publishes the release after the tag is pushed."
echo ""

read -rp "$(echo -e "${YELLOW}Continue with release ${TAG}? [y/N]: ${NC}")" CONFIRM
case "$CONFIRM" in
  y|Y|yes|YES) ;;
  *) echo "Aborted."; exit 0 ;;
esac

if [[ "$NEW_VERSION" != "$CURRENT_VERSION" ]]; then
  echo -e "${BLUE}Updating VERSION...${NC}"
  printf '%s\n' "$NEW_VERSION" >"$ROOT_DIR/VERSION"
  git add VERSION
  git commit -m "Release ${TAG}"
fi
git tag "$TAG"

echo -e "${BLUE}Pushing branch + tag...${NC}"
git push -u origin HEAD
git push origin "$TAG"

RELEASE_URL="https://github.com/${REPO}/releases/tag/${TAG}"
echo -e "${GREEN}Tag pushed.${NC} The Build workflow publishes ${RELEASE_URL}"

if [[ "${WATCH:-1}" == "1" ]]; then
  echo -e "${BLUE}Waiting for the Build run for ${TAG}...${NC}"
  run_id=""
  for _ in $(seq 1 30); do
    run_id="$(gh run list -R "$REPO" --workflow build.yml --branch "$TAG" --limit 1 --json databaseId --jq '.[0].databaseId // empty' 2>/dev/null || true)"
    [[ -n "$run_id" ]] && break
    sleep 5
  done
  if [[ -n "$run_id" ]]; then
    gh run watch -R "$REPO" "$run_id" --exit-status && echo -e "${GREEN}Released:${NC} ${RELEASE_URL}" \
      || die "Build/release run failed: https://github.com/${REPO}/actions/runs/${run_id}"
  else
    echo -e "${YELLOW}Could not find the run yet; check https://github.com/${REPO}/actions${NC}"
  fi
fi
