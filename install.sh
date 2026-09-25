#!/usr/bin/env bash
# Ready Up installer and updater. Run it from your CS2 server root (the folder that contains
# game/), as the user that owns the server files:
#
#   curl -fsSL https://raw.githubusercontent.com/Auto-Tournament/ready-up/master/install.sh | bash
#   curl -fsSL https://raw.githubusercontent.com/Auto-Tournament/ready-up/master/install.sh | bash -s -- essentials
#
# In a terminal it shows a checklist of components (installed version -> latest) and
# installs, updates or removes what you tick. With a bundle/component name or --yes it
# never asks.
#
# Usage: install.sh [BUNDLE|COMPONENT ...] [options]
#
#   essentials   core + match (default for a fresh install; no skins)
#   full         core + match + skins + hello
#   core | match | skins | hello   single components (core is always included)
#
#   --dir PATH       CS2 server root (contains game/csgo) or a game/csgo directory (default: .)
#   --version vX.Y.Z install that release instead of the latest
#   --zip FILE       install from a local zip (a bundle or component zip; repeatable). A
#                    SHA256SUMS file next to the zip is used to verify it.
#   --remove NAME    remove an installed component (repeatable; not core)
#   -y, --yes        no questions: update what is installed (or install essentials)
#   --uninstall      remove Ready Up: the gameinfo.gi line and its files. Config is kept
#   --purge          with --uninstall: also delete readyup.cfg, the plugins' JSON data
#                    (admins, match state, skins loadouts) and cfg/ReadyUp
#   -h, --help       this text
#
# What it touches: game/csgo/readyup/, game/csgo/cfg/ReadyUp/ (only files that are missing;
# changed templates are written as *.default), and gameinfo.gi / gameinfo_branchspecific.gi
# (one "Game csgo/readyup" line after Metamod if present; backup gameinfo.gi.readyup-backup-*).
# It never uses sudo, stops or starts servers, or touches databases. Needs bash 4+, python3,
# curl or wget (downloads only), and unzip (else python3 extracts).
set -euo pipefail

REPO="${READYUP_REPO:-Auto-Tournament/ready-up}"
API="${READYUP_API:-https://api.github.com}"
GAME_PATH="csgo/readyup"
COMPONENTS=(core match skins hello)
declare -A LABEL=([core]="Core" [match]="Match" [skins]="Skins" [hello]="Hello")
declare -A NOTE=([core]="required" [match]="ready-up, knife, pauses, webhooks" [skins]="may get servers banned"
  [hello]="example plugin")

DIR="."
VERSION=""
ZIPS=()
WANT=()
WANT_FULL=0
REMOVE=()
YES=0
UNINSTALL=0
PURGE=0

# ---- output ---------------------------------------------------------------------------------
if [[ -t 1 && -z "${NO_COLOR:-}" && "${TERM:-dumb}" != "dumb" ]]; then
  B=$'\e[1m' D=$'\e[2m' G=$'\e[32m' Y=$'\e[33m' R=$'\e[31m' N=$'\e[0m'
else
  B="" D="" G="" Y="" R="" N=""
fi
say() { printf '%s\n' "$*"; }
ok() { printf '%s✓%s %s\n' "$G" "$N" "$*"; }
warn() { printf '%s!%s %s\n' "$Y" "$N" "$*" >&2; }
die() {
  printf '%serror:%s %s\n' "$R" "$N" "$*" >&2
  exit 1
}
usage() { sed -n '2,/^set -euo/p' "$0" 2>/dev/null | sed '$d; s/^# \{0,1\}//' || true; }

# ---- arguments ------------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dir) DIR="${2:?--dir needs a path}"; shift 2 ;;
    --version) VERSION="${2:?--version needs vX.Y.Z}"; shift 2 ;;
    --zip) ZIPS+=("${2:?--zip needs a file}"); shift 2 ;;
    --remove) REMOVE+=("${2:?--remove needs a component}"); shift 2 ;;
    -y | --yes) YES=1; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    --purge) PURGE=1; shift ;;
    -h | --help)
      if [[ -f "$0" ]]; then usage; else say "See https://github.com/$REPO#install"; fi
      exit 0
      ;;
    essentials) WANT+=(core match); shift ;;
    full) WANT+=(core match skins hello); WANT_FULL=1; shift ;;
    core | match | skins | hello) WANT+=("$1"); shift ;;
    *) die "unknown argument: $1 (see --help)" ;;
  esac
done
[[ $PURGE -eq 0 || $UNINSTALL -eq 1 ]] || die "--purge only goes with --uninstall"
for c in "${REMOVE[@]}"; do
  case "$c" in match | skins | hello) ;; core) die "core can't be removed on its own; use --uninstall" ;; *) die "unknown component: $c" ;; esac
done

# ---- requirements ---------------------------------------------------------------------------
((BASH_VERSINFO[0] >= 4)) || die "bash 4 or newer is required"
command -v python3 >/dev/null 2>&1 || die "python3 is required (it edits gameinfo.gi and reads the manifests). Install python3 and run this again."

have() { command -v "$1" >/dev/null 2>&1; }

fetch() {  # <url> <out-file>
  if have curl; then
    curl -fsSL --retry 2 -H "User-Agent: ready-up-installer" -o "$2" "$1"
  elif have wget; then
    wget -q --header "User-Agent: ready-up-installer" -O "$2" "$1"
  else
    die "curl or wget is needed to download releases (or use --zip with a local file)"
  fi
}

unzip_to() {  # <zip> <dir>
  mkdir -p "$2"
  if have unzip; then
    unzip -qo "$1" -d "$2"
  else
    python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$1" "$2"
  fi
}

# ---- locate game/csgo -------------------------------------------------------------------------
if [[ -f "$DIR/game/csgo/gameinfo.gi" ]]; then
  CSGO="$(cd "$DIR/game/csgo" && pwd)"
elif [[ -f "$DIR/gameinfo.gi" && "$(basename "$(cd "$DIR" && pwd)")" == "csgo" ]]; then
  CSGO="$(cd "$DIR" && pwd)"
else
  die "$(cd "$DIR" 2>/dev/null && pwd || echo "$DIR") is not a CS2 server root (no game/csgo/gameinfo.gi).
       cd to the folder that contains game/ and run this again, or pass --dir /path/to/cs2."
fi
[[ -w "$CSGO" ]] || die "$CSGO is not writable by $(id -un); run this as the user that owns the server files"
RU="$CSGO/readyup"
BIN="$RU/bin/linuxsteamrt64"
STATE="$RU/installed.json"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ready-up-install.XXXXXX")"
TUI_ACTIVE=0
cleanup() {
  if [[ $TUI_ACTIVE -eq 1 ]]; then printf '\e[?25h' >&3 || true; fi  # cursor back if we hid it
  rm -rf "$WORK"
}
trap cleanup EXIT

# ---- installed state ----------------------------------------------------------------------------
declare -A INSTALLED=()
read_state() {
  INSTALLED=()
  [[ -f "$STATE" ]] || return 0
  local c v
  while read -r c v; do
    [[ -n "$c" ]] && INSTALLED[$c]="$v"
  done < <(python3 -c 'import json,sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    d = {}
for k, v in sorted(d.get("components", {}).items()):
    print(k, v)' "$STATE")
}
write_state() {
  local args=() c
  for c in "${!INSTALLED[@]}"; do args+=("$c=${INSTALLED[$c]}"); done
  mkdir -p "$RU"
  python3 - "$STATE" "${args[@]}" <<'PY'
import json, sys, datetime
path, *pairs = sys.argv[1:]
comps = dict(p.split("=", 1) for p in pairs)
json.dump({"components": dict(sorted(comps.items())),
           "updated": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")},
          open(path + ".tmp", "w"), indent=2)
open(path + ".tmp", "a").write("\n")
PY
  mv -f "$STATE.tmp" "$STATE"
}
read_state

manifest_files() {  # <manifest.json> -> one path per line
  python3 -c 'import json,sys; print("\n".join(json.load(open(sys.argv[1]))["files"]))' "$1"
}
manifest_field() {  # <manifest.json> <field>
  python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get(sys.argv[2], ""))' "$1" "$2"
}

# ---- gameinfo -------------------------------------------------------------------------------------
patcher() {
  local p="$RU/tools/patch_gameinfo.py"
  [[ -f "$p" ]] || p="$WORK/patch_gameinfo.py"
  [[ -f "$p" ]] || die "patch_gameinfo.py not found (install core first)"
  python3 "$p" "$@"
}

patch_all_gameinfo() {  # [--remove]
  local gi out
  for gi in "$CSGO/gameinfo.gi" "$CSGO/gameinfo_branchspecific.gi"; do
    [[ -f "$gi" ]] || continue
    if ! out="$(patcher "$gi" --game "$GAME_PATH" "$@" 2>&1)"; then
      if [[ "$gi" == */gameinfo_branchspecific.gi && "$out" == Skipped* ]]; then
        say "  ${D}gameinfo_branchspecific.gi has no SearchPaths; nothing to patch there${N}"
      elif [[ "$gi" == */gameinfo.gi ]]; then
        die "could not patch gameinfo.gi: $out
       Add \"Game $GAME_PATH\" above \"Game csgo\" (below Metamod's line) by hand."
      else
        warn "$(basename "$gi"): $out"
      fi
      continue
    fi
    case "$out" in
      Patched*)
        # Backup names end in a sortable timestamp; the newest is the one just written.
        ok "$(basename "$gi") patched (backup: $(find "$CSGO" -maxdepth 1 -name "$(basename "$gi").readyup-backup-*" -printf '%f\n' | sort | tail -n1))"
        ;;
      Removed*) ok "$(basename "$gi"): Ready Up line removed" ;;
      Already*) ok "$(basename "$gi") already set up" ;;
      Not\ present*) ok "$(basename "$gi"): no Ready Up line" ;;
      *) say "  $out" ;;
    esac
  done
}

# ---- uninstall ------------------------------------------------------------------------------------
PROTECTED_RE='^readyup/bin/linuxsteamrt64/readyup\.cfg$'

remove_component() {  # <component>
  local c="$1" m="$RU/manifests/$1.json" f
  if [[ -f "$m" ]]; then
    while IFS= read -r f; do
      [[ -z "$f" || "$f" =~ $PROTECTED_RE ]] && continue
      rm -f "$CSGO/$f" "$CSGO/$f.prev" "$CSGO/$f.readyup-new"
    done < <(manifest_files "$m")
    rm -f "$m"
  fi
  unset "INSTALLED[$c]"
}

if [[ $UNINSTALL -eq 1 ]]; then
  say "${B}Ready Up uninstall${N} ${D}($CSGO)${N}"
  [[ -f "$RU/tools/patch_gameinfo.py" ]] && cp "$RU/tools/patch_gameinfo.py" "$WORK/patch_gameinfo.py"
  if [[ -f "$WORK/patch_gameinfo.py" ]]; then
    patch_all_gameinfo --remove
  else
    warn "patch_gameinfo.py is missing; remove the \"Game $GAME_PATH\" line from gameinfo.gi by hand"
  fi
  for c in hello skins match core; do
    [[ -n "${INSTALLED[$c]:-}" || -f "$RU/manifests/$c.json" ]] || continue
    remove_component "$c"
    ok "removed $c"
  done
  [[ -f "$RU/manifests/tools.json" ]] && remove_component tools
  rm -f "$STATE"
  if [[ $PURGE -eq 1 ]]; then
    rm -rf "$RU" "$CSGO/cfg/ReadyUp"
    ok "deleted $RU and $CSGO/cfg/ReadyUp"
  else
    find "$RU" -depth -type d -empty -delete 2>/dev/null || true
    [[ -d "$RU" ]] && say "  kept your config and data in $RU (readyup.cfg, plugins/*/*.json) and $CSGO/cfg/ReadyUp; --purge deletes it"
  fi
  say ""
  say "Restart the server to finish. CS2 then loads its own libserver.so again."
  exit 0
fi

# ---- where the files come from ----------------------------------------------------------------
declare -A AVAIL=()     # component -> version available
declare -A ASSET_URL=() # component -> download url (release mode)
declare -A STAGED=()    # component -> staged manifest path
RELEASE_TAG=""
RELEASE_NOTES=""
SUMS=""

stage_zip() {  # <zip> : extract and register every component manifest inside
  local zip="$1" dir m c
  dir="$WORK/stage/$(basename "$zip" .zip)"
  unzip_to "$zip" "$dir"
  local found=0
  for m in "$dir"/readyup/manifests/*.json; do
    [[ -f "$m" ]] || continue
    c="$(basename "$m" .json)"
    STAGED[$c]="$m"
    AVAIL[$c]="$(manifest_field "$m" version)"
    found=1
  done
  [[ $found -eq 1 ]] || die "$zip is not a Ready Up zip (no readyup/manifests/*.json inside)"
}

verify_sum() {  # <file> <sums-file> : 0 ok, 1 mismatch, 2 not listed
  local name want got
  name="$(basename "$1")"
  want="$(awk -v n="$name" '$2 == n || $2 == "*"n {print $1}' "$2" | head -n1)"
  [[ -n "$want" ]] || return 2
  got="$(sha256sum "$1" | cut -d' ' -f1)"
  [[ "$want" == "$got" ]]
}

if [[ ${#REMOVE[@]} -gt 0 && ${#WANT[@]} -eq 0 && ${#ZIPS[@]} -eq 0 ]]; then
  :  # removing only: nothing to download
elif [[ ${#ZIPS[@]} -gt 0 ]]; then
  for z in "${ZIPS[@]}"; do
    [[ -f "$z" ]] || die "no such file: $z"
    if [[ -f "$(dirname "$z")/SHA256SUMS" ]]; then
      set +e
      verify_sum "$z" "$(dirname "$z")/SHA256SUMS"
      rc=$?
      set -e
      case $rc in
        0) ok "checksum ok: $(basename "$z")" ;;
        1) die "checksum mismatch for $z (SHA256SUMS next to it disagrees)" ;;
        *) warn "$(basename "$z") is not listed in SHA256SUMS; not verified" ;;
      esac
    fi
    stage_zip "$z"
  done
else
  if [[ -n "$VERSION" ]]; then
    url="$API/repos/$REPO/releases/tags/$VERSION"
  else
    url="$API/repos/$REPO/releases/latest"
  fi
  if ! fetch "$url" "$WORK/release.json" 2>/dev/null; then
    die "no published release found at $url.
       There may be no release yet: download the CI artifact zips and use --zip ready-up-essentials-*.zip"
  fi
  while IFS=$'\t' read -r kind a b; do
    case "$kind" in
      tag) RELEASE_TAG="$a" ;;
      asset) c="${a#ready-up-}"; c="${c%%-*}"; ASSET_URL[$c]="$b"; AVAIL[$c]="${RELEASE_TAG#v}" ;;
      sums) SUMS="$b" ;;
    esac
  done < <(python3 - "$WORK/release.json" "$WORK/notes.txt" <<'PY'
import json, re, sys
rel = json.load(open(sys.argv[1]))
open(sys.argv[2], "w").write(rel.get("body") or "")
print("tag\t%s\t" % rel.get("tag_name", ""))
for a in rel.get("assets", []):
    name, url = a.get("name", ""), a.get("browser_download_url", "")
    if name == "SHA256SUMS":
        print("sums\t%s\t%s" % (name, url))
    elif re.match(r"^ready-up-(core|match|skins|hello)-.*\.zip$", name):
        print("asset\t%s\t%s" % (name, url))
PY
  )
  RELEASE_NOTES="$(cat "$WORK/notes.txt")"
  [[ ${#ASSET_URL[@]} -gt 0 ]] || die "release ${RELEASE_TAG:-?} has no ready-up-<component> zips"
  if [[ -n "$SUMS" ]]; then fetch "$SUMS" "$WORK/SHA256SUMS"; fi
fi

ensure_staged() {  # <component> (release mode: download + verify + extract on demand)
  local c="$1" url zip
  [[ -n "${STAGED[$c]:-}" ]] && return 0
  url="${ASSET_URL[$c]:-}"
  [[ -n "$url" ]] || die "$c is not available from ${RELEASE_TAG:-this source}"
  zip="$WORK/$(basename "$url")"
  fetch "$url" "$zip" || die "download failed: $url"
  if [[ -f "$WORK/SHA256SUMS" ]]; then
    verify_sum "$zip" "$WORK/SHA256SUMS" || die "checksum mismatch for $(basename "$zip")"
  fi
  stage_zip "$zip"
  [[ -n "${STAGED[$c]:-}" ]] || die "$(basename "$zip") does not contain the $c component"
}

# ---- choose -----------------------------------------------------------------------------------
declare -A SEL=()
for c in "${COMPONENTS[@]}"; do
  [[ -n "${INSTALLED[$c]:-}" ]] && SEL[$c]=1 || SEL[$c]=0
done
if [[ ${#INSTALLED[@]} -eq 0 ]]; then SEL[core]=1 SEL[match]=1; fi
SEL[core]=1

TTY_OK=0
if [[ $YES -eq 0 && ${#WANT[@]} -eq 0 && ${#REMOVE[@]} -eq 0 ]] && (exec 3<>/dev/tty) 2>/dev/null; then
  exec 3<>/dev/tty
  TTY_OK=1
fi

row_versions() {  # <component> -> "installed -> latest" text
  local c="$1" have="${INSTALLED[$1]:-}" avail="${AVAIL[$1]:-}"
  if [[ -z "$avail" ]]; then
    printf '%s' "${have:-not installed}  (not in this release)"
  elif [[ -z "$have" ]]; then
    printf 'new %s' "$avail"
  elif [[ "$have" == "$avail" ]]; then
    printf '%s (latest)' "$have"
  else
    printf '%s -> %s' "$have" "$avail"
  fi
}

print_rows() {  # <cursor index or -1>
  local i c mark ptr
  for i in "${!COMPONENTS[@]}"; do
    c="${COMPONENTS[$i]}"
    [[ "${SEL[$c]}" == 1 ]] && mark="[x]" || mark="[ ]"
    [[ "$i" == "$1" ]] && ptr=">" || ptr=" "
    printf '\e[2K%s %s %-6s %-28s %s\n' "$ptr" "$mark" "${LABEL[$c]}" "$(row_versions "$c")" "${D}${NOTE[$c]}${N}" >&3
  done
}

tui_select() {
  local n=${#COMPONENTS[@]} cur=0 key rest
  say "${D}↑/↓ move, space toggles, enter confirms, q quits${N}" >&3
  TUI_ACTIVE=1
  printf '\e[?25l' >&3
  print_rows "$cur"
  while :; do
    IFS= read -rsn1 key <&3 || key="q"
    case "$key" in
      $'\e')
        rest=""
        IFS= read -rsn2 -t 1 rest <&3 || true
        case "$rest" in '[A') ((cur > 0)) && cur=$((cur - 1)) ;; '[B') ((cur < n - 1)) && cur=$((cur + 1)) ;; esac
        ;;
      k) ((cur > 0)) && cur=$((cur - 1)) ;;
      j) ((cur < n - 1)) && cur=$((cur + 1)) ;;
      ' ')
        local c="${COMPONENTS[$cur]}"
        if [[ "$c" == core ]]; then :; elif [[ "${SEL[$c]}" == 1 ]]; then SEL[$c]=0; else SEL[$c]=1; fi
        ;;
      '') break ;;
      q | Q) printf '\e[?25h' >&3; say "Nothing changed." >&3; exit 0 ;;
    esac
    printf '\e[%dA' "$n" >&3
    print_rows "$cur"
  done
  printf '\e[?25h' >&3
  TUI_ACTIVE=0
}

plain_select() {
  local i c line cur=()
  for i in "${!COMPONENTS[@]}"; do
    c="${COMPONENTS[$i]}"
    printf '  %d) %-6s %-28s %s\n' "$((i + 1))" "${LABEL[$c]}" "$(row_versions "$c")" "${NOTE[$c]}" >&3
    [[ "${SEL[$c]}" == 1 ]] && cur+=("$((i + 1))")
  done
  local def
  def="$(IFS=,; echo "${cur[*]}")"
  while :; do
    printf 'Components to install (e.g. 1,2) [%s]: ' "$def" >&3
    IFS= read -r line <&3 || line=""
    line="${line// /}"
    [[ -z "$line" ]] && line="$def"
    local okk=1 n
    for c in "${COMPONENTS[@]}"; do SEL[$c]=0; done
    IFS=, read -ra picks <<<"$line"
    for n in "${picks[@]}"; do
      if [[ "$n" =~ ^[0-9]+$ ]] && ((n >= 1 && n <= ${#COMPONENTS[@]})); then
        SEL[${COMPONENTS[$((n - 1))]}]=1
      else
        okk=0
      fi
    done
    SEL[core]=1
    [[ $okk -eq 1 ]] && break
    say "  please enter numbers from the list, separated by commas" >&3
  done
}

TO_INSTALL=()
TO_REMOVE=()
if [[ $TTY_OK -eq 1 ]]; then
  say "${B}Ready Up${N} ${D}installer · $CSGO${N}" >&3
  if [[ "${TERM:-dumb}" == "dumb" || -n "${READYUP_PLAIN:-}" ]]; then plain_select; else tui_select; fi
  for c in "${COMPONENTS[@]}"; do
    if [[ "${SEL[$c]}" == 1 ]]; then
      TO_INSTALL+=("$c")
    elif [[ -n "${INSTALLED[$c]:-}" ]]; then
      TO_REMOVE+=("$c")
    fi
  done
  prompt="Install/update ${#TO_INSTALL[@]} component(s)"
  [[ ${#TO_REMOVE[@]} -gt 0 ]] && prompt+=", remove ${#TO_REMOVE[@]} (${TO_REMOVE[*]})"
  printf '%s? [Y/n] ' "$prompt" >&3
  IFS= read -r answer <&3 || answer="n"
  case "$answer" in "" | y | Y | yes | YES) ;; *) say "Nothing changed." >&3; exit 0 ;; esac
else
  declare -A pick=()
  if [[ ${#WANT[@]} -gt 0 ]]; then
    for c in "${WANT[@]}"; do pick[$c]=1; done
  elif [[ ${#REMOVE[@]} -eq 0 ]]; then
    for c in "${COMPONENTS[@]}"; do [[ "${SEL[$c]}" == 1 ]] && pick[$c]=1; done
  fi
  if [[ ${#pick[@]} -gt 0 || ${#INSTALLED[@]} -eq 0 ]]; then pick[core]=1; fi
  for c in "${COMPONENTS[@]}"; do [[ -n "${pick[$c]:-}" ]] && TO_INSTALL+=("$c"); done
  TO_REMOVE=("${REMOVE[@]}")
  say "${B}Ready Up${N} ${D}installer · $CSGO${N}"
fi

for c in "${TO_INSTALL[@]}"; do
  [[ -n "${AVAIL[$c]:-}" ]] || die "$c is not in ${RELEASE_TAG:-the given zip(s)}; nothing was changed"
done
for c in "${TO_REMOVE[@]}"; do
  [[ "$c" != core ]] || die "core can't be removed on its own; use --uninstall"
done

# ---- install ----------------------------------------------------------------------------------
install_file() {  # <src> <dst> ; atomic replace (a running server keeps its mapped copy)
  local src="$1" dst="$2"
  mkdir -p "$(dirname "$dst")"
  if [[ -f "$dst" ]] && cmp -s "$src" "$dst"; then return 0; fi
  if [[ -f "$dst" && ( "$dst" == *.so ) ]]; then cp -p "$dst" "$dst.prev"; fi
  cp "$src" "$dst.readyup-new"
  chmod --reference="$src" "$dst.readyup-new" 2>/dev/null || true
  mv -f "$dst.readyup-new" "$dst"
}

# Keep user-owned copies; offer the new default next to them when the shipped one changed.
seed_user_file() {  # <new-template> <old-template-or-empty> <user-file>
  local new="$1" old="$2" user="$3"
  if [[ ! -f "$user" ]]; then
    mkdir -p "$(dirname "$user")"
    cp "$new" "$user"
    say "  + ${user#"$CSGO"/}"
  elif ! cmp -s "$new" "$user" && { [[ -z "$old" ]] || ! cmp -s "$new" "$old"; }; then
    cp "$new" "$user.default"
    say "  ~ kept ${user#"$CSGO"/}; the new default is in $(basename "$user").default"
  fi
}

install_component() {  # <component>
  local c="$1" m root f old_tpl
  ensure_staged "$c"
  m="${STAGED[$c]}"
  root="$(dirname "$(dirname "$(dirname "$m")")")"  # <stage>/readyup/manifests/x.json -> <stage>
  while IFS= read -r f; do
    [[ -n "$f" ]] || continue
    old_tpl=""
    case "$f" in
      readyup/cfg-templates/ReadyUp/*.cfg | readyup/bin/linuxsteamrt64/readyup.cfg.example)
        [[ -f "$CSGO/$f" ]] && { old_tpl="$WORK/old-$(basename "$f")"; cp "$CSGO/$f" "$old_tpl"; }
        ;;
    esac
    install_file "$root/$f" "$CSGO/$f"
    case "$f" in
      readyup/cfg-templates/ReadyUp/*.cfg)
        seed_user_file "$root/$f" "$old_tpl" "$CSGO/cfg/ReadyUp/$(basename "$f")" ;;
      readyup/bin/linuxsteamrt64/readyup.cfg.example)
        seed_user_file "$root/$f" "$old_tpl" "$BIN/readyup.cfg" ;;
    esac
  done < <(manifest_files "$m")
  # Files the previous version shipped but this one does not.
  if [[ -f "$RU/manifests/$c.json" ]]; then
    comm -23 <(manifest_files "$RU/manifests/$c.json" | sort) <(manifest_files "$m" | sort) | while IFS= read -r f; do
      [[ -n "$f" && ! "$f" =~ $PROTECTED_RE ]] && rm -f "$CSGO/$f"
    done
  fi
  mkdir -p "$RU/manifests"
  cp "$m" "$RU/manifests/$c.json"
  INSTALLED[$c]="$(manifest_field "$m" version)"
}

declare -A BEFORE=()
for c in "${!INSTALLED[@]}"; do BEFORE[$c]="${INSTALLED[$c]}"; done

# Core first (the patcher and the plugin host come with it).
ordered=()
for c in core match skins hello; do
  for t in "${TO_INSTALL[@]}"; do [[ "$t" == "$c" ]] && ordered+=("$c"); done
done
for c in "${ordered[@]}"; do
  install_component "$c"
  old="${BEFORE[$c]:-}"
  new="${INSTALLED[$c]}"
  if [[ -z "$old" ]]; then
    ok "$c $new installed"
  elif [[ "$old" == "$new" ]]; then
    ok "$c $new (already up to date)"
  else
    ok "$c $old → $new"
  fi
done
# The full bundle also carries the offline checkers: installed with `full`, kept up to date after.
if [[ -n "${STAGED[tools]:-}" ]] && [[ $WANT_FULL -eq 1 || -n "${INSTALLED[tools]:-}" ]]; then
  install_component tools
fi

for c in "${TO_REMOVE[@]}"; do
  if [[ -n "${INSTALLED[$c]:-}" || -f "$RU/manifests/$c.json" ]]; then
    remove_component "$c"
    ok "$c removed"
  fi
done

write_state
patch_all_gameinfo
if [[ -f "$BIN/readyup_db.json" && ! -f "$RU/plugins/match/admins.json" ]]; then
  warn "readyup_db.json found: Ready Up no longer uses Postgres. Copy admins, settings and skins into JSON once:"
  say "    python3 $RU/tools/migrate-postgres-to-json.py --csgo $CSGO   (INSTALL.md, \"Upgrading from Postgres\")"
elif [[ ! -f "$RU/plugins/match/admins.json" ]]; then
  say "  ${D}no admins yet: run ${N}ru admins add <steamid64>${D} in the server console (ADMINS.md)${N}"
fi

if [[ -n "$RELEASE_NOTES" ]]; then
  say ""
  say "${B}Release notes ($RELEASE_TAG)${N}"
  printf '%s\n' "$RELEASE_NOTES" | sed -n '1,12p' | sed 's/^/  /'
  say "  https://github.com/$REPO/releases/tag/$RELEASE_TAG"
fi
say ""
say "${B}Next:${N} restart the server, then run ${B}ru selftest${N} in its console (expect PASS)."
say "Run this installer again at any time to update or change components."
