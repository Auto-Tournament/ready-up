#!/usr/bin/env bash
# Release notes for a tag (used by .github/workflows/build.yml).
#
#   scripts/ci/release-notes.sh v1.2.3 [cs2-build.env] > notes.md
set -euo pipefail

TAG="${1:?usage: $0 <tag> [cs2-build.env]}"
ENV_FILE="${2:-}"
VERSION="${TAG#v}"

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

Ready Up runs on Linux dedicated servers (\`linuxsteamrt64\`). From the server root (the folder with \`game/\`):

\`\`\`bash
curl -fsSL https://raw.githubusercontent.com/Auto-Tournament/ready-up/master/install.sh | bash
\`\`\`

It shows the components (installed -> $VERSION), installs or updates what you tick, patches \`gameinfo.gi\` (after Metamod if present) and keeps your config. \`bash -s -- essentials\` installs without questions; run it again to update.

| Download | Contents |
|---|---|
| \`ready-up-essentials-$VERSION-linuxsteamrt64.zip\` | core + match. The default. No skins. |
| \`ready-up-full-$VERSION-linuxsteamrt64.zip\` | core + match + skins + hello + gamedata checkers |
| \`ready-up-core\`, \`-match\`, \`-skins\`, \`-hello\` | single components (core alone runs without the match flow) |
| \`SHA256SUMS\` | checksums (the installer verifies them) |

Skins (weapon paints, knives, gloves, agents) can get a server's GSLT banned; only install them on purpose.

Manual install: extract a zip into \`game/csgo\`, then \`python3 readyup/tools/patch_gameinfo.py gameinfo.gi\` (and \`gameinfo_branchspecific.gi\`), and restart. CS2 updates rewrite \`gameinfo.gi\`: run the installer (or the patcher) again after each one.

## Build

Built in the Steam Runtime 3 "sniper" SDK. OpenSSL, libpq, libcurl, libstdc++ and libgcc are linked in, so the core and plugins only need glibc 2.31+ on the host.
EOF
if [[ -n "$BUILDID" ]]; then
  echo
  echo "Engine surface verified against CS2 build \`$BUILDID\`${PATCH_VERSION:+ (PatchVersion $PATCH_VERSION)}."
fi
