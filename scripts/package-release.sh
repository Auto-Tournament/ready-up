#!/usr/bin/env bash
# Package a release zip. Its root is the contents of game/csgo, so it can be extracted
# straight into a server's game/csgo:
#
#   readyup/bin/linuxsteamrt64/libserver.so            the shim (sniper build)
#   readyup/bin/linuxsteamrt64/engine-surface.json     gamedata (also embedded in the shim)
#   readyup/bin/linuxsteamrt64/readyup.cfg.example     copied to readyup.cfg if missing
#   readyup/tools/patch_gameinfo.py                    adds Game csgo/readyup to gameinfo.gi
#   readyup/tools/install.sh                           optional safe installer
#   readyup/cfg-templates/ReadyUp/*.cfg                mode cfg templates (cfg exec mode)
#   readyup/VERSION, README.md, INSTALL.md, LICENSE, BUILD_INFO
#
# Nothing in the zip overwrites admin-owned files (readyup.cfg, readyup_db.json, cfg/).
#
#   scripts/package-release.sh <libserver.so> <version> <out-dir>
#   -> <out-dir>/readyup-<version>-linuxsteamrt64.zip (+ .sha256)
set -euo pipefail

SO="${1:?usage: $0 <libserver.so> <version> <out-dir>}"
VERSION="${2:?usage: $0 <libserver.so> <version> <out-dir>}"
OUT="${3:?usage: $0 <libserver.so> <version> <out-dir>}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

[[ -f "$SO" ]] || { echo "missing $SO" >&2; exit 1; }
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
R="$STAGE/readyup"

install -D -m 755 "$SO" "$R/bin/linuxsteamrt64/libserver.so"
install -D -m 644 "$ROOT_DIR/gamedata/engine-surface.json" "$R/bin/linuxsteamrt64/engine-surface.json"
install -D -m 644 "$ROOT_DIR/cfg/readyup.cfg.example" "$R/bin/linuxsteamrt64/readyup.cfg.example"
install -D -m 755 "$ROOT_DIR/scripts/patch_gameinfo.py" "$R/tools/patch_gameinfo.py"
install -D -m 755 "$ROOT_DIR/scripts/install-release.sh" "$R/tools/install.sh"
mkdir -p "$R/cfg-templates/ReadyUp"
install -m 644 "$ROOT_DIR"/cfg/ReadyUp/*.cfg "$R/cfg-templates/ReadyUp/"
printf '%s\n' "$VERSION" >"$R/VERSION"
install -m 644 "$ROOT_DIR/README.md" "$R/README.md"
install -m 644 "$ROOT_DIR/docs/INSTALL.md" "$R/INSTALL.md"
[[ -f "$ROOT_DIR/LICENSE" ]] && install -m 644 "$ROOT_DIR/LICENSE" "$R/LICENSE"

{
  echo "version=$VERSION"
  echo "commit=$(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "built=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "libserver_sha256=$(sha256sum "$SO" | cut -d' ' -f1)"
  [[ -n "${CS2_BUILDID:-}" ]] && echo "verified_cs2_buildid=$CS2_BUILDID"
  [[ -n "${CS2_PATCH_VERSION:-}" ]] && echo "verified_cs2_patch_version=$CS2_PATCH_VERSION"
} >"$R/BUILD_INFO"

# Guard: the dev installer (stops csm servers, wipes DB rows) must never ship.
if grep -q 'csm stop\|TRUNCATE readyup_admins' -r "$R"; then
  echo "package-release: refusing to package the dev install.sh" >&2
  exit 1
fi

ZIP="readyup-${VERSION}-linuxsteamrt64.zip"
rm -f "$OUT/$ZIP"
(cd "$STAGE" && find readyup -exec touch -h -d "@${SOURCE_DATE_EPOCH:-$(date +%s)}" {} + && zip -qrX "$OUT/$ZIP" readyup)
(cd "$OUT" && sha256sum "$ZIP" >"$ZIP.sha256")
echo "Packaged: $OUT/$ZIP"
unzip -l "$OUT/$ZIP"
