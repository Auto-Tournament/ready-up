#!/usr/bin/env bash
# Fast CS2 update detection for .github/workflows/cs2-update-watch.yml.
#
# GitHub's `schedule` trigger is best effort and can run hours late, so run this every 2-3
# minutes on any always-on box (systemd --user timer or cron; docs/CS2-COMPAT.md). It compares
# CS2's public buildid (cs2-buildid.sh) with the last checked one on the `cs2-build` branch and,
# when they differ, starts the watch workflow with workflow_dispatch. The 15-minute GitHub cron
# stays as the fallback.
#
# Cross-check: Steam's keyless ISteamApps/UpToDateCheck is asked whether the recorded
# PatchVersion is still current. When Steam already says "outdated" but api.steamcmd.net still
# has the old buildid, it only logs (the workflow keys its binary cache and state on the
# buildid, so dispatching before the buildid moves would verify the old build again).
#
#   scripts/ci/cs2-poll.sh
#
# Env:
#   CS2_POLL_TOKEN     fine-grained PAT, this repo only, "Actions: read and write" (required
#                      unless DRY_RUN=1). Keep it in a root-only / user-only env file.
#   CS2_POLL_REPO      owner/repo                         (default Auto-Tournament/ready-up)
#   CS2_POLL_WORKFLOW  workflow file                      (default cs2-update-watch.yml)
#   CS2_POLL_REF       branch the workflow runs on        (default master)
#   CS2_POLL_STATE_BRANCH                                 (default cs2-build)
#   CS2_POLL_COOLDOWN  seconds before the same buildid is dispatched again (default 1800)
#   CS2_POLL_STATE_DIR where the last dispatch is remembered
#                      (default ${XDG_STATE_HOME:-~/.local/state}/readyup-cs2-poll)
#   DRY_RUN=1          print what would be dispatched, do not call the API
#
# Exit: 0 = nothing to do or dispatched, 1 = could not read the buildid / state / dispatch.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${CS2_POLL_REPO:-Auto-Tournament/ready-up}"
WORKFLOW="${CS2_POLL_WORKFLOW:-cs2-update-watch.yml}"
REF="${CS2_POLL_REF:-master}"
STATE_BRANCH="${CS2_POLL_STATE_BRANCH:-cs2-build}"
COOLDOWN="${CS2_POLL_COOLDOWN:-1800}"
STATE_DIR="${CS2_POLL_STATE_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/readyup-cs2-poll}"
API="${GITHUB_API_URL:-https://api.github.com}"
TOKEN="${CS2_POLL_TOKEN:-}"
DRY_RUN="${DRY_RUN:-0}"

log() { echo "cs2-poll: $*"; }

if [[ -z "$TOKEN" && "$DRY_RUN" != 1 ]]; then
  log "CS2_POLL_TOKEN is not set" >&2
  exit 1
fi

auth=()
[[ -n "$TOKEN" ]] && auth=(-H "Authorization: Bearer $TOKEN")

buildid="$("$HERE/cs2-buildid.sh")" || { log "could not read the public buildid" >&2; exit 1; }

# Last checked state, straight from the API (raw.githubusercontent.com is cached for minutes).
if ! state="$(curl -fsS --max-time 20 "${auth[@]}" -H 'Accept: application/vnd.github.raw' \
    -H 'X-GitHub-Api-Version: 2022-11-28' \
    "$API/repos/$REPO/contents/state.env?ref=$STATE_BRANCH")"; then
  log "could not read state.env on $REPO@$STATE_BRANCH" >&2
  exit 1
fi
prev_build="$(printf '%s\n' "$state" | sed -n 's/^BUILDID=//p' | head -n1)"
prev_patch="$(printf '%s\n' "$state" | sed -n 's/^PATCH_VERSION=//p' | head -n1)"

# Cross-check with Steam (keyless). Informational only; never fatal.
steam_note=""
if [[ -n "$prev_patch" ]]; then
  if utd="$(curl -fsS --max-time 15 \
      "https://api.steampowered.com/ISteamApps/UpToDateCheck/v1/?appid=730&version=$prev_patch" 2>/dev/null)"; then
    steam_note="$(printf '%s' "$utd" | python3 -c '
import json, sys
r = json.load(sys.stdin).get("response", {})
if not r.get("success"):
    print("steam: no answer")
elif r.get("up_to_date"):
    print("steam: %s is current" % sys.argv[1])
else:
    print("steam: %s is OUTDATED (required_version %s)" % (sys.argv[1], r.get("required_version", "?")))
' "$prev_patch" 2>/dev/null || true)"
  fi
fi

if [[ "$buildid" == "$prev_build" ]]; then
  case "$steam_note" in
    *OUTDATED*) log "buildid $buildid unchanged, but $steam_note; waiting for api.steamcmd.net to catch up" ;;
    *) log "buildid $buildid unchanged${steam_note:+ ($steam_note)}" ;;
  esac
  exit 0
fi

# Changed. Dispatch once per buildid (the workflow needs ~10 min to record the new state).
mkdir -p "$STATE_DIR"
stamp="$STATE_DIR/last-dispatch"
now="$(date +%s)"
if [[ -f "$stamp" ]]; then
  read -r last_build last_time <"$stamp" || true
  if [[ "${last_build:-}" == "$buildid" && $((now - ${last_time:-0})) -lt "$COOLDOWN" ]]; then
    log "buildid $buildid (checked: ${prev_build:-none}) already dispatched $((now - last_time))s ago"
    exit 0
  fi
fi

log "CS2 buildid ${prev_build:-none} -> $buildid${steam_note:+ ($steam_note)}: dispatching $WORKFLOW on $REF"
if [[ "$DRY_RUN" == 1 ]]; then
  log "[dry-run] POST $API/repos/$REPO/actions/workflows/$WORKFLOW/dispatches {\"ref\":\"$REF\"}"
  exit 0
fi
if ! curl -fsS --max-time 20 -X POST "${auth[@]}" -H 'Accept: application/vnd.github+json' \
    -H 'X-GitHub-Api-Version: 2022-11-28' \
    "$API/repos/$REPO/actions/workflows/$WORKFLOW/dispatches" -d "{\"ref\":\"$REF\"}"; then
  log "workflow_dispatch failed" >&2
  exit 1
fi
echo "$buildid $now" >"$stamp"
log "dispatched"
