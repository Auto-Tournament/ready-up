#!/usr/bin/env bash
set -euo pipefail

# ReadyUp - Discord Webhook Script
# Sends a Discord webhook notification for a ReadyUp release

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
    echo "Usage: ./discord-webhook.sh [X.Y.Z]"
    exit 1
  fi
fi

NEW_VERSION="${NEW_VERSION#v}"

if ! [[ "$NEW_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo -e "${RED}Invalid version format. Use semantic versioning (e.g., 1.0.0)${NC}"
  exit 1
fi

if [ -z "${DISCORD_WEBHOOK_URL:-}" ]; then
  echo -e "${RED}Error: DISCORD_WEBHOOK_URL environment variable is required but not set.${NC}"
  echo -e "${YELLOW}Set it in .env or export it before running:${NC}"
  echo "  export DISCORD_WEBHOOK_URL=\"https://discord.com/api/webhooks/...\""
  exit 1
fi

echo -e "${GREEN}ReadyUp - Discord Webhook${NC}"
echo "========================================="
echo -e "${BLUE}Version:${NC} ${GREEN}${NEW_VERSION}${NC}"
echo ""

get_changelog() {
  local current_tag="v${NEW_VERSION}"
  local prev_tag
  prev_tag=$(git tag --sort=-v:refname | grep -v "^${current_tag}$" | sed -n '1p' 2>/dev/null || echo "")

  local log_range
  if [ -n "$prev_tag" ]; then
    log_range="${prev_tag}..HEAD"
  else
    log_range="HEAD"
  fi

  # Prefer merge commit PR titles; fallback to commit subjects.
  local pr_titles
  pr_titles="$(git log ${log_range} --merges --format="%B" 2>/dev/null | \
    awk '
      /^Merge pull request/ {
        getline
        getline
        if (NF > 0) print "- " $0
      }
    ' | head -20)"

  if [ -n "$pr_titles" ]; then
    if [[ "$OSTYPE" == "darwin"* ]]; then
      echo "$pr_titles" | tail -r
    else
      echo "$pr_titles" | tac
    fi
    return 0
  fi

  git log ${log_range} --format="%s" 2>/dev/null | \
    grep -viE '^Release v[0-9]+\.[0-9]+\.[0-9]+$' | \
    head -20 | \
    awk '{print "- " $0}'
}

CHANGELOG="$(get_changelog)"
if [ -z "$CHANGELOG" ]; then
  CHANGELOG="- Release v${NEW_VERSION}"
fi

# Discord embed limits: keep this safely below 1024.
if [ ${#CHANGELOG} -gt 960 ]; then
  CHANGELOG="${CHANGELOG:0:960}\n- ...and more"
fi

RELEASE_URL="https://github.com/${REPO_OWNER}/${REPO_NAME}/releases/tag/v${NEW_VERSION}"
TIMESTAMP="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

build_payload_with_jq() {
  jq -n \
    --arg content "**New ReadyUp Release: v${NEW_VERSION}**\n${RELEASE_URL}" \
    --arg title "ReadyUp v${NEW_VERSION}" \
    --arg desc "A new version of ReadyUp has been released." \
    --arg changelog "$CHANGELOG" \
    --arg url "$RELEASE_URL" \
    --arg timestamp "$TIMESTAMP" \
    '{
      content: $content,
      embeds: [{
        title: $title,
        description: $desc,
        url: $url,
        fields: [
          { name: "Changelog", value: $changelog, inline: false },
          { name: "GitHub Release", value: ("[View Release](" + $url + ")"), inline: true }
        ],
        footer: { text: "ReadyUp" },
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
  content="$(json_escape "**New ReadyUp Release: v${NEW_VERSION}**\n${RELEASE_URL}")"
  title="$(json_escape "ReadyUp v${NEW_VERSION}")"
  desc="$(json_escape "A new version of ReadyUp has been released.")"
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
      "url": ${url},
      "fields": [
        { "name": "Changelog", "value": ${changelog}, "inline": false },
        { "name": "GitHub Release", "value": "[View Release](${RELEASE_URL})", "inline": true }
      ],
      "footer": { "text": "ReadyUp" },
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

