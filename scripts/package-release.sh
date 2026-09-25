#!/usr/bin/env bash
# Package Ready Up release zips from one build. Every zip's root is the contents of
# game/csgo, so it can be extracted straight into a server's game/csgo (or handed to the
# installer: install.sh --zip <zip>).
#
#   scripts/package-release.sh <build-dir> <version> <out-dir>
#
# <build-dir> holds libserver.so, plugins/match.so, plugins/fleet.so, plugins/skins.so,
# plugins/hello.so, plugins/midas.so, plugins/whitelist.so, plugins/practice.so, plugins/essentials.so,
# plugins/deathmatch.so and (optionally) readyup_sigcheck / readyup_hookcheck.
#
# Component zips (the installer mixes these):
#   ready-up-core-<v>-linuxsteamrt64.zip    the core (libserver.so, engine-surface.json,
#                                            readyup.cfg.example, tools, docs, LICENSE,
#                                            THIRD_PARTY_NOTICES.txt)
#   ready-up-match-<v>-linuxsteamrt64.zip   plugins/match.so (ready-up, scrims, knife, pauses,
#                                            practice, match configs, webhooks, demos) + the
#                                            cfg/ReadyUp/*.cfg templates it execs
#   ready-up-fleet-<v>-linuxsteamrt64.zip   plugins/fleet.so (link to the Auto Tournament platform)
#                                            + a commented cfg/ReadyUp/fleet.cfg template. Idle
#                                            until a url is configured, so bundles carry it.
#   ready-up-skins-<v>-linuxsteamrt64.zip   plugins/skins.so + engine-surface.skins.json
#   ready-up-hello-<v>-linuxsteamrt64.zip   plugins/hello.so (example plugin)
#   ready-up-midas-<v>-linuxsteamrt64.zip   plugins/midas.so + cfg template (fun: gold weapons, off by default)
#   ready-up-whitelist-<v>-linuxsteamrt64.zip plugins/whitelist.so (only listed players; off by default)
#   ready-up-practice-<v>-linuxsteamrt64.zip  plugins/practice.so + prac.cfg / practice.cfg templates
#   ready-up-essentials-plugin-<v>-...zip   plugins/essentials.so (admins, map commands); "essentials" alone is the bundle
#   ready-up-deathmatch-<v>-linuxsteamrt64.zip plugins/deathmatch.so + deathmatch.cfg template (off until .ru dm ffa|tdm)
# Bundles (for manual download):
#   ready-up-essentials-<v>-...zip          core + essentials + match + fleet + practice. The default. NO skins.
#   ready-up-full-<v>-...zip                core + essentials + match + fleet + practice + skins + hello + midas + whitelist + deathmatch + readyup_sigcheck/hookcheck
# Plus SHA256SUMS over every zip.
#
# Each component ships readyup/manifests/<component>.json ({component, version, files}),
# which is how install.sh knows what to update or remove. Nothing in any zip overwrites
# admin-owned files: readyup.cfg, cfg/ReadyUp/*.cfg and the plugins' JSON data (admins,
# state, loadouts) are never shipped as such (readyup.cfg.example and readyup/cfg-templates/ are).
set -euo pipefail

BUILD="${1:?usage: $0 <build-dir> <version> <out-dir>}"
VERSION="${2:?usage: $0 <build-dir> <version> <out-dir>}"
OUT="${3:?usage: $0 <build-dir> <version> <out-dir>}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$(cd "$BUILD" && pwd)"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
EPOCH="${SOURCE_DATE_EPOCH:-$(date +%s)}"
SUFFIX="$VERSION-linuxsteamrt64.zip"

for f in libserver.so plugins/match.so plugins/fleet.so plugins/skins.so plugins/hello.so plugins/midas.so plugins/whitelist.so plugins/practice.so plugins/essentials.so plugins/deathmatch.so; do
  [[ -f "$BUILD/$f" ]] || { echo "package-release: missing $BUILD/$f" >&2; exit 1; }
done

# stage_component <name> <description> <src>:<dest-under-readyup>[:mode] ...
stage_component() {
  local name="$1" desc="$2"
  shift 2
  local dir="$WORK/c/$name" spec src dst mode
  mkdir -p "$dir/readyup/manifests"
  local files=()
  for spec in "$@"; do
    IFS=: read -r src dst mode <<<"$spec"
    install -D -m "${mode:-644}" "$src" "$dir/readyup/$dst"
    files+=("readyup/$dst")
  done
  python3 - "$dir/readyup/manifests/$name.json" "$name" "$VERSION" "$desc" "${files[@]}" <<'PY'
import json, sys
path, name, version, desc, *files = sys.argv[1:]
json.dump({"component": name, "version": version, "description": desc, "files": sorted(files)},
          open(path, "w"), indent=2)
open(path, "a").write("\n")
PY
}

core_files=(
  "$BUILD/libserver.so:bin/linuxsteamrt64/libserver.so:755"
  "$ROOT_DIR/gamedata/engine-surface.json:bin/linuxsteamrt64/engine-surface.json"
  "$ROOT_DIR/cfg/readyup.cfg.example:bin/linuxsteamrt64/readyup.cfg.example"
  "$ROOT_DIR/scripts/patch_gameinfo.py:tools/patch_gameinfo.py:755"
  "$ROOT_DIR/scripts/migrate-postgres-to-json.py:tools/migrate-postgres-to-json.py:755"
  "$ROOT_DIR/install.sh:tools/install.sh:755"
  "$ROOT_DIR/README.md:README.md"
  "$ROOT_DIR/docs/INSTALL.md:INSTALL.md"
)
[[ -f "$ROOT_DIR/LICENSE" ]] && core_files+=("$ROOT_DIR/LICENSE:LICENSE")
[[ -f "$ROOT_DIR/THIRD_PARTY_NOTICES.txt" ]] && core_files+=("$ROOT_DIR/THIRD_PARTY_NOTICES.txt:THIRD_PARTY_NOTICES.txt")
printf '%s\n' "$VERSION" >"$WORK/VERSION"
{
  echo "version=$VERSION"
  echo "commit=$(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "built=$(date -u -d "@$EPOCH" +%Y-%m-%dT%H:%M:%SZ)"
  echo "libserver_sha256=$(sha256sum "$BUILD/libserver.so" | cut -d' ' -f1)"
  [[ -n "${CS2_BUILDID:-}" ]] && echo "verified_cs2_buildid=$CS2_BUILDID"
  [[ -n "${CS2_PATCH_VERSION:-}" ]] && echo "verified_cs2_patch_version=$CS2_PATCH_VERSION"
} >"$WORK/BUILD_INFO"
core_files+=("$WORK/VERSION:VERSION" "$WORK/BUILD_INFO:BUILD_INFO")
stage_component core "Ready Up core: libserver.so, engine surface, plugin host, status endpoint" "${core_files[@]}"

# The match flow (plugins/match) and the ReadyUp/*.cfg files it execs (warmup, knife, live, ...).
match_files=("$BUILD/plugins/match.so:plugins/match.so:755")
for f in "$ROOT_DIR"/cfg/ReadyUp/*.cfg; do
  case "$(basename "$f")" in fleet.cfg | midas.cfg | prac.cfg | practice.cfg | deathmatch.cfg) continue ;; esac  # the fleet / midas / practice / deathmatch components' own
  match_files+=("$f:cfg-templates/ReadyUp/$(basename "$f")")
done
stage_component match "Ready-up, scrims, knife round, pauses, practice, match configs, webhooks, demos" "${match_files[@]}"

# The platform link. fleet.so stays idle without a url, and the template is all comments. The
# protocol schemas (plugins/fleet/protocol) are for tests only; the plugin does not read them.
stage_component fleet "Link to the Auto Tournament platform (idle until configured)" \
  "$BUILD/plugins/fleet.so:plugins/fleet.so:755" \
  "$ROOT_DIR/cfg/ReadyUp/fleet.cfg:cfg-templates/ReadyUp/fleet.cfg"

cat >"$WORK/SKINS-WARNING.txt" <<'EOF'
Ready Up skins (weapon paints, knives, gloves, agents)

Servers that run skin changers risk a GSLT ban from Valve. Only install this on servers
where you accept that risk. Remove readyup/plugins/skins.so and
readyup/bin/linuxsteamrt64/engine-surface.skins.json (or run install.sh and untick Skins)
to go back to a skins-free server.
EOF
stage_component skins "Weapon paints, knives, gloves, agents (servers running skin changers risk GSLT bans)" \
  "$BUILD/plugins/skins.so:plugins/skins.so:755" \
  "$ROOT_DIR/gamedata/engine-surface.skins.json:bin/linuxsteamrt64/engine-surface.skins.json" \
  "$WORK/SKINS-WARNING.txt:SKINS-WARNING.txt"

stage_component hello "Example plugin (.hello, hello_status); for plugin developers" \
  "$BUILD/plugins/hello.so:plugins/hello.so:755"

# Fun plugin; the template keeps it off (enabled=0).
stage_component midas "Fun: weapons picked up by chosen players turn gold (off by default, never under the valve ruleset)" \
  "$BUILD/plugins/midas.so:plugins/midas.so:755" \
  "$ROOT_DIR/cfg/ReadyUp/midas.cfg:cfg-templates/ReadyUp/midas.cfg"

# Server basics apart from the match flow: admins (admins.json) and map change / reload / restart.
stage_component essentials "Server basics: admins (admins.json), map change / reload / restart, default maps per mode" \
  "$BUILD/plugins/essentials.so:plugins/essentials.so:755"

# Practice mode + tools (.prac, .savepos, .rethrow, .bot, ...): its own plugin, so a server can
# run it without the match flow. Ships prac.cfg (the cvars it execs) and its settings template.
stage_component practice "Practice mode and tools (.prac, .savepos/.loadpos, .spawn, .rethrow, .bot)" \
  "$BUILD/plugins/practice.so:plugins/practice.so:755" \
  "$ROOT_DIR/cfg/ReadyUp/prac.cfg:cfg-templates/ReadyUp/prac.cfg" \
  "$ROOT_DIR/cfg/ReadyUp/practice.cfg:cfg-templates/ReadyUp/practice.cfg"

stage_component whitelist "Only listed players may stay on the server (off until ru whitelist on)" \
  "$BUILD/plugins/whitelist.so:plugins/whitelist.so:755"

# Deathmatch (FFA / TDM on CS2's deathmatch game mode): off until an admin switches it on.
stage_component deathmatch "Deathmatch: free for all / team deathmatch with kill + time limits and a leaderboard (off until .ru dm ffa|tdm)" \
  "$BUILD/plugins/deathmatch.so:plugins/deathmatch.so:755" \
  "$ROOT_DIR/cfg/ReadyUp/deathmatch.cfg:cfg-templates/ReadyUp/deathmatch.cfg"

extras=()
for t in readyup_sigcheck readyup_hookcheck; do
  [[ -f "$BUILD/$t" ]] && extras+=("$BUILD/$t:tools/$t:755")
done
if [[ ${#extras[@]} -gt 0 ]]; then
  stage_component tools "Offline gamedata checkers (readyup_sigcheck, readyup_hookcheck)" "${extras[@]}"
fi

# Guard: the old dev installer (stops csm servers, wipes DB rows) must never ship.
if grep -rq 'csm stop\|TRUNCATE readyup_admins' "$WORK/c"; then
  echo "package-release: refusing to package a dev-only script" >&2
  exit 1
fi

# make_zip <zip-name> <component>...
make_zip() {
  local zip="$1" stage="$WORK/z/$1" c
  shift
  mkdir -p "$stage"
  for c in "$@"; do cp -a "$WORK/c/$c/." "$stage/"; done
  rm -f "$OUT/$zip"
  (cd "$stage" && find readyup -exec touch -h -d "@$EPOCH" {} + && find readyup -type f | LC_ALL=C sort | zip -qX "$OUT/$zip" -@)
  echo "Packaged: $zip ($*)"
}

make_zip "ready-up-core-$SUFFIX" core
make_zip "ready-up-match-$SUFFIX" match
make_zip "ready-up-skins-$SUFFIX" skins
make_zip "ready-up-hello-$SUFFIX" hello
make_zip "ready-up-midas-$SUFFIX" midas
make_zip "ready-up-whitelist-$SUFFIX" whitelist
make_zip "ready-up-deathmatch-$SUFFIX" deathmatch
make_zip "ready-up-practice-$SUFFIX" practice
make_zip "ready-up-essentials-plugin-$SUFFIX" essentials
make_zip "ready-up-fleet-$SUFFIX" fleet
make_zip "ready-up-essentials-$SUFFIX" core essentials match fleet practice
full=(core essentials match fleet practice skins hello midas whitelist deathmatch)
[[ -d "$WORK/c/tools" ]] && full+=(tools)
make_zip "ready-up-full-$SUFFIX" "${full[@]}"

(cd "$OUT" && sha256sum ready-up-*-"$SUFFIX" >SHA256SUMS)
echo "SHA256SUMS:"
cat "$OUT/SHA256SUMS"
