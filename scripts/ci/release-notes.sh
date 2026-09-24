#!/usr/bin/env bash
# Release notes for a tag (used by .github/workflows/build.yml).
#
#   scripts/ci/release-notes.sh v1.2.3 [cs2-build.env] > notes.md
set -euo pipefail

TAG="${1:?usage: $0 <tag> [cs2-build.env]}"
ENV_FILE="${2:-}"
VERSION="${TAG#v}"
ZIP="readyup-${VERSION}-linuxsteamrt64.zip"

prev="$(git tag --sort=-v:refname --merged "$TAG" 2>/dev/null | grep -E '^v[0-9]' | grep -vx "$TAG" | head -n1 || true)"
range="${prev:+$prev..}$TAG"
changes="$(git log "$range" --no-merges --pretty='- %s' 2>/dev/null | grep -viE '^- Release v[0-9]+\.[0-9]+\.[0-9]+$' | head -n 100 || true)"
[[ -n "$changes" ]] || changes="- Release $TAG"

BUILDID="" PATCH_VERSION=""
# shellcheck disable=SC1090
[[ -n "$ENV_FILE" && -f "$ENV_FILE" ]] && source "$ENV_FILE"

cat <<EOF
## Changes${prev:+ since $prev}

$changes

## Install

ReadyUp runs on Linux dedicated servers (\`linuxsteamrt64\`).

1. Download \`$ZIP\`.
2. Extract it into \`game/csgo\`. You should end up with \`game/csgo/readyup/bin/linuxsteamrt64/libserver.so\`.
3. Add ReadyUp to \`gameinfo.gi\` (and \`gameinfo_branchspecific.gi\` if you have it):

   \`\`\`bash
   cd game/csgo
   python3 readyup/tools/patch_gameinfo.py gameinfo.gi --game csgo/readyup
   \`\`\`

   If Metamod is installed, ReadyUp goes directly below its \`Game csgo/addons/metamod\` line; otherwise directly above \`Game csgo\`.
4. Restart the server.

\`readyup/tools/install.sh\` does steps 2-3 for you (\`--dry-run\` to preview). It never stops servers or touches databases. CS2 updates rewrite \`gameinfo.gi\`, so run the patcher again after each one.

## Build

Built in the Steam Runtime 3 "sniper" SDK. OpenSSL, libpq, libcurl, libstdc++ and libgcc are linked in, so the shim only needs glibc 2.31+ on the host.
EOF
if [[ -n "$BUILDID" ]]; then
  echo
  echo "Engine surface verified against CS2 build \`$BUILDID\`${PATCH_VERSION:+ (PatchVersion $PATCH_VERSION)}."
fi
