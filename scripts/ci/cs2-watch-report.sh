#!/usr/bin/env bash
# Report the result of a CS2 update check (used by .github/workflows/cs2-update-watch.yml).
#
#   scripts/ci/cs2-watch-report.sh fail <buildid> <report.md> <run-url>
#       open (or refresh) the GitHub issue labeled `cs2-update` with the report,
#       and post to Discord if DISCORD_WEBHOOK_URL is set
#   scripts/ci/cs2-watch-report.sh pass <buildid> <commit-sha> <run-url>
#       comment "CS2 build <buildid> verified" on the open `cs2-update` issue and close it;
#       with no open issue, comment on the verified commit instead
#
# Needs `gh` authenticated (GH_TOKEN) with issues: write (+ contents: write for commit
# comments) and GH_REPO or a checkout. DRY_RUN=1 prints the gh/curl calls instead.
set -euo pipefail

LABEL="cs2-update"
mode="${1:?usage: $0 fail|pass ...}"
buildid="${2:?buildid}"

gh_() {
  if [[ "${DRY_RUN:-0}" == 1 ]]; then
    printf '[dry-run] gh'; printf ' %q' "$@"; echo
  else
    gh "$@"
  fi
}

open_issue() {
  gh issue list --label "$LABEL" --state open --limit 1 --json number --jq '.[0].number // empty' 2>/dev/null || true
}

discord() {
  local text="$1"
  [[ -n "${DISCORD_WEBHOOK_URL:-}" ]] || return 0
  local payload
  payload="$(python3 -c 'import json,sys; print(json.dumps({"content": sys.argv[1][:1990], "allowed_mentions": {"parse": []}}))' "$text")"
  if [[ "${DRY_RUN:-0}" == 1 ]]; then
    echo "[dry-run] POST discord webhook: $payload"
    return 0
  fi
  curl -fsS --max-time 20 -H 'Content-Type: application/json' -d "$payload" "$DISCORD_WEBHOOK_URL" >/dev/null \
    || echo "warning: Discord webhook post failed" >&2
}

case "$mode" in
  fail)
    report="${3:?report.md}" run_url="${4:?run url}"
    body="$(mktemp)"
    trap 'rm -f "$body"' EXIT
    {
      echo "CS2 build \`$buildid\` no longer matches \`gamedata/engine-surface.json\`."
      echo "Ready Up disables itself (or the affected feature) on servers running this build until the surface is fixed."
      echo
      [[ -f "$report" ]] && cat "$report" || echo "_No report produced; see the run log._"
      echo
      echo "Run: $run_url"
      echo
      echo "<sub>Opened by cs2-update-watch. It comments \"CS2 build N verified\" and closes this issue once a build passes.</sub>"
    } >"$body"

    gh_ label create "$LABEL" --color B60205 --description "CS2 update broke the engine surface" --force >/dev/null
    num="$(open_issue)"
    title="CS2 update $buildid broke the engine surface"
    if [[ -n "$num" ]]; then
      gh_ issue edit "$num" --title "$title" --body-file "$body" >/dev/null
      gh_ issue comment "$num" --body "Still failing on CS2 build \`$buildid\`. Report refreshed above. Run: $run_url" >/dev/null
      echo "refreshed issue #$num"
    else
      gh_ issue create --title "$title" --label "$LABEL" --body-file "$body"
    fi
    fails="$(grep -E '^\| (signature|rtti|hook site)' "$report" 2>/dev/null | grep -c '\*\*FAIL\*\*' || true)"
    discord "⚠️ **Ready Up**: CS2 build \`$buildid\` broke the engine surface (${fails:-?} failing checks). $run_url"
    ;;
  pass)
    commit="${3:?commit sha}" run_url="${4:?run url}"
    msg="CS2 build $buildid verified"
    num="$(open_issue)"
    if [[ -n "$num" ]]; then
      gh_ issue comment "$num" --body "$msg ([run]($run_url))." >/dev/null
      gh_ issue close "$num" --reason completed >/dev/null
      echo "commented + closed issue #$num"
      discord "✅ **Ready Up**: $msg. $run_url"
    else
      repo="${GH_REPO:-$(gh repo view --json nameWithOwner --jq .nameWithOwner)}"
      gh_ api "repos/$repo/commits/$commit/comments" -f body="$msg ([run]($run_url))." >/dev/null
      echo "commented on $commit"
    fi
    ;;
  *)
    echo "unknown mode: $mode" >&2
    exit 2
    ;;
esac
