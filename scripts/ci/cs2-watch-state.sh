#!/usr/bin/env bash
# State for .github/workflows/cs2-update-watch.yml, kept on the orphan branch `cs2-build`
# (file state.env + an append-only history.log). Needs a git checkout with an `origin`
# remote and push rights for `write` (GITHUB_TOKEN with contents: write in CI).
#
#   scripts/ci/cs2-watch-state.sh read
#       prints BUILDID=, SURFACE_SHA256=, STATUS=, VERIFIED_COMMIT=, UPDATED= (empty if none)
#   scripts/ci/cs2-watch-state.sh write <buildid> <surface-sha256> <pass|fail> <commit> [patch-version]
#       CS2_STATE_FILES="a/compat.json a/badge.json" also commits those files (by basename) to the
#       branch root; they are served from raw.githubusercontent.com (README badge, docs/CS2-COMPAT.md).
#   scripts/ci/cs2-watch-state.sh publish <message>
#       commits only CS2_STATE_FILES (state.env untouched): the dynamic stages of
#       .github/workflows/cs2-dynamic.yml update compat.json + badge.json this way.
set -euo pipefail

BRANCH="${CS2_STATE_BRANCH:-cs2-build}"
cmd="${1:?usage: $0 read | write <buildid> <surface-sha256> <status> <commit> [patch-version]}"

case "$cmd" in
  read)
    if git fetch -q --depth=1 origin "refs/heads/$BRANCH" 2>/dev/null; then
      git show FETCH_HEAD:state.env 2>/dev/null || true
    fi
    ;;
  publish)
    msg="${2:?message}"
    wt="$(mktemp -d)"
    trap 'git worktree remove --force "$wt" >/dev/null 2>&1 || rm -rf "$wt"' EXIT
    git fetch -q origin "refs/heads/$BRANCH"
    git worktree add -q --detach "$wt" FETCH_HEAD
    for f in ${CS2_STATE_FILES:-}; do
      [[ -f "$f" ]] || { echo "warning: $f not found, not recorded" >&2; continue; }
      cp "$f" "$wt/$(basename "$f")"
      git -C "$wt" add "$(basename "$f")"
    done
    echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) $msg" >>"$wt/history.log"
    git -C "$wt" add history.log
    git -C "$wt" -c user.name="github-actions[bot]" \
      -c user.email="41898282+github-actions[bot]@users.noreply.github.com" \
      commit -q -m "$msg"
    git -C "$wt" push -q origin "HEAD:refs/heads/$BRANCH"
    echo "recorded on $BRANCH: $msg"
    ;;
  write)
    buildid="${2:?buildid}" surface="${3:?surface sha}" status="${4:?status}" commit="${5:?commit}"
    patch="${6:-}"
    now="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    wt="$(mktemp -d)"
    trap 'git worktree remove --force "$wt" >/dev/null 2>&1 || rm -rf "$wt"' EXIT
    if git fetch -q origin "refs/heads/$BRANCH" 2>/dev/null; then
      git worktree add -q --detach "$wt" FETCH_HEAD
    else
      git worktree add -q --detach "$wt"
      git -C "$wt" checkout -q --orphan "cs2-state-tmp"
      git -C "$wt" rm -rq --cached . >/dev/null 2>&1 || true
      find "$wt" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
      cat >"$wt/README.md" <<'EOF'
# cs2-build

State for `.github/workflows/cs2-update-watch.yml`: the last CS2 (app 730) public buildid
checked against `gamedata/engine-surface.json`. Written by CI; do not edit by hand.
EOF
    fi
    cat >"$wt/state.env" <<EOF
BUILDID=$buildid
PATCH_VERSION=$patch
SURFACE_SHA256=$surface
STATUS=$status
VERIFIED_COMMIT=$commit
UPDATED=$now
EOF
    echo "$now buildid=$buildid patch=$patch status=$status commit=$commit surface=$surface" >>"$wt/history.log"
    git -C "$wt" add README.md state.env history.log 2>/dev/null || git -C "$wt" add state.env history.log
    for f in ${CS2_STATE_FILES:-}; do
      if [[ -f "$f" ]]; then
        cp "$f" "$wt/$(basename "$f")"
        git -C "$wt" add "$(basename "$f")"
      else
        echo "warning: $f not found, not recorded" >&2
      fi
    done
    if [[ "$status" == pass ]]; then msg="CS2 build $buildid verified"; else msg="CS2 build $buildid: engine surface broken"; fi
    git -C "$wt" -c user.name="github-actions[bot]" \
      -c user.email="41898282+github-actions[bot]@users.noreply.github.com" \
      commit -q -m "$msg"
    git -C "$wt" push -q origin "HEAD:refs/heads/$BRANCH"
    echo "recorded on $BRANCH: $msg"
    ;;
  *)
    echo "unknown command: $cmd" >&2
    exit 2
    ;;
esac
