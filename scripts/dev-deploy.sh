#!/usr/bin/env bash
# Build ReadyUp in Docker and push libserver.so to a CS2 test server.
#
# Run as `sivert` on the cs2 box. The server files are owned by another user
# (cs2servermanager), so the artifact is streamed as a tarball over ssh and
# swapped in on the far side.
#
# Usage:
#   scripts/dev-deploy.sh [--target DIR] [--ssh USER@HOST] [--session NAME]
#                         [--no-build] [--restart] [--timeout SEC]
#   scripts/dev-deploy.sh --plugin NAME [--target DIR] [--ssh ...] [--session ...] [--no-build]
#
#   --target DIR    server root (contains game/ and run.sh)
#                   default: /home/cs2servermanager/readyup-test   (env RU_TARGET)
#   --ssh DEST      ssh destination owning the server
#                   default: cs2servermanager@localhost        (env RU_SSH)
#   --session NAME  tmux session running the server, default ru-test (env RU_SESSION)
#   --no-build      deploy the existing build-docker/libserver.so
#   --restart       kill + relaunch the tmux session, wait for the server to
#                   come up (or die) and print the ReadyUp log lines
#   --timeout SEC   how long --restart waits, default 180
#   --plugin NAME   hot-reload one plugin instead of deploying the core: build only
#                   target readyup_plugin_NAME, copy build-docker/plugins/NAME.so to
#                   csgo/readyup/plugins/NAME.so, then type `ru plugin reload NAME`
#                   into the server console (tmux) and print the result. No restart.
#
# The previous file is kept as <file>.prev (libserver.so.prev / NAME.so.prev). The new file is
# renamed into place (never written over), so a running server keeps its
# mapped copy intact until it restarts.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${RU_TARGET:-/home/cs2servermanager/readyup-test}"
SSH_DEST="${RU_SSH:-cs2servermanager@localhost}"
SESSION="${RU_SESSION:-ru-test}"
BUILD=1
RESTART=0
TIMEOUT=180
PLUGIN=""

die() { echo "dev-deploy: $*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target)   TARGET="${2:?}"; shift 2 ;;
    --ssh)      SSH_DEST="${2:?}"; shift 2 ;;
    --session)  SESSION="${2:?}"; shift 2 ;;
    --no-build) BUILD=0; shift ;;
    --restart)  RESTART=1; shift ;;
    --timeout)  TIMEOUT="${2:?}"; shift 2 ;;
    --plugin)   PLUGIN="${2:?}"; shift 2 ;;
    -h|--help)  sed -n '2,30p' "$0"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

if [[ -n "$PLUGIN" ]]; then
  [[ "$PLUGIN" =~ ^[a-z0-9_-]{1,32}$ ]] || die "invalid plugin name: $PLUGIN"
  [[ $RESTART -eq 0 ]] || die "--plugin hot-reloads; it does not combine with --restart"
fi

TARGET="${TARGET%/}"
case "$(basename "$TARGET")" in
  server-1|server-2|server-3|master-install)
    die "refusing to deploy to $TARGET (MatchZy-managed server)" ;;
esac
[[ "$TARGET" == /* ]] || die "--target must be an absolute path"

SSH=(ssh -o BatchMode=yes "$SSH_DEST")
"${SSH[@]}" true 2>/dev/null \
  || die "cannot ssh to $SSH_DEST with BatchMode (add this user's key to its authorized_keys)"

if [[ $BUILD -eq 1 ]]; then
  if [[ -n "$PLUGIN" ]]; then
    BUILD_TARGET="readyup_plugin_$PLUGIN" "$ROOT_DIR/scripts/docker-build.sh"
  else
    "$ROOT_DIR/scripts/docker-build.sh"
  fi
fi
ART_DIR="$ROOT_DIR/${BUILD_DIR:-build-docker}"
if [[ -n "$PLUGIN" ]]; then
  SRC_DIR="$ART_DIR/plugins"
  FILE="$PLUGIN.so"
  DEST="$TARGET/game/csgo/readyup/plugins"
else
  SRC_DIR="$ART_DIR"
  FILE="libserver.so"
  DEST="$TARGET/game/csgo/readyup/bin/linuxsteamrt64"
fi
[[ -f "$SRC_DIR/$FILE" ]] || die "no artifact at $SRC_DIR/$FILE"

echo "Deploying $FILE $(sha256sum "$SRC_DIR/$FILE" | cut -c1-12) -> $SSH_DEST:$DEST"

# The remote script goes on the command line so stdin is free for the tarball.
read -r -d '' DEPLOY_SCRIPT <<'REMOTE' || true
set -euo pipefail
dest="$1"
file="$2"
mkdir -p "$dest"
stage="$(mktemp -d "$dest/.deploy.XXXXXX")"
trap 'rm -rf "$stage"' EXIT
tar -xf - -C "$stage"
chmod 755 "$stage/$file"
if [[ -f "$dest/$file" ]]; then cp -p "$dest/$file" "$dest/$file.prev"; fi
mv -f "$stage/$file" "$dest/$file"
ls -l "$dest/$file"*
REMOTE
tar -C "$SRC_DIR" -cf - "$FILE" \
  | "${SSH[@]}" "bash -c $(printf '%q' "$DEPLOY_SCRIPT") deploy $(printf '%q' "$DEST") $(printf '%q' "$FILE")"

if [[ -n "$PLUGIN" ]]; then
  # The core runs the reload at the top of its next GameFrame. Keep
  # sv_hibernate_when_empty 0 on dev servers so an empty server still ticks.
  echo "Reloading plugin $PLUGIN in tmux session $SESSION ..."
  "${SSH[@]}" bash -s -- "$TARGET" "$SESSION" "$PLUGIN" <<'REMOTE'
set -uo pipefail
target="$1"; session="$2"; plugin="$3"
log="$target/console.log"
tmux has-session -t "$session" 2>/dev/null || { echo "no tmux session $session; start the server first" >&2; exit 1; }
start=$(( $(stat -c %s "$log" 2>/dev/null || echo 0) + 1 ))
tmux send-keys -t "$session" "ru plugin reload $plugin" Enter
for ((i = 0; i < 30; i++)); do
  out="$(tail -c "+$start" "$log" 2>/dev/null | grep -a 'plugin' || true)"
  if grep -qE "plugin: (reloaded $plugin|reload $plugin failed)" <<<"$out"; then
    echo "$out" | tail -n 20
    grep -q "plugin: reloaded $plugin" <<<"$out"
    exit $?
  fi
  sleep 0.5
done
echo "no reload result after 15s (hibernating? set sv_hibernate_when_empty 0). Log so far:" >&2
tail -c "+$start" "$log" | tail -n 20 >&2
exit 1
REMOTE
  exit $?
fi

[[ $RESTART -eq 1 ]] || { echo "Deployed (server not restarted)."; exit 0; }

echo "Restarting tmux session $SESSION ..."
"${SSH[@]}" bash -s -- "$TARGET" "$SESSION" "$TIMEOUT" <<'REMOTE'
set -uo pipefail
target="$1"; session="$2"; timeout="$3"
log="$target/console.log"
touch "$log"
start=$(( $(stat -c %s "$log") + 1 ))

tmux kill-session -t "$session" 2>/dev/null
tmux new-session -d -s "$session" -x 250 -y 50 "$target/run.sh"
tmux pipe-pane -t "$session" -o "cat >> $log"

show_ru() { tail -c "+$start" "$log" | grep -a -i 'readyup' | tail -n 40; }

for ((i = 0; i < timeout; i++)); do
  if tail -c "+$start" "$log" | grep -a -q 'SV:  64 player server started'; then
    echo "Server started."
    show_ru
    exit 0
  fi
  if ! tmux has-session -t "$session" 2>/dev/null; then
    echo "Server process exited during startup. Last output:" >&2
    tail -c "+$start" "$log" | tail -n 30 >&2
    echo "--- ReadyUp lines ---" >&2
    show_ru >&2
    exit 1
  fi
  sleep 1
done
echo "Timed out after ${timeout}s waiting for server start." >&2
show_ru >&2
exit 1
REMOTE
