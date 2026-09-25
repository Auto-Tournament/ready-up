#!/usr/bin/env bash
# End-to-end test of install.sh against fake CS2 server trees, using the zips from
# scripts/package-release.sh (no network, no real server):
#
#   tests/installer/test_install.sh <dist-dir>
#
# Covers: fresh essentials install (with and without a Metamod line), rerun = no-op, user
# config kept (+ *.default when a template changes), fleet in essentials (idle template), removing
# and re-adding fleet (its data dir kept), adding skins from the full zip, removing
# it, the numbered-prompt and arrow-key pickers through a pty (`script`), uninstall, --purge,
# the license choice (--accept-license, saved choice, the prompt through a pty).
set -euo pipefail

DIST="$(cd "${1:?usage: $0 <dist-dir>}" && pwd)"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
INSTALL="$ROOT/install.sh"
ESS="$(ls "$DIST"/ready-up-essentials-*.zip)"
FULL="$(ls "$DIST"/ready-up-full-*.zip)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
fails=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
check() {  # <description> <command...>
  local d="$1"
  shift
  if "$@"; then pass "$d"; else fail "$d"; fi
}

make_server() {  # <dir> <with-metamod 0|1>
  mkdir -p "$1/game/csgo" "$1/game/bin/linuxsteamrt64"
  {
    printf '"GameInfo"\n{\n\tFileSystem\n\t{\n\t\tSearchPaths\n\t\t{\n'
    [[ "$2" == 1 ]] && printf '\t\t\tGame\tcsgo/addons/metamod\n'
    printf '\t\t\tGame_LowViolence\tcsgo_lv\n\t\t\tGame\tcsgo\n\t\t\tGame\tcsgo_imported\n\t\t\tGame\tcsgo_core\n'
    printf '\t\t\tGame\tcore\n\t\t}\n\t}\n}\n'
  } >"$1/game/csgo/gameinfo.gi"
  cp "$1/game/csgo/gameinfo.gi" "$1/game/csgo/gameinfo_branchspecific.gi"
}

line_of() { grep -nE "^[[:space:]]*Game[[:space:]]+$2[[:space:]]*$" "$1" | cut -d: -f1 | head -n1; }
count_ru() { grep -cE '^[[:space:]]*Game[[:space:]]+csgo/readyup[[:space:]]*$' "$1" || true; }
installed() { python3 -c 'import json,sys; print(" ".join(sorted(json.load(open(sys.argv[1]))["components"])))' "$1/game/csgo/readyup/installed.json"; }
run() { bash "$INSTALL" "$@" </dev/null; }
lic() { python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d["use"], d["how"], d["accepted_at"][-1])' "$1/game/csgo/readyup/license-acceptance.json"; }
# The file exists and has no active `key = value` line (only comments / blank lines).
idle_cfg() { [[ -f "$1" ]] && ! grep -Ev '^[[:space:]]*(//|#|$)' "$1" | grep -q .; }

echo "== license: unattended install without a choice is refused, nothing installed"
S="$T/server-nolic"
make_server "$S" 0
if run --dir "$S" --zip "$ESS" essentials >"$T/out" 2>&1; then
  fail "unattended install without --accept-license succeeded"
else
  check "clear error names --accept-license" grep -q "accept-license=noncommercial" "$T/out"
  check "nothing installed without a license choice" test ! -e "$S/game/csgo/readyup/bin/linuxsteamrt64/libserver.so"
fi
if run --dir "$S" --zip "$ESS" --accept-license=free essentials >"$T/out" 2>&1; then
  fail "--accept-license=free accepted"
else
  check "bad --accept-license value refused" grep -q "must be noncommercial or commercial" "$T/out"
fi
echo "== license: --accept-license=commercial is recorded"
run --dir "$S" --zip "$ESS" --accept-license=commercial essentials >"$T/out" 2>&1 || { cat "$T/out"; fail "commercial install exited non-zero"; }
check "license-acceptance.json: commercial, flag, UTC time" test "$(lic "$S")" = "commercial flag Z"

for mm in 0 1; do
  S="$T/server-mm$mm"
  make_server "$S" "$mm"
  CS="$S/game/csgo"
  echo "== fresh essentials install (metamod=$mm)"
  run --dir "$S" --zip "$ESS" --accept-license=noncommercial essentials >"$T/out" 2>&1 || { cat "$T/out"; fail "install exited non-zero"; continue; }
  check "license-acceptance.json: noncommercial, flag, UTC time" test "$(lic "$S")" = "noncommercial flag Z"
  check "core libserver.so installed" test -x "$CS/readyup/bin/linuxsteamrt64/libserver.so"
  check "match.so installed" test -x "$CS/readyup/plugins/match.so"
  check "engine-surface.json installed" test -f "$CS/readyup/bin/linuxsteamrt64/engine-surface.json"
  check "no skins.so in essentials" test ! -e "$CS/readyup/plugins/skins.so"
  check "no skins gamedata in essentials" test ! -e "$CS/readyup/bin/linuxsteamrt64/engine-surface.skins.json"
  check "readyup.cfg created from the example" test -f "$CS/readyup/bin/linuxsteamrt64/readyup.cfg"
  check "cfg/ReadyUp templates seeded" test -f "$CS/cfg/ReadyUp/live.cfg"
  check "fleet.so installed" test -x "$CS/readyup/plugins/fleet.so"
  check "fleet.cfg seeded, all comments (fleet stays idle)" idle_cfg "$CS/cfg/ReadyUp/fleet.cfg"
  check "installed.json lists core fleet match" test "$(installed "$S")" = "core fleet match"
  for gi in gameinfo.gi gameinfo_branchspecific.gi; do
    check "$gi: exactly one readyup line" test "$(count_ru "$CS/$gi")" = 1
    check "$gi: readyup before Game csgo" test "$(line_of "$CS/$gi" csgo/readyup)" -lt "$(line_of "$CS/$gi" csgo)"
    if [[ $mm == 1 ]]; then
      check "$gi: readyup right after metamod" test "$(line_of "$CS/$gi" csgo/readyup)" = "$(($(line_of "$CS/$gi" csgo/addons/metamod) + 1))"
    fi
    check "$gi: backup written" compgen -G "$CS/$gi.readyup-backup-*" >/dev/null
  done

  echo "== rerun is a no-op; user config kept"
  echo "// my edit" >>"$CS/cfg/ReadyUp/live.cfg"
  echo "debug=1" >>"$CS/readyup/bin/linuxsteamrt64/readyup.cfg"
  cp "$CS/gameinfo.gi" "$T/gi.before"
  nbak="$(compgen -G "$CS/gameinfo.gi.readyup-backup-*" | wc -l)"
  run --dir "$S" --zip "$ESS" essentials >"$T/out" 2>&1 || { cat "$T/out"; fail "rerun exited non-zero"; }
  check "gameinfo.gi unchanged on rerun" cmp -s "$T/gi.before" "$CS/gameinfo.gi"
  check "no new backup on rerun" test "$(compgen -G "$CS/gameinfo.gi.readyup-backup-*" | wc -l)" = "$nbak"
  check "edited live.cfg kept" grep -q "my edit" "$CS/cfg/ReadyUp/live.cfg"
  check "edited readyup.cfg kept" grep -q "^debug=1" "$CS/readyup/bin/linuxsteamrt64/readyup.cfg"
  check "no .default when the template did not change" test ! -e "$CS/cfg/ReadyUp/live.cfg.default"
  check "rerun reports up to date" grep -q "already up to date" "$T/out"
  check "rerun uses the saved license choice" grep -q "license: noncommercial use (accepted earlier" "$T/out"

  echo "== changed template -> .default next to the user's copy"
  echo "// older template" >>"$CS/readyup/cfg-templates/ReadyUp/live.cfg"
  run --dir "$S" --zip "$ESS" -y >"$T/out" 2>&1 || { cat "$T/out"; fail "update exited non-zero"; }
  check "live.cfg.default written" test -f "$CS/cfg/ReadyUp/live.cfg.default"
  check "user live.cfg still kept" grep -q "my edit" "$CS/cfg/ReadyUp/live.cfg"
done

S="$T/server-mm0"
CS="$S/game/csgo"
echo "== add skins from the full zip, then remove it"
run --dir "$S" --zip "$FULL" skins >"$T/out" 2>&1 || { cat "$T/out"; fail "skins install exited non-zero"; }
check "skins.so installed" test -f "$CS/readyup/plugins/skins.so"
check "skins gamedata next to the core" test -f "$CS/readyup/bin/linuxsteamrt64/engine-surface.skins.json"
check "installed.json has skins" grep -q '"skins"' "$CS/readyup/installed.json"
run --dir "$S" --remove skins >"$T/out" 2>&1 || { cat "$T/out"; fail "--remove skins exited non-zero"; }
check "skins.so removed" test ! -e "$CS/readyup/plugins/skins.so"
check "skins gamedata removed" test ! -e "$CS/readyup/bin/linuxsteamrt64/engine-surface.skins.json"
check "core still installed" test -f "$CS/readyup/bin/linuxsteamrt64/libserver.so"
check "installed.json back to core fleet match" test "$(installed "$S")" = "core fleet match"

echo "== add midas from the full zip (off by default), then remove it"
run --dir "$S" --zip "$FULL" midas >"$T/out" 2>&1 || { cat "$T/out"; fail "midas install exited non-zero"; }
check "midas.so installed" test -x "$CS/readyup/plugins/midas.so"
check "midas.cfg seeded, all comments (midas stays off)" idle_cfg "$CS/cfg/ReadyUp/midas.cfg"
run --dir "$S" --remove midas >"$T/out" 2>&1 || { cat "$T/out"; fail "--remove midas exited non-zero"; }
check "midas.so removed" test ! -e "$CS/readyup/plugins/midas.so"

echo "== remove fleet (its data dir and user fleet.cfg stay), add it back from the fleet zip"
mkdir -p "$CS/readyup/plugins/fleet" && echo '{}' >"$CS/readyup/plugins/fleet/credentials.json"
echo "url = https://t.example.com" >>"$CS/cfg/ReadyUp/fleet.cfg"
run --dir "$S" --remove fleet >"$T/out" 2>&1 || { cat "$T/out"; fail "--remove fleet exited non-zero"; }
check "fleet.so removed" test ! -e "$CS/readyup/plugins/fleet.so"
check "fleet credentials kept" test -f "$CS/readyup/plugins/fleet/credentials.json"
check "user fleet.cfg kept" grep -q "t.example.com" "$CS/cfg/ReadyUp/fleet.cfg"
check "installed.json is core match" test "$(installed "$S")" = "core match"
run --dir "$S" --zip "$(ls "$DIST"/ready-up-fleet-*.zip)" fleet >"$T/out" 2>&1 || { cat "$T/out"; fail "fleet install exited non-zero"; }
check "fleet.so back" test -x "$CS/readyup/plugins/fleet.so"
check "user fleet.cfg not overwritten" grep -q "t.example.com" "$CS/cfg/ReadyUp/fleet.cfg"
check "installed.json is core fleet match again" test "$(installed "$S")" = "core fleet match"

if command -v script >/dev/null 2>&1; then
  echo "== numbered picker (TERM=dumb) through a pty"
  printf '1,2,3,4\ny\n' | script -qec "TERM=dumb bash '$INSTALL' --dir '$S' --zip '$FULL'" /dev/null >"$T/out" 2>&1 || true
  check "numbered picker installed skins" test -f "$CS/readyup/plugins/skins.so"
  check "numbered picker showed the prompt" grep -q "Components to install" "$T/out"

  echo "== arrow-key picker through a pty: untick skins (down x3, space, enter), confirm"
  printf '\e[B\e[B\e[B \ny\n' | script -qec "TERM=xterm bash '$INSTALL' --dir '$S' --zip '$FULL'" /dev/null >"$T/out" 2>&1 || true
  check "arrow picker removed skins" test ! -e "$CS/readyup/plugins/skins.so"
  check "arrow picker kept core" test -f "$CS/readyup/bin/linuxsteamrt64/libserver.so"

  echo "== license prompt through a pty: commercial stops, noncommercial needs \"yes\""
  S2="$T/server-prompt"
  make_server "$S2" 0
  printf '2\n' | script -qec "TERM=dumb bash '$INSTALL' --dir '$S2' --zip '$ESS'" /dev/null >"$T/out" 2>&1 || true
  check "commercial answer names the contact" grep -q "sivert@autotournament.gg" "$T/out"
  check "commercial answer installs nothing" test ! -e "$S2/game/csgo/readyup/bin/linuxsteamrt64/libserver.so"
  check "commercial answer saves nothing" test ! -e "$S2/game/csgo/readyup/license-acceptance.json"
  printf '1\nno\n' | script -qec "TERM=dumb bash '$INSTALL' --dir '$S2' --zip '$ESS'" /dev/null >"$T/out" 2>&1 || true
  check "noncommercial without yes installs nothing" test ! -e "$S2/game/csgo/readyup/bin/linuxsteamrt64/libserver.so"
  check "the summary links the license" grep -q "polyformproject.org/licenses/noncommercial" "$T/out"
  printf '1\nyes\n\ny\n' | script -qec "TERM=dumb bash '$INSTALL' --dir '$S2' --zip '$ESS'" /dev/null >"$T/out" 2>&1 || true
  check "noncommercial + yes installs" test -f "$S2/game/csgo/readyup/bin/linuxsteamrt64/libserver.so"
  check "license-acceptance.json: noncommercial, prompt" test "$(lic "$S2")" = "noncommercial prompt Z"

  echo "== answering n changes nothing"
  printf '\ny\n' >/dev/null
  printf '\nn\n' | script -qec "TERM=dumb bash '$INSTALL' --dir '$S' --zip '$FULL'" /dev/null >"$T/out" 2>&1 || true
  check "declined run said nothing changed" grep -q "Nothing changed" "$T/out"
else
  echo "SKIP pty picker tests (no \`script\`)"
fi

if command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1; then
  echo "== release mode against a fake GitHub API (local http server)"
  W="$T/www"
  mkdir -p "$W/repos/test/ready-up/releases"
  cp "$DIST"/ready-up-*.zip "$DIST/SHA256SUMS" "$W/"
  PORT=$((20000 + RANDOM % 20000))
  python3 - "$W" "$PORT" "$DIST" <<'PY'
import json, os, re, sys
www, port, dist = sys.argv[1], sys.argv[2], sys.argv[3]
names = sorted(f for f in os.listdir(dist) if f.endswith(".zip") or f == "SHA256SUMS")
ver = next(re.match(r"ready-up-core-(.*)-linuxsteamrt64\.zip", n).group(1) for n in names if n.startswith("ready-up-core-"))
rel = {"tag_name": "v" + ver, "body": "Test release notes line 1\nline 2",
       "assets": [{"name": n, "browser_download_url": "http://127.0.0.1:%s/%s" % (port, n)} for n in names]}
json.dump(rel, open(os.path.join(www, "repos/test/ready-up/releases/latest"), "w"))
PY
  (cd "$W" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1) &
  HTTP_PID=$!
  for _ in $(seq 50); do
    (exec 9<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && break
    sleep 0.1
  done
  S="$T/server-release"
  make_server "$S" 0
  READYUP_API="http://127.0.0.1:$PORT" READYUP_REPO=test/ready-up run --dir "$S" --accept-license=noncommercial essentials >"$T/out" 2>&1 ||
    { cat "$T/out"; fail "release-mode install exited non-zero"; }
  check "release mode installed core" test -f "$S/game/csgo/readyup/bin/linuxsteamrt64/libserver.so"
  check "release mode printed the release notes" grep -q "Test release notes line 1" "$T/out"
  # Pretend an older core is installed: the update shows old -> new.
  python3 -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); d["components"]["core"]="0.0.1"; json.dump(d,open(p,"w"))' \
    "$S/game/csgo/readyup/installed.json"
  READYUP_API="http://127.0.0.1:$PORT" READYUP_REPO=test/ready-up run --dir "$S" -y >"$T/out" 2>&1 ||
    { cat "$T/out"; fail "release-mode update exited non-zero"; }
  check "update shows old → new" grep -q "core 0.0.1 → " "$T/out"
  echo "tampered" >>"$W/$(basename "$ESS" | sed 's/essentials/core/')"
  if READYUP_API="http://127.0.0.1:$PORT" READYUP_REPO=test/ready-up run --dir "$S" core >"$T/out" 2>&1; then
    fail "a zip that does not match SHA256SUMS was installed"
  else
    check "checksum mismatch refused" grep -q "checksum mismatch" "$T/out"
  fi
  kill "$HTTP_PID" 2>/dev/null || true
fi

echo "== not a server root"
if (cd "$T" && bash "$INSTALL" --zip "$ESS" -y </dev/null >"$T/out" 2>&1); then
  fail "ran outside a server root"
else
  check "clear error outside a server root" grep -q "not a CS2 server root" "$T/out"
fi

echo "== uninstall keeps config; --purge deletes it"
for mm in 0 1; do
  S="$T/server-mm$mm"
  CS="$S/game/csgo"
  run --dir "$S" --uninstall >"$T/out" 2>&1 || { cat "$T/out"; fail "uninstall exited non-zero"; }
  for gi in gameinfo.gi gameinfo_branchspecific.gi; do
    check "$gi: readyup line removed (metamod=$mm)" test "$(count_ru "$CS/$gi")" = 0
    check "$gi: Game csgo still there" test -n "$(line_of "$CS/$gi" csgo)"
  done
  [[ $mm == 1 ]] && check "metamod line untouched" test -n "$(line_of "$CS/gameinfo.gi" csgo/addons/metamod)"
  check "libserver.so removed" test ! -e "$CS/readyup/bin/linuxsteamrt64/libserver.so"
  check "readyup.cfg kept" test -f "$CS/readyup/bin/linuxsteamrt64/readyup.cfg"
  check "cfg/ReadyUp kept" test -f "$CS/cfg/ReadyUp/live.cfg"
done
S="$T/server-mm0"
run --dir "$S" --uninstall --purge >"$T/out" 2>&1 || { cat "$T/out"; fail "purge exited non-zero"; }
check "--purge removed readyup/" test ! -e "$S/game/csgo/readyup"
check "--purge removed cfg/ReadyUp" test ! -e "$S/game/csgo/cfg/ReadyUp"

echo
if [[ $fails -ne 0 ]]; then
  echo "installer tests: $fails FAILED"
  exit 1
fi
echo "installer tests: ALL OK"
