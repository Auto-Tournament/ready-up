#!/usr/bin/env bash
# Download ONLY the CS2 dedicated-server files Ready Up's checks need, anonymously,
# with DepotDownloader (never a full app_update, ~18 MB instead of ~60 GB):
#
#   depot 2347773 (CS2 Linux binaries):
#     game/csgo/bin/linuxsteamrt64/libserver.so
#     game/bin/linuxsteamrt64/libengine2.so
#     game/bin/linuxsteamrt64/libtier0.so
#     game/bin/linuxsteamrt64/libschemasystem.so
#   depot 2347770 (CS2 common content):
#     game/csgo/steam.inf              (PatchVersion / ServerVersion)
#
#   scripts/ci/fetch-cs2-binaries.sh <out-dir>
#
# Writes <out-dir>/cs2-build.env with BUILDID, PATCH_VERSION, SERVER_VERSION,
# SOURCE_REVISION, LIBSERVER_SHA256. The buildid is read from api.steamcmd.net before
# and after the download; if CS2 updated in between, the download is repeated.
#
# Env:
#   DD_DIR        where DepotDownloader is cached (default ~/.cache/depotdownloader)
#   DD_VERSION    DepotDownloader release (pinned + sha256-checked)
set -euo pipefail

OUT="${1:?usage: $0 <out-dir>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP=730
BIN_DEPOT=2347773
COMMON_DEPOT=2347770

DD_VERSION="${DD_VERSION:-3.4.0}"
DD_SHA256="a999dec66b4850fc961bd50366696d23c2d0fad7b18790e6a5647b2f19097a53"  # DepotDownloader-linux-x64.zip 3.4.0
DD_DIR="${DD_DIR:-$HOME/.cache/depotdownloader}"
DD="$DD_DIR/$DD_VERSION/DepotDownloader"

FILES=(
  game/csgo/bin/linuxsteamrt64/libserver.so
  game/bin/linuxsteamrt64/libengine2.so
  game/bin/linuxsteamrt64/libtier0.so
  game/bin/linuxsteamrt64/libschemasystem.so
  game/csgo/steam.inf
)

if [[ ! -x "$DD" ]]; then
  mkdir -p "$DD_DIR/$DD_VERSION"
  zip="$DD_DIR/DepotDownloader-$DD_VERSION-linux-x64.zip"
  curl -fsSL --retry 3 -o "$zip" \
    "https://github.com/SteamRE/DepotDownloader/releases/download/DepotDownloader_${DD_VERSION}/DepotDownloader-linux-x64.zip"
  if [[ "$DD_VERSION" == "3.4.0" ]]; then
    echo "$DD_SHA256  $zip" | sha256sum -c --quiet -
  fi
  python3 -m zipfile -e "$zip" "$DD_DIR/$DD_VERSION"
  chmod +x "$DD"
fi

# .NET needs ICU unless told otherwise; DepotDownloader does not care about cultures.
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

mkdir -p "$OUT"
filelist="$(mktemp)"
trap 'rm -f "$filelist"' EXIT
printf '%s\n' "${FILES[@]}" >"$filelist"

download() {
  local depot="$1" log
  log="$(mktemp)"
  if ! "$DD" -app "$APP" -depot "$depot" -branch public -filelist "$filelist" \
       -dir "$OUT" -max-downloads 8 >"$log" 2>&1; then
    cat "$log" >&2
    rm -f "$log"
    return 1
  fi
  grep -E '^(Got manifest|Manifest|Depot [0-9]+ - Downloaded)' "$log" || true
  rm -f "$log"
}

for _ in 1 2; do
  before="$("$HERE/cs2-buildid.sh")"
  echo "CS2 public buildid: $before (fetching depots $BIN_DEPOT + $COMMON_DEPOT)"
  download "$BIN_DEPOT"
  download "$COMMON_DEPOT"
  after="$("$HERE/cs2-buildid.sh")"
  [[ "$before" == "$after" ]] && break
  echo "CS2 updated during download ($before -> $after); fetching again"
done

for f in "${FILES[@]}"; do
  [[ -s "$OUT/$f" ]] || { echo "missing after download: $f" >&2; exit 1; }
done

inf="$OUT/game/csgo/steam.inf"
inf_get() { sed -n "s/^$1=//p" "$inf" | tr -d '\r' | head -n1; }
{
  echo "BUILDID=$after"
  echo "PATCH_VERSION=$(inf_get PatchVersion)"
  echo "SERVER_VERSION=$(inf_get ServerVersion)"
  echo "SOURCE_REVISION=$(inf_get SourceRevision)"
  echo "LIBSERVER_SHA256=$(sha256sum "$OUT/game/csgo/bin/linuxsteamrt64/libserver.so" | cut -d' ' -f1)"
} >"$OUT/cs2-build.env"
cat "$OUT/cs2-build.env"
