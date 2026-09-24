#!/usr/bin/env bash
# Ready Up release installer (shipped in the release zip as readyup/tools/install.sh).
#
# Optional helper: the release zip can also be installed by hand (extract into game/csgo,
# run readyup/tools/patch_gameinfo.py, restart). This script does the same steps safely:
#
#   readyup/tools/install.sh [--dry-run] [TARGET...]
#
# TARGET is a CS2 root (contains game/csgo) or a game/csgo directory. Without a TARGET,
# it installs into the game/csgo directory the zip was extracted into.
#
# What it does, per target:
#   - installs libserver.so + engine-surface.json into game/csgo/readyup/bin/linuxsteamrt64/
#     via copy-then-rename, so a running server keeps its mapped copy (old one kept as
#     libserver.so.prev)
#   - creates readyup.cfg from readyup.cfg.example ONLY if it does not exist
#   - copies cfg templates to game/csgo/cfg/ReadyUp/ ONLY where missing
#   - patches gameinfo.gi and gameinfo_branchspecific.gi (Metamod first if present, then
#     Ready Up, then `Game csgo`); a backup is written whenever a file changes
#
# What it never does: stop/start/attach servers, touch databases or readyup_db.json,
# edit server.cfg, build anything, or use sudo. Restart the server yourself afterwards.
set -euo pipefail

PKG="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # the extracted readyup/ folder
GAME_PATH="csgo/readyup"
DRY_RUN=0
TARGETS=()

usage() { sed -n '2,24p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

for arg in "$@"; do
  case "$arg" in
    --dry-run|-n) DRY_RUN=1 ;;
    -h|--help) usage; exit 0 ;;
    -*) echo "unknown option: $arg" >&2; exit 2 ;;
    *) TARGETS+=("$arg") ;;
  esac
done

SRC_SO="$PKG/bin/linuxsteamrt64/libserver.so"
SRC_SURFACE="$PKG/bin/linuxsteamrt64/engine-surface.json"
PATCHER="$PKG/tools/patch_gameinfo.py"
for f in "$SRC_SO" "$SRC_SURFACE" "$PATCHER"; do
  [[ -f "$f" ]] || { echo "ERROR: $f is missing; run this from the extracted release zip." >&2; exit 1; }
done
command -v python3 >/dev/null 2>&1 || { echo "ERROR: python3 is required for patch_gameinfo.py" >&2; exit 1; }

if [[ ${#TARGETS[@]} -eq 0 ]]; then
  parent="$(dirname "$PKG")"
  if [[ -f "$parent/gameinfo.gi" ]]; then
    TARGETS=("$parent")
  else
    echo "ERROR: no target given and $parent is not a game/csgo directory." >&2
    echo "Usage: $0 [--dry-run] /path/to/cs2   (or /path/to/cs2/game/csgo)" >&2
    exit 2
  fi
fi

run() {
  if [[ "$DRY_RUN" == 1 ]]; then
    echo "  [dry-run] $*"
  else
    "$@"
  fi
}

same_file() { [[ -e "$1" && -e "$2" && "$(realpath "$1")" == "$(realpath "$2")" ]]; }

# Atomic replace: write next to the destination, then rename over it. A running server
# keeps the old inode mapped instead of seeing a half-written or truncated library.
install_atomic() {
  local src="$1" dst="$2" mode="$3"
  if same_file "$src" "$dst"; then
    echo "  = $dst (already in place)"
    return
  fi
  if [[ -f "$dst" ]] && cmp -s "$src" "$dst"; then
    echo "  = $dst (unchanged)"
    return
  fi
  if [[ "$(basename "$dst")" == "libserver.so" && -f "$dst" ]]; then
    run cp -p "$dst" "$dst.prev"
  fi
  run cp "$src" "$dst.readyup-new"
  run chmod "$mode" "$dst.readyup-new"
  run mv -f "$dst.readyup-new" "$dst"
  echo "  + $dst"
}

install_one() {
  local target="$1" csgo
  if [[ -f "$target/game/csgo/gameinfo.gi" ]]; then
    csgo="$(cd "$target/game/csgo" && pwd)"
  elif [[ -f "$target/gameinfo.gi" ]]; then
    csgo="$(cd "$target" && pwd)"
  else
    echo "ERROR: $target is neither a CS2 root nor a game/csgo directory (no gameinfo.gi)." >&2
    return 1
  fi
  if [[ ! -w "$csgo" ]]; then
    echo "ERROR: $csgo is not writable by $(id -un). Run as the server's user." >&2
    return 1
  fi
  local bin="$csgo/readyup/bin/linuxsteamrt64"
  echo "Installing Ready Up $(cat "$PKG/VERSION" 2>/dev/null || echo '?') into $csgo"

  run mkdir -p "$bin" "$csgo/readyup/tools"
  install_atomic "$SRC_SO" "$bin/libserver.so" 755
  install_atomic "$SRC_SURFACE" "$bin/engine-surface.json" 644
  if ! same_file "$PKG" "$csgo/readyup"; then
    install_atomic "$PATCHER" "$csgo/readyup/tools/patch_gameinfo.py" 755
    install_atomic "${BASH_SOURCE[0]}" "$csgo/readyup/tools/install.sh" 755
    [[ -f "$PKG/bin/linuxsteamrt64/readyup.cfg.example" ]] && \
      install_atomic "$PKG/bin/linuxsteamrt64/readyup.cfg.example" "$bin/readyup.cfg.example" 644
    [[ -f "$PKG/VERSION" ]] && install_atomic "$PKG/VERSION" "$csgo/readyup/VERSION" 644
  fi

  # Runtime config: create once, never overwrite admin edits.
  if [[ -f "$bin/readyup.cfg" ]]; then
    echo "  = $bin/readyup.cfg (kept)"
  elif [[ -f "$PKG/bin/linuxsteamrt64/readyup.cfg.example" ]]; then
    run cp "$PKG/bin/linuxsteamrt64/readyup.cfg.example" "$bin/readyup.cfg"
    echo "  + $bin/readyup.cfg (from example)"
  fi
  if [[ ! -f "$bin/readyup_db.json" ]]; then
    echo "  note: no readyup_db.json; admins/persistence stay off until you add one (docs/INSTALL.md)."
  fi

  # Mode cfg templates (only used when cfg exec is enabled): copy missing files only.
  if [[ -d "$PKG/cfg-templates/ReadyUp" ]]; then
    run mkdir -p "$csgo/cfg/ReadyUp"
    local f base
    for f in "$PKG"/cfg-templates/ReadyUp/*.cfg; do
      [[ -f "$f" ]] || continue
      base="$(basename "$f")"
      if [[ ! -f "$csgo/cfg/ReadyUp/$base" ]]; then
        run cp "$f" "$csgo/cfg/ReadyUp/$base"
        echo "  + $csgo/cfg/ReadyUp/$base"
      fi
    done
  fi

  local gi rc
  for gi in "$csgo/gameinfo.gi" "$csgo/gameinfo_branchspecific.gi"; do
    [[ -f "$gi" ]] || continue
    if [[ "$DRY_RUN" == 1 ]]; then
      set +e; python3 "$PATCHER" "$gi" --game "$GAME_PATH" --check >/dev/null; rc=$?; set -e
      case "$rc" in
        0) echo "  = $gi (search path already correct)" ;;
        3) echo "  [dry-run] would patch $gi" ;;
        *) echo "  ! $gi could not be patched" ;;
      esac
    else
      python3 "$PATCHER" "$gi" --game "$GAME_PATH" | sed 's/^/  /'
    fi
  done

  if command -v pgrep >/dev/null 2>&1 && pgrep -f "$(dirname "$(dirname "$csgo")")/game/bin/linuxsteamrt64/cs2" >/dev/null 2>&1; then
    echo "  A CS2 server is running from this install: restart it to load Ready Up."
  fi
}

rc=0
for t in "${TARGETS[@]}"; do
  install_one "$t" || rc=1
done
[[ "$DRY_RUN" == 1 ]] && echo "Dry run: nothing was changed."
[[ $rc -eq 0 ]] && echo "Done. Restart the server(s) to load Ready Up. CS2 updates rewrite gameinfo.gi: re-run this (or patch_gameinfo.py) after each update."
exit $rc
