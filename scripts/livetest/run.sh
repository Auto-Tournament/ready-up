#!/usr/bin/env bash
# Ready Up live regression test: a bot-only match (knife -> pick -> live ->
# halftime -> map end) on the readyup-test CS2 server, asserted from the
# `[ReadyUp] state:` lines and log events. No human needs to join.
#
# Usage: scripts/livetest/run.sh [options]     (see --help, README.md)
#   Exit 0 PASS, 1 FAIL, 2 server busy (humans connected) or unavailable.
#
# Defaults target the cs2 box: server /home/cs2servermanager/readyup-test,
# tmux session ru-test, reached as cs2servermanager@localhost over ssh
# (env RU_TARGET / RU_SESSION / RU_SSH, same as scripts/dev-deploy.sh).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
command -v python3 >/dev/null || { echo "livetest: python3 is required" >&2; exit 2; }
exec python3 -u "$HERE/livetest.py" "$@"
