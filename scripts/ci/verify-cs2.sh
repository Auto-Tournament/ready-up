#!/usr/bin/env bash
# Verify gamedata/engine-surface.json against a fetched CS2 build:
#   - readyup_sigcheck: every function resolves uniquely + passes its identity anchors,
#                       every RTTI name still exists
#   - readyup_hookcheck: every "hook": "funchook" site's prologue can be relocated
#
#   scripts/ci/verify-cs2.sh <cs2-dir> <tools-dir> [report.md]
#
# <cs2-dir> is the output of fetch-cs2-binaries.sh, <tools-dir> holds readyup_sigcheck and
# readyup_hookcheck. Writes a markdown report (default <cs2-dir>/report.md) and exits 1 if
# any check fails. With VERIFY_RAW_DIR set, the raw checker output is kept there
# (sigcheck.txt, hookcheck.txt, rc.env) for scripts/ci/compat-report.py.
set -euo pipefail

CS2="${1:?usage: $0 <cs2-dir> <tools-dir> [report.md]}"
TOOLS="${2:?usage: $0 <cs2-dir> <tools-dir> [report.md]}"
REPORT="${3:-$CS2/report.md}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SURFACE="${SURFACE:-$ROOT_DIR/gamedata/engine-surface.json}"
# Plugin gamedata fragments (engine-surface.<plugin>.json, e.g. skins) are merged on top of the
# base file, exactly as the core does at load. Every fragment in gamedata/ is verified.
shopt -s nullglob
FRAGMENTS=("$(dirname "$SURFACE")"/engine-surface.*.json)
shopt -u nullglob
LIB="$CS2/game/csgo/bin/linuxsteamrt64/libserver.so"

BUILDID="" PATCH_VERSION="" SERVER_VERSION=""
if [[ -f "$CS2/cs2-build.env" ]]; then
  # shellcheck disable=SC1091
  source "$CS2/cs2-build.env"
fi

sig_out="$(mktemp)" hook_out="$(mktemp)"
trap 'rm -f "$sig_out" "$hook_out"' EXIT
set +e
"$TOOLS/readyup_sigcheck" "$LIB" "$SURFACE" "${FRAGMENTS[@]}" >"$sig_out" 2>&1
sig_rc=$?
"$TOOLS/readyup_hookcheck" "$LIB" "$SURFACE" "${FRAGMENTS[@]}" >"$hook_out" 2>&1
hook_rc=$?
set -e
cat "$sig_out" "$hook_out"
if [[ -n "${VERIFY_RAW_DIR:-}" ]]; then
  mkdir -p "$VERIFY_RAW_DIR"
  cp "$sig_out" "$VERIFY_RAW_DIR/sigcheck.txt"
  cp "$hook_out" "$VERIFY_RAW_DIR/hookcheck.txt"
  printf 'SIGCHECK_RC=%d\nHOOKCHECK_RC=%d\n' "$sig_rc" "$hook_rc" >"$VERIFY_RAW_DIR/rc.env"
fi

python3 - "$sig_out" "$hook_out" "$sig_rc" "$hook_rc" "$SURFACE" \
  "${BUILDID:-?}" "${PATCH_VERSION:-?}" "${SERVER_VERSION:-?}"   "$(basename "$SURFACE")" "${FRAGMENTS[@]##*/}" >"$REPORT" <<'PY'
import json, re, sys

sig_path, hook_path, sig_rc, hook_rc, surface_path, buildid, patch, server = sys.argv[1:9]
sig_rc, hook_rc = int(sig_rc), int(hook_rc)
meta = json.load(open(surface_path)).get("_meta", {})

def cell(s):
    s = s.strip().replace("|", "\\|").replace("\n", " ")
    return s if len(s) <= 180 else s[:177] + "..."

rows = []
for line in open(sig_path, errors="replace"):
    m = re.match(r"^(OK|FAIL)\s+rtti\s+(\S+)\s+(.*)$", line)
    if m:
        rows.append(("rtti", m.group(2), m.group(1), m.group(3)))
        continue
    m = re.match(r"^(OK|FAIL)\s+(\S+)\s+(required|optional)\s+(matches=\d+)\s+(rva=\S+)\s*(.*)$", line)
    if m:
        rows.append(("signature (" + m.group(3) + ")", m.group(2), m.group(1),
                     "%s %s %s" % (m.group(4), m.group(5), m.group(6))))
for line in open(hook_path, errors="replace"):
    m = re.match(r"^(OK|FAIL)\s+(\S+)\s+(.*)$", line)
    if m:
        rows.append(("hook site", m.group(2), m.group(1), m.group(3)))

ok = sig_rc == 0 and hook_rc == 0
fails = [r for r in rows if r[2] != "OK"]
print("## %s CS2 build %s: engine surface %s" % ("✅" if ok else "❌", buildid, "verified" if ok else "BROKEN"))
print()
print("| | |")
print("|---|---|")
print("| Steam buildid | `%s` |" % buildid)
print("| PatchVersion / ServerVersion | `%s` / `%s` |" % (patch, server))
print("| engine-surface.json written for | `%s` (buildid `%s`) |" % (meta.get("game_version", "?"), meta.get("steam_buildid", "?")))
print("| gamedata files | %s |" % ", ".join("`%s`" % f for f in sys.argv[9:]))
print("| readyup_sigcheck | %s (exit %d) |" % ("pass" if sig_rc == 0 else "**FAIL**", sig_rc))
print("| readyup_hookcheck | %s (exit %d) |" % ("pass" if hook_rc == 0 else "**FAIL**", hook_rc))
print()
if fails:
    print("### Failures")
    print()
    print("| Check | Entry | Result | Detail |")
    print("|---|---|---|---|")
    for kind, name, res, detail in fails:
        print("| %s | `%s` | **%s** | %s |" % (kind, name, res, cell(detail)))
    print()
if sig_rc not in (0, 1) or hook_rc not in (0, 1) or (not ok and not fails):
    print("> A checker exited abnormally; see the job log for its raw output.")
    print()
print("<details><summary>All checks (%d)</summary>" % len(rows))
print()
print("| Check | Entry | Result | Detail |")
print("|---|---|---|---|")
for kind, name, res, detail in rows:
    print("| %s | `%s` | %s | %s |" % (kind, name, res, cell(detail)))
print()
print("</details>")
if not ok:
    print()
    print("Fix: update `gamedata/engine-surface.json` or the failing fragment (signatures / anchors), then re-run "
          "`build/readyup_sigcheck <libserver.so> gamedata/engine-surface.json gamedata/engine-surface.skins.json` and "
          "`build/readyup_hookcheck` with the same files. "
          "`scripts/ci/fetch-cs2-binaries.sh <dir>` downloads just the needed CS2 files.")
PY

echo "report: $REPORT"
if [[ $sig_rc -ne 0 || $hook_rc -ne 0 ]]; then
  exit 1
fi
