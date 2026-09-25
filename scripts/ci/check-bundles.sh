#!/usr/bin/env bash
# Checks the zips from scripts/package-release.sh:
#   - SHA256SUMS matches every zip
#   - every zip carries a manifest per component, and each manifest lists exactly the zip's
#     files for that component
#   - core / match / essentials contain NO skins code or gamedata: no skins.so, no
#     engine-surface.skins.json, no skins warning, and libserver.so has no skins SQL/symbols
#   - skins / full do contain skins.so + engine-surface.skins.json
#   - fleet / essentials / full contain plugins/fleet.so + an all-comments fleet.cfg template
#     (the link stays idle until configured); the fleet zip has no match.so
#   - match / essentials / full contain plugins/match.so (+ the cfg/ReadyUp templates); the core
#     zip does not, and its libserver.so has no match flow in it (it must run without match.so)
#
#   scripts/ci/check-bundles.sh <dist-dir> <version>
set -euo pipefail

DIST="${1:?usage: $0 <dist-dir> <version>}"
VERSION="${2:?usage: $0 <dist-dir> <version>}"
SUFFIX="$VERSION-linuxsteamrt64.zip"
fail=0
bad() { echo "  BAD  $*"; fail=1; }
ok() { echo "  ok   $*"; }

echo "SHA256SUMS:"
if (cd "$DIST" && sha256sum --quiet -c SHA256SUMS); then ok "all checksums match"; else bad "checksum mismatch"; fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

extract() {  # <bundle> -> prints the extraction dir
  local zip="$DIST/ready-up-$1-$SUFFIX" dir="$WORK/$1"
  [[ -f "$zip" ]] || { echo "missing $zip" >&2; return 1; }
  mkdir -p "$dir"
  python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$zip" "$dir"
  echo "$dir"
}

check_manifests() {  # <dir> <expected components...>
  local dir="$1"
  shift
  python3 - "$dir" "$@" <<'PY' || fail=1
import json, os, sys
root, *want = sys.argv[1:]
mdir = os.path.join(root, "readyup", "manifests")
have = sorted(f[:-5] for f in os.listdir(mdir) if f.endswith(".json"))
ok = True
if have != sorted(want):
    print("  BAD  manifests %s, expected %s" % (have, sorted(want))); ok = False
listed = set()
for c in have:
    m = json.load(open(os.path.join(mdir, c + ".json")))
    for f in m["files"]:
        listed.add(f)
        if not os.path.isfile(os.path.join(root, f)):
            print("  BAD  %s lists missing file %s" % (c, f)); ok = False
actual = set()
for d, _, fs in os.walk(root):
    for f in fs:
        rel = os.path.relpath(os.path.join(d, f), root)
        if not rel.startswith("readyup/manifests/"):
            actual.add(rel)
extra = sorted(actual - listed)
if extra:
    print("  BAD  files not in any manifest: %s" % extra); ok = False
print("  %s   manifests %s cover every file" % ("ok" if ok else "BAD", have))
sys.exit(0 if ok else 1)
PY
}

no_skins() {  # <bundle>
  local dir
  dir="$(extract "$1")"
  local hits
  hits="$(cd "$dir" && find . -iname '*skins*' -o -iname 'SKINS-WARNING*' | sed 's|^\./||')"
  if [[ -n "$hits" ]]; then bad "$1 contains skins files: $hits"; else ok "$1: no skins files"; fi
  local so="$dir/readyup/bin/linuxsteamrt64/libserver.so"
  if [[ -f "$so" ]]; then
    if grep -aq 'readyup_weapon_\|weapon_paints' "$so"; then bad "$1: libserver.so contains skins code"; else ok "$1: libserver.so has no skins code"; fi
  fi
}

has_match() {  # <bundle>
  local dir="$WORK/$1"
  for f in readyup/plugins/match.so readyup/cfg-templates/ReadyUp/live.cfg; do
    if [[ -f "$dir/$f" ]]; then ok "$1 has $f"; else bad "$1 lacks $f"; fi
  done
}

core_without_match() {
  local dir="$WORK/core"
  if [[ -e "$dir/readyup/plugins/match.so" ]]; then bad "core contains match.so"; else ok "core: no match.so"; fi
  # The match flow's log formats / commands must not be compiled into the core any more.
  if grep -aq 'match-load\[\|knife: starting knife round\|ru_match_token' "$dir/readyup/bin/linuxsteamrt64/libserver.so"; then
    bad "core: libserver.so still contains the match flow"
  else
    ok "core: libserver.so has no match flow"
  fi
}

has_fleet() {  # <bundle>
  local dir="$WORK/$1"
  for f in readyup/plugins/fleet.so readyup/cfg-templates/ReadyUp/fleet.cfg; do
    if [[ -f "$dir/$f" ]]; then ok "$1 has $f"; else bad "$1 lacks $f"; fi
  done
  # The template must leave the link idle: no active (uncommented) key = value line.
  local tpl="$dir/readyup/cfg-templates/ReadyUp/fleet.cfg"
  if [[ -f "$tpl" ]] && grep -Ev '^[[:space:]]*(//|#|$)' "$tpl" | grep -q .; then
    bad "$1: fleet.cfg template has active settings (fleet must stay idle by default)"
  else
    ok "$1: fleet.cfg template is all comments"
  fi
}

has_skins() {  # <bundle>
  local dir
  dir="$(extract "$1")"
  for f in readyup/plugins/skins.so readyup/bin/linuxsteamrt64/engine-surface.skins.json; do
    if [[ -f "$dir/$f" ]]; then ok "$1 has $f"; else bad "$1 lacks $f"; fi
  done
}

has_notices() {  # <bundle> -- every zip carrying the core component ships the license +
                 # BSD-3-Clause/GPLv2 third-party notices required by the vendored
                 # third_party/distorm and third_party/funchook.
  local dir="$WORK/$1"
  for f in readyup/LICENSE readyup/THIRD_PARTY_NOTICES.txt; do
    if [[ -f "$dir/$f" ]]; then ok "$1 has $f"; else bad "$1 lacks $f"; fi
  done
}

echo "core:";       no_skins core;       check_manifests "$WORK/core" core; core_without_match; has_notices core
echo "match:";      no_skins match;      check_manifests "$WORK/match" match; has_match match
echo "fleet:";      no_skins fleet;      check_manifests "$WORK/fleet" fleet; has_fleet fleet
if [[ -e "$WORK/fleet/readyup/plugins/match.so" ]]; then bad "fleet contains match.so"; else ok "fleet: no match.so"; fi
echo "essentials:"; no_skins essentials; check_manifests "$WORK/essentials" core match fleet; has_match essentials; has_fleet essentials; has_notices essentials
echo "hello:";      no_skins hello;      check_manifests "$WORK/hello" hello
echo "midas:";      no_skins midas;      check_manifests "$WORK/midas" midas
for b in core essentials; do
  if [[ -e "$WORK/$b/readyup/plugins/midas.so" ]]; then bad "$b contains midas.so"; else ok "$b: no midas.so"; fi
done
echo "skins:";      has_skins skins;     check_manifests "$WORK/skins" skins
echo "full:";       has_skins full; has_match full; has_fleet full; has_notices full
full_components=(core match fleet skins hello midas)
[[ -f "$WORK/full/readyup/manifests/tools.json" ]] && full_components+=(tools)
check_manifests "$WORK/full" "${full_components[@]}"

if [[ $fail -ne 0 ]]; then
  echo "check-bundles: FAILED" >&2
  exit 1
fi
echo "check-bundles: OK"
