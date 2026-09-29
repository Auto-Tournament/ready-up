#!/usr/bin/env bash
set -euo pipefail

# Ready Up - Discord Webhook Script
# Sends a Discord webhook notification for a Ready Up release.
# Pre-releases (X.Y.Z-beta.N / X.Y.Z-rc.N) get their own title, an amber embed and the beta install line.
#
# DISCORD_DRY_RUN=1 prints the payload instead of sending it (no webhook URL needed).

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="${SCRIPT_DIR}"
cd "${PROJECT_ROOT}"

# Source .env file if it exists (from project root)
if [ -f "${PROJECT_ROOT}/.env" ]; then
  echo -e "${BLUE}Sourcing .env file...${NC}"
  set -a
  # shellcheck disable=SC1091
  source "${PROJECT_ROOT}/.env"
  set +a
fi

REPO_OWNER="Auto-Tournament"
REPO_NAME="ready-up"

read_version_file() {
  if [ -f "${PROJECT_ROOT}/VERSION" ]; then
    tr -d ' \t\r\n' <"${PROJECT_ROOT}/VERSION"
  else
    echo ""
  fi
}

NEW_VERSION="${1:-}"
if [ -z "$NEW_VERSION" ]; then
  NEW_VERSION="$(read_version_file)"
  if [ -z "$NEW_VERSION" ]; then
    echo -e "${RED}Error: VERSION file not found and no version provided${NC}"
    echo "Usage: ./discord-webhook.sh [X.Y.Z[-beta.N]]"
    exit 1
  fi
fi

NEW_VERSION="${NEW_VERSION#v}"

if ! [[ "$NEW_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]]; then
  echo -e "${RED}Invalid version format. Use semantic versioning (e.g., 1.0.0 or 1.0.0-beta.1)${NC}"
  exit 1
fi

if [ -z "${DISCORD_WEBHOOK_URL:-}" ] && [ "${DISCORD_DRY_RUN:-}" != "1" ]; then
  echo -e "${RED}Error: DISCORD_WEBHOOK_URL environment variable is required but not set.${NC}"
  echo -e "${YELLOW}Set it in .env or export it before running:${NC}"
  echo "  export DISCORD_WEBHOOK_URL=\"https://discord.com/api/webhooks/...\""
  exit 1
fi

# A suffix (-beta.N, -rc.N, ...) marks a pre-release, same rule the GitHub release step uses.
TITLE_SUFFIX=""
EMBED_COLOR=5793266 # blurple
DESC="A new version of Ready Up has been released."
CONTENT_LABEL="Release"
if [[ "$NEW_VERSION" == *-* ]]; then
  TITLE_SUFFIX=" (beta)"
  if [[ "$NEW_VERSION" == *-rc* ]]; then
    TITLE_SUFFIX=" (release candidate)"
  fi
  EMBED_COLOR=16096779 # amber, 0xF59E0B
  CONTENT_LABEL="Pre-release"
  DESC="A new Ready Up pre-release is out, for testing. Stable installs are not affected."
  DESC+=$'\n'"Newest pre-release: \`install.sh --channel beta\`"
  DESC+=$'\n'"This one: \`install.sh --version v${NEW_VERSION}\`"
fi

echo -e "${GREEN}Ready Up - Discord Webhook${NC}"
echo "========================================="
echo -e "${BLUE}Version:${NC} ${GREEN}${NEW_VERSION}${NC}"
echo ""

# Commit subjects since the previous tag, newest first. The repo squash-merges, so each PR is one
# "Title (#123)" subject, which becomes a PR link. Merge commits and release bumps are skipped.
# With no previous tag the range is capped instead of being the whole history.
get_changelog() {
  local current_tag="v${NEW_VERSION}"
  local prev_tag
  prev_tag="$(git tag --sort=-v:refname | grep -vxF "$current_tag" | sed -n '1p' || true)"

  # Stop at the release tag when it exists (HEAD may be past it), otherwise at HEAD.
  local end="HEAD"
  if git rev-parse -q --verify "refs/tags/${current_tag}" >/dev/null 2>&1; then
    end="$current_tag"
  fi

  local -a range=(-n 100 "$end")
  if [ -n "$prev_tag" ]; then
    range=("${prev_tag}..${end}")
  fi

  git log "${range[@]}" --no-merges --format="%s" 2>/dev/null |
    { grep -viE '^Release v?[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$' || true; } |
    head -15 |
    sed -E "s#[[:space:]]\(\#([0-9]+)\)\$# ([\#\1](https://github.com/${REPO_OWNER}/${REPO_NAME}/pull/\1))#" |
    awk '{print "- " $0}'
}

CHANGELOG="$(get_changelog)"
if [ -z "$CHANGELOG" ]; then
  CHANGELOG="- Release v${NEW_VERSION}"
fi

# Discord embed limits: keep this safely below 1024. Cut at a line boundary so a link is never half-cut.
if [ ${#CHANGELOG} -gt 960 ]; then
  CHANGELOG="${CHANGELOG:0:960}"
  CHANGELOG="${CHANGELOG%$'\n'*}"$'\n'"- ...and more"
fi

RELEASE_URL="https://github.com/${REPO_OWNER}/${REPO_NAME}/releases/tag/v${NEW_VERSION}"
TIMESTAMP="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
TITLE="Ready Up v${NEW_VERSION}${TITLE_SUFFIX}"
CONTENT="**New Ready Up ${CONTENT_LABEL}: v${NEW_VERSION}**"$'\n'"${RELEASE_URL}"

build_payload_with_jq() {
  jq -n \
    --arg content "$CONTENT" \
    --arg title "$TITLE" \
    --arg desc "$DESC" \
    --argjson color "$EMBED_COLOR" \
    --arg changelog "$CHANGELOG" \
    --arg url "$RELEASE_URL" \
    --arg timestamp "$TIMESTAMP" \
    '{
      content: $content,
      embeds: [{
        title: $title,
        description: $desc,
        color: $color,
        url: $url,
        fields: [
          { name: "Changelog", value: $changelog, inline: false },
          { name: "GitHub Release", value: ("[View Release](" + $url + ")"), inline: true }
        ],
        footer: { text: "Ready Up" },
        timestamp: $timestamp
      }]
    }'
}

json_escape() {
  python3 - <<'PY' "$1"
import json, sys
print(json.dumps(sys.argv[1]))
PY
}

build_payload_fallback() {
  local content title desc changelog url timestamp
  content="$(json_escape "$CONTENT")"
  title="$(json_escape "$TITLE")"
  desc="$(json_escape "$DESC")"
  changelog="$(json_escape "${CHANGELOG}")"
  url="$(json_escape "${RELEASE_URL}")"
  timestamp="$(json_escape "${TIMESTAMP}")"

  cat <<EOF
{
  "content": ${content},
  "embeds": [
    {
      "title": ${title},
      "description": ${desc},
      "color": ${EMBED_COLOR},
      "url": ${url},
      "fields": [
        { "name": "Changelog", "value": ${changelog}, "inline": false },
        { "name": "GitHub Release", "value": "[View Release](${RELEASE_URL})", "inline": true }
      ],
      "footer": { "text": "Ready Up" },
      "timestamp": ${timestamp}
    }
  ]
}
EOF
}

PAYLOAD=""
if command -v jq >/dev/null 2>&1; then
  PAYLOAD="$(build_payload_with_jq)"
else
  PAYLOAD="$(build_payload_fallback)"
fi

if [ "${DISCORD_DRY_RUN:-}" = "1" ]; then
  echo -e "${YELLOW}DISCORD_DRY_RUN=1: payload not sent${NC}"
  echo "$PAYLOAD"
  exit 0
fi

echo -e "${BLUE}Sending Discord webhook...${NC}"
HTTP_CODE="$(curl -sS -o /tmp/readyup_discord_webhook.out -w "%{http_code}" \
  -H "Content-Type: application/json" \
  -X POST \
  -d "$PAYLOAD" \
  "$DISCORD_WEBHOOK_URL" || true)"

if [[ "$HTTP_CODE" =~ ^2 ]]; then
  echo -e "${GREEN}✓ Discord webhook sent successfully${NC}"
  exit 0
fi

echo -e "${RED}✗ Discord webhook failed (HTTP ${HTTP_CODE})${NC}"
echo -e "${YELLOW}Response:${NC}"
cat /tmp/readyup_discord_webhook.out || true
exit 1
