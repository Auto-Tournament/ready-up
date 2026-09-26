#!/usr/bin/env python3
"""CS2 compatibility report for .github/workflows/cs2-update-watch.yml (python3 stdlib only).

Turns the raw output of readyup_sigcheck / readyup_hookcheck (scripts/ci/verify-cs2.sh with
VERIFY_RAW_DIR set) into per-component results and writes compat.json + badge.json. The JSON
contract is documented in docs/CS2-COMPAT.md; the Auto Tournament platform builds against it,
so change it only together with `schema`.

  compat-report.py result --raw-dir DIR --gamedata DIR --build-env FILE --out-dir DIR [run options]
      static verdict -> DIR/compat.json + DIR/badge.json
  compat-report.py phase {queued,checking,no_verdict} --out FILE [--build-env FILE] [run options]
      progress event (same shape, no checks yet) -> FILE
  compat-report.py post FILE
      POST FILE to $COMPAT_INGEST_URL with "Authorization: Bearer $COMPAT_INGEST_TOKEN".
      Silently skipped when the URL is unset; never exits non-zero (failures are logged).
  compat-report.py dynamic --stage {selftest,live} --base FILE --selftest FILE [--livetest NAME=RC ...]
                   --out-dir DIR [run options]
      dynamic verdict (.github/workflows/cs2-dynamic.yml): the static compat.json plus the
      readyup_selftest.txt of a real server (plugin needs, plugin selftest lines) and, for
      stage live, the bot live test exit codes -> DIR/compat.json + DIR/badge.json
  compat-report.py needs-check [--plugins-dir DIR] [--gamedata DIR]
      every plugin's needs.json covers what its source calls (schema_offset literals,
      subscribe_game_event names, engine-facing ru_api members); exit 1 on drift
  compat-report.py needs-derive NAME
      print the needs.json the source of plugins/NAME implies (a starting point for a new plugin)

Run options (defaults from the GitHub Actions environment):
  --run-id, --run-url, --trigger, --started-at, --commit, --version-file, --buildid
"""
import argparse
import datetime
import glob
import json
import os
import re
import sys
import urllib.error
import urllib.request

SCHEMA = 1

# Components with an engine-surface file. The base file is the core; every
# engine-surface.<id>.json fragment is its plugin's own component.
CORE_ID = "core"
# Plugins without an engine-surface fragment, in display order. Each one's static verdict comes
# from its plugins/<id>/needs.json: the engine-surface entries its ru_api calls need.
RUNTIME_ONLY = ["match", "practice", "essentials", "midas", "whitelist", "deathmatch", "fleet"]
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# Plugins whose manifest is checked (needs-check) and used for verdicts. hello is the example.
NEEDS_PLUGINS = ["match", "practice", "essentials", "midas", "whitelist", "deathmatch", "fleet", "skins"]
NAMES = {"core": "Core", "skins": "Skins", "match": "Match", "practice": "Practice",
         "essentials": "Essentials", "midas": "Midas", "whitelist": "Whitelist", "deathmatch": "Deathmatch",
         "fleet": "Fleet"}
# Order of static check kinds inside a component.
STATIC_KINDS = ["signature", "rtti", "vtable", "hook_site", "layout"]
TRIGGERS = ("build_change", "surface_change", "nightly", "release", "manual")
BADGE = {
    "pass": ("compatible", "brightgreen"),
    "warn": ("static ok", "yellow"),
    "fail": ("incompatible", "red"),
    "checking": ("checking", "blue"),
}
MAX_FAILURE_LEN = 240


def now_iso():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def read_text(path):
    if not path or not os.path.isfile(path):
        return ""
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def read_env_file(path):
    out = {}
    for line in read_text(path).splitlines():
        line = line.strip()
        if "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def component_name(cid):
    return NAMES.get(cid, cid.replace("-", " ").replace("_", " ").title())


# ---------------------------------------------------------------------------------------------
# Surface ownership: which component owns which entry
# ---------------------------------------------------------------------------------------------

def surface_files(gamedata_dir):
    """[(component id, path)] base first, then fragments sorted by name."""
    base = os.path.join(gamedata_dir, "engine-surface.json")
    files = [(CORE_ID, base)] if os.path.isfile(base) else []
    for path in sorted(glob.glob(os.path.join(gamedata_dir, "engine-surface.*.json"))):
        cid = os.path.basename(path)[len("engine-surface."):-len(".json")]
        if cid:
            files.append((cid, path))
    return files


def ownership(files):
    """{(kind, name): component id}, plus {function name: required?}."""
    owner, hooked = {}, set()
    for cid, path in files:
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
        for name, fn in (d.get("functions") or {}).items():
            owner[("signature", name)] = cid
            if isinstance(fn, dict) and fn.get("hook") == "funchook":
                owner[("hook_site", name)] = cid
                hooked.add(name)
        for name in (d.get("rtti") or {}):
            owner[("rtti", name)] = cid
        for name in (d.get("vtable_indices") or {}):
            owner[("vtable", name)] = cid
        for name in (d.get("layouts") or {}):
            owner[("layout", name)] = cid
    return owner, hooked


# ---------------------------------------------------------------------------------------------
# Checker output parsing (formats: tools/sigcheck.cpp, tools/hookcheck.cpp)
# ---------------------------------------------------------------------------------------------

SIG_FUNC = re.compile(r"^(OK|FAIL)\s+(\S+)\s+(required|optional)\s+matches=(-?\d+)\s+rva=(\S+)\s*(.*)$")
SIG_OTHER = re.compile(r"^(OK|FAIL|SKIP)\s+(rtti|vtable|layout)\s+(\S+)\s*(.*)$")
HOOK_LINE = re.compile(r"^(OK|FAIL)\s+(\S+)\s+(.*)$")


def parse_sigcheck(text):
    """[(kind, name, ok, detail)] ; SKIP lines (runtime-only modules) are left out."""
    rows = []
    for line in text.splitlines():
        line = line.rstrip()
        m = SIG_OTHER.match(line)
        if m:
            if m.group(1) != "SKIP":
                rows.append((m.group(2), m.group(3), m.group(1) == "OK", m.group(4).strip()))
            continue
        m = SIG_FUNC.match(line)
        if m:
            detail = "%s matches=%s %s" % (m.group(3), m.group(4), m.group(6).strip())
            rows.append(("signature", m.group(2), m.group(1) == "OK", detail.strip()))
    return rows


def parse_hookcheck(text):
    rows = []
    for line in text.splitlines():
        m = HOOK_LINE.match(line.rstrip())
        if m:
            rows.append(("hook_site", m.group(2), m.group(1) == "OK", m.group(3).strip()))
    return rows


def short(s):
    s = " ".join(s.split())
    return s if len(s) <= MAX_FAILURE_LEN else s[:MAX_FAILURE_LEN - 3] + "..."


# ---------------------------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------------------------

def pending_check(kind="selftest"):
    return {"kind": kind, "status": "pending", "passed": 0, "total": 0, "failures": []}


def component_status(checks):
    """Worst of the finished checks; pending only if nothing has run yet."""
    done = [c["status"] for c in checks if c["status"] != "pending"]
    if not done:
        return "pending"
    for s in ("fail", "warn"):
        if s in done:
            return s
    return "pass"


def overall_status(components):
    statuses = [c["status"] for c in components]
    if "fail" in statuses:
        return "fail"
    if all(c["status"] == "pass" and all(k["status"] == "pass" for k in c["checks"]) for c in components):
        return "pass"
    # Static checks passed, dynamic (selftest/livetest) stages still pending.
    return "warn"


def build_components(sig_text, hook_text, sig_rc, hook_rc, files, needs=None):
    """needs: {plugin id: needs.json} (load_needs). A plugin's static checks are the rows of the
    engine-surface entries it needs (also counted for the entry's owner, usually core)."""
    needs = needs or {}
    owner, _ = ownership(files)
    rows = parse_sigcheck(sig_text) + parse_hookcheck(hook_text)
    static_ids = [cid for cid, _ in files] or [CORE_ID]
    plugin_ids = [c for c in RUNTIME_ONLY if c not in static_ids]
    per = {cid: {k: {"passed": 0, "total": 0, "failures": []} for k in STATIC_KINDS}
           for cid in static_ids + plugin_ids}
    wanted = {cid: set(n.get("surface") or []) for cid, n in needs.items() if cid in per}

    for kind, name, ok, detail in rows:
        cid = owner.get((kind, name), CORE_ID)
        if cid not in per:
            cid = CORE_ID
        targets = [cid] + [p for p, names in wanted.items() if name in names and p != cid]
        for t in targets:
            bucket = per[t][kind]
            bucket["total"] += 1
            if ok:
                bucket["passed"] += 1
            else:
                bucket["failures"].append(short("%s: %s" % (name, detail)))

    # A checker that failed without a parsable FAIL line (parse error, crash, no hook sites):
    # never let that read as a pass.
    for tool, rc, kind in (("readyup_sigcheck", sig_rc, "signature"), ("readyup_hookcheck", hook_rc, "hook_site")):
        tool_kinds = ["hook_site"] if kind == "hook_site" else ["signature", "rtti", "vtable", "layout"]
        any_fail = any(per[c][k]["failures"] for c in per for k in tool_kinds)
        if rc != 0 and not any_fail:
            bucket = per[CORE_ID if CORE_ID in per else static_ids[0]][kind]
            bucket["total"] += 1
            bucket["failures"].append("%s exited %d without a FAIL line; see the run log" % (tool, rc))

    components = []
    for cid in static_ids + plugin_ids:
        checks = []
        for kind in STATIC_KINDS:
            b = per[cid][kind]
            if b["total"] == 0:
                continue
            checks.append({"kind": kind, "status": "fail" if b["failures"] else "pass",
                           "passed": b["passed"], "total": b["total"], "failures": b["failures"]})
        checks.extend(dynamic_pending_checks(cid, needs.get(cid)))
        components.append({"id": cid, "name": component_name(cid), "status": component_status(checks),
                           "checks": checks})
    return components


def dynamic_pending_checks(cid, need):
    """What only a running server can confirm: schema fields and game events (from needs.json),
    the selftest, and for match the bot live test."""
    checks = []
    if need:
        n_schema = len(need.get("schema") or []) + len(need.get("schema_optional") or [])
        if n_schema:
            checks.append(dict(pending_check("schema"), total=n_schema))
        if need.get("events"):
            checks.append(dict(pending_check("event"), total=len(need["events"])))
    checks.append(pending_check("selftest"))
    if cid == "match":
        checks.append(pending_check("livetest"))
    return checks


def all_component_ids(files):
    ids = [cid for cid, _ in files] or [CORE_ID]
    return ids + [c for c in RUNTIME_ONLY if c not in ids]


# ---------------------------------------------------------------------------------------------
# Plugin needs (plugins/<id>/needs.json, docs/CS2-COMPAT.md "Plugin needs")
# ---------------------------------------------------------------------------------------------

# Engine-facing ru_api members -> the engine-surface entries (gamedata/engine-surface*.json:
# function, hook, rtti, vtable or layout name) the core needs to serve them
# (core/src/readyup/plugin_engine_api.cpp, plugin_loader.cpp). Members not listed here touch no
# engine surface (config, stash, interfaces, logging, ...).
_CONSOLE = ["ISource2GameClients::ClientCommand", "CCommand"]
_FRAME = ["ISource2Server::GameFrame"]
_HTML = ["LegacyGameEventListener", "CServerSideClient_GameEventLegacyProxy"]
_ENTSYS = ["UTIL_Remove"]  # g_pGameEntitySystem is found through UTIL_Remove
API_SURFACE = {
    "chat_all": ["UTIL_ClientPrintAll"],
    "chat_to_slot": ["ClientPrint"],
    "register_chat_command": ["Host_Say"],
    "register_chat_command_ex": ["Host_Say"],
    "set_chat_name_prefix": ["Host_Say"],
    "register_console_command": _CONSOLE,
    "register_console_command_ex": _CONSOLE,
    "register_ru_subcommand": _CONSOLE,
    "on_tick": _FRAME,
    "on_frame": _FRAME,
    "post_to_game_thread": _FRAME,
    "subscribe_game_event": ["CGameEventManager_Init", "CGameEventManager"],
    "center_html_to_slot": _HTML,
    "center_html_all": _HTML,
    "center_html_to_slot_prio": _HTML,
    "center_html_all_prio": _HTML,
    "schema_offset": ["CSchemaSystem", "CSchemaSystemTypeScope"],
    "entity_system_status": _ENTSYS,
    "entity_by_index": _ENTSYS,
    "entity_from_handle": _ENTSYS,
    "entity_handle_of": _ENTSYS,
    "entity_classname": _ENTSYS,
    "entity_remove": _ENTSYS,
    "entity_mark_changed": ["CBaseEntity_NetworkStateChanged", "CEntityInstance::NetworkStateChanged"],
    "econ_attr_set_by_name": ["CAttributeList_SetOrAddAttributeValueByName"],
    "entity_change_subclass": ["CBaseEntity_ChangeSubclass"],
    "entity_set_model": ["CBaseModelEntity_SetModel"],
    "entity_set_bodygroup_by_name": ["CBaseModelEntity_GetModel", "CModel_FindBodygroupByName",
                                     "CBaseModelEntity_SetBodygroup"],
    "entity_set_abs_origin": ["CBaseEntity_SetAbsOrigin"],
    "set_round_termination_suppressed": ["CCSGameRules_TerminateRound"],
}
NEEDS_KEYS = ("schema_version", "plugin", "api", "surface", "schema", "schema_optional", "events")
SRC_EXT = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc")


def load_needs(plugins_dir):
    """{plugin id: needs.json dict} for every plugins/<id>/needs.json (missing dir: {})."""
    out = {}
    for path in sorted(glob.glob(os.path.join(plugins_dir or "", "*", "needs.json"))):
        with open(path, encoding="utf-8") as f:
            out[os.path.basename(os.path.dirname(path))] = json.load(f)
    return out


def plugin_sources(plugin_dir):
    """Concatenated source of a plugin (tests excluded), comments stripped."""
    texts = []
    for dirpath, dirnames, filenames in os.walk(plugin_dir):
        dirnames[:] = sorted(d for d in dirnames if d != "tests")
        for fn in sorted(filenames):
            if fn.endswith(SRC_EXT):
                texts.append(read_text(os.path.join(dirpath, fn)))
    src = "\n".join(texts)
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


_LIT = r'"([A-Za-z_][A-Za-z0-9_]*)"'
_LIST_OF_LITS = r'\{((?:\s*"[A-Za-z0-9_]+"\s*,?)+)\}'


def derive_needs(src):
    """What the source implies: {"api": set, "schema": {(cls, field)}, "fields": set, "events": set}."""
    api = {m for m in re.findall(r"->\s*([a-z_0-9]+)\s*\(", src) if m in API_SURFACE}
    pairs = set(re.findall(r'"([A-Z][A-Za-z0-9_]*)"\s*,\s*"(m_[A-Za-z0-9_]+)"', src))
    # Wrappers that try several classes: F({"A", "B"}, "m_x") and F("m_x", {"A", "B"}).
    for lits, field in re.findall(_LIST_OF_LITS + r"\s*,\s*\"(m_[A-Za-z0-9_]+)\"", src):
        pairs |= {(c, field) for c in re.findall(_LIT, lits)}
    for field, lits in re.findall(r"\"(m_[A-Za-z0-9_]+)\"\s*,\s*" + _LIST_OF_LITS, src):
        pairs |= {(c, field) for c in re.findall(_LIT, lits)}
    fields = set(re.findall(r"\"(m_[A-Za-z0-9_]+)\"", src))
    # Event names: a literal argument, or the loop variable over an inline list / named array.
    arrays = {n: re.findall(r'"([a-z0-9_]+)"', body)
              for n, body in re.findall(r"const\s+char\s*\*\s*(?:const\s+)?(\w+)\s*\[\s*\]\s*=\s*\{([^}]*)\}", src)}
    loops = [(m.start(), m.group(1), m.group(2), m.group(3)) for m in re.finditer(
        r"for\s*\(\s*const\s+char\s*\*\s*(?:const\s+)?(\w+)\s*:\s*(?:\{([^}]*)\}|(\w+))\s*\)", src)]
    events = set()
    for m in re.finditer(r"subscribe_game_event\s*\(\s*[^,]+,\s*(?:\"([^\"]+)\"|(\w+))", src):
        if m.group(1):
            events.add(m.group(1))
            continue
        var = m.group(2)
        loop = [lp for lp in loops if lp[0] < m.start() and lp[1] == var]
        if loop:
            _, _, inline, arr = loop[-1]
            events |= set(re.findall(r'"([a-z0-9_]+)"', inline)) if inline else set(arrays.get(arr, []))
        else:
            events.add("<non-literal:%s>" % var)
    return {"api": api, "schema": pairs, "fields": fields, "events": events}


def schema_alternatives(entry):
    """"A.m_x|B.m_x" -> [("A", "m_x"), ("B", "m_x")]."""
    out = []
    for alt in entry.split("|"):
        cls, _, field = alt.strip().partition(".")
        out.append((cls, field))
    return out


def surface_names(gamedata_dir):
    names = set()
    for _, path in surface_files(gamedata_dir):
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
        for key in ("functions", "rtti", "vtable_indices", "layouts"):
            names |= set(d.get(key) or {})
    return names


def check_needs(pid, need, derived, known_surface):
    """Problems (strings) with one plugin's manifest; empty = in sync with its source."""
    errs = []
    extra = set(need) - set(NEEDS_KEYS)
    if extra:
        errs.append("unknown keys: %s" % ", ".join(sorted(extra)))
    if need.get("plugin") != pid:
        errs.append('"plugin" is %r, expected %r' % (need.get("plugin"), pid))
    api = set(need.get("api") or [])
    for m in sorted(derived["api"] - api):
        errs.append('source calls ru_api %s: add it to "api"' % m)
    for m in sorted(api - set(API_SURFACE)):
        errs.append('"api" lists %s, which touches no engine surface (not in API_SURFACE)' % m)
    surface = set(need.get("surface") or [])
    for m in sorted(api & set(API_SURFACE)):
        for s in API_SURFACE[m]:
            if s not in surface:
                errs.append('ru_api %s needs engine-surface entry %s: add it to "surface"' % (m, s))
    for s in sorted(surface - known_surface):
        errs.append('"surface" lists %s, which is in no gamedata/engine-surface*.json' % s)
    declared = set()
    for e in (need.get("schema") or []) + (need.get("schema_optional") or []):
        alts = schema_alternatives(e)
        if any(not c or not f.startswith("m_") for c, f in alts):
            errs.append('bad schema entry %r (want "Class.m_field" or "A.m_x|B.m_x")' % e)
        declared |= set(alts)
    for c, f in sorted(derived["schema"] - declared):
        errs.append('source reads schema %s.%s: add it to "schema" (or "schema_optional")' % (c, f))
    declared_fields = {f for _, f in declared}
    for f in sorted(derived["fields"] - declared_fields):
        errs.append('source uses schema field literal "%s" with no class in "schema"' % f)
    events = set(need.get("events") or [])
    for e in sorted(derived["events"] - events):
        errs.append('source subscribes to game event %s: add it to "events"' % e)
    return errs


def cmd_needs_check(args):
    known = surface_names(args.gamedata)
    needs = load_needs(args.plugins_dir)
    bad = 0
    for pid in NEEDS_PLUGINS:
        d = os.path.join(args.plugins_dir, pid)
        if not os.path.isdir(d):
            continue
        if pid not in needs:
            print("FAIL %s: no plugins/%s/needs.json" % (pid, pid))
            bad += 1
            continue
        errs = check_needs(pid, needs[pid], derive_needs(plugin_sources(d)), known)
        for e in errs:
            print("FAIL %s: %s" % (pid, e))
        if not errs:
            n = needs[pid]
            print("OK   %s: %d api, %d surface, %d schema, %d events" % (
                pid, len(n.get("api") or []), len(n.get("surface") or []),
                len(n.get("schema") or []) + len(n.get("schema_optional") or []), len(n.get("events") or [])))
        bad += bool(errs)
    return 1 if bad else 0


def cmd_needs_derive(args):
    d = derive_needs(plugin_sources(os.path.join(args.plugins_dir, args.plugin)))
    surface = sorted({s for m in d["api"] for s in API_SURFACE[m]})
    by_field = {}
    for c, f in sorted(d["schema"]):
        by_field.setdefault(f, []).append("%s.%s" % (c, f))
    doc = {"schema_version": 1, "plugin": args.plugin, "api": sorted(d["api"]), "surface": surface,
           "schema": sorted("|".join(v) for v in by_field.values()), "schema_optional": [],
           "events": sorted(d["events"])}
    print(json.dumps(doc, indent=1))
    return 0


# ---------------------------------------------------------------------------------------------
# Dynamic stages (selftest + live), .github/workflows/cs2-dynamic.yml
# ---------------------------------------------------------------------------------------------

# readyup_selftest.txt check line: "  <STATUS> <name padded to 46> <detail>" (core/src/readyup/selftest.cpp).
ST_LINE = re.compile(r"^\s+(OK|FAIL|PEND|SKIP|INFO|WARN)\s+(.*?)\s*$")
ST_NEED = re.compile(r"^need (\S+) (surface|schema|schema_optional|event) (\S+)(?:\s+(.*))?$")
ST_SECTION = re.compile(r"^\[(.+)\]\s*$")


def split_name_detail(rest):
    parts = re.split(r"\s{2,}", rest.strip(), maxsplit=1)
    return parts[0], (parts[1] if len(parts) > 1 else "")


def parse_selftest(text):
    """{"core": [(status, name, detail)], "plugins": {pid: [(status, check, detail)]},
        "needs": {pid: [(status, kind, entry, detail)]}, "loaded": set | None, "exit_code": int | None}"""
    out = {"core": [], "plugins": {}, "needs": {}, "loaded": None, "exit_code": None, "load_failures": []}
    section = ""
    for raw in text.splitlines():
        line = raw.rstrip()
        m = ST_SECTION.match(line)
        if m:
            section = m.group(1)
            continue
        if line.startswith("exit_code:"):
            try:
                out["exit_code"] = int(line.split(":", 1)[1])
            except ValueError:
                pass
            continue
        m = ST_LINE.match(line)
        if not m:
            continue
        status, rest = m.group(1), m.group(2)
        n = ST_NEED.match(rest)
        if n:
            out["needs"].setdefault(n.group(1), []).append((status, n.group(2), n.group(3), (n.group(4) or "").strip()))
            continue
        if section == "plugins":
            if rest.startswith("plugin host"):
                lm = re.search(r"loaded: (.*)$", rest)
                out["loaded"] = set(x.strip().split(" ")[0] for x in lm.group(1).split(",")) if lm else set()
                continue
            if rest.startswith("plugin load"):
                out["load_failures"].append(rest[len("plugin load"):].strip())
                continue
            pm = re.match(r"^([a-z0-9_-]+): (.*)$", rest)
            if pm:
                name, detail = split_name_detail(pm.group(2))
                out["plugins"].setdefault(pm.group(1), []).append((status, name, detail))
                continue
        out["core"].append((status,) + split_name_detail(rest))
    return out


def tally(kind, rows, fail_statuses=("FAIL",), warn_statuses=("WARN",)):
    """rows: [(status, label)] -> check dict; PEND rows keep the check pending if nothing failed."""
    passed = sum(1 for s, _ in rows if s == "OK")
    failures = [short(label) for s, label in rows if s in fail_statuses]
    warns = [short(label) for s, label in rows if s in warn_statuses]
    counted = [r for r in rows if r[0] in ("OK",) + tuple(fail_statuses) + tuple(warn_statuses)]
    if failures:
        status = "fail"
    elif not counted or any(s == "PEND" for s, _ in rows):
        status = "pending" if not warns else "warn"
    elif warns:
        status = "warn"
    else:
        status = "pass"
    return {"kind": kind, "status": status, "passed": passed, "total": len(counted), "failures": failures + warns}


def dynamic_checks(cid, need, st, has_report):
    """schema / event / selftest checks for one component from a parsed selftest."""
    checks = []
    needs_rows = st["needs"].get(cid, []) if has_report else []
    if need:
        n_schema = len(need.get("schema") or []) + len(need.get("schema_optional") or [])
        if n_schema:
            rows = [(s if k == "schema" else ("MISS" if s == "WARN" else s), "%s: %s" % (e, d or "not found"))
                    for s, k, e, d in needs_rows if k in ("schema", "schema_optional")]
            # a required field that is missing fails the plugin; an optional one only warns
            rows = [("FAIL" if s == "WARN" else ("WARN" if s == "MISS" else s), label) for s, label in rows]
            checks.append(tally("schema", rows) if rows else dict(pending_check("schema"), total=n_schema))
        if need.get("events"):
            rows = [(s, "%s: %s" % (e, d or "unknown")) for s, k, e, d in needs_rows if k == "event"]
            checks.append(tally("event", rows) if rows else dict(pending_check("event"), total=len(need["events"])))
    if not has_report:
        checks.append(pending_check("selftest"))
        return checks
    if cid == CORE_ID:
        # "[runtime lookup]" schema lines are plugin lookups (alternatives a plugin tries); the
        # plugin's own needs lines judge those.
        rows = [(s, "%s: %s" % (n, d)) for s, n, d in st["core"]
                if not n.startswith("selftest:") and not d.endswith("[runtime lookup]")]
        sel = tally("selftest", rows)
        if st["exit_code"] is None:
            sel["failures"].append("no exit_code line (report cut short)")
            sel["status"] = "fail"
        checks.append(sel)
        return checks
    rows = []
    for s, n, d in st["plugins"].get(cid, []):
        if n == "disabled":
            rows.append(("FAIL", "disabled: %s" % d))
        else:
            rows.append((s, "%s: %s" % (n, d)))
    for f in st["load_failures"]:
        if f.startswith(cid + ":") or f.startswith(cid + ".so"):
            rows.append(("FAIL", "load failed: %s" % f))
    loaded = st["loaded"]
    if loaded is not None and cid not in loaded and not any(s == "FAIL" for s, _ in rows):
        rows.append(("FAIL", "not loaded (not in the plugin host's list)"))
    if not rows and loaded is not None and cid in loaded:
        rows.append(("OK", "loaded"))
    checks.append(tally("selftest", rows) if rows else pending_check("selftest"))
    return checks


def livetest_check(results):
    """results: [(name, rc)] ; rc 0 pass, 1 fail, 2 no verdict (server busy/down)."""
    rows = []
    for name, rc in results:
        if rc == 0:
            rows.append(("OK", name))
        elif rc == 2:
            rows.append(("PEND", "%s: no verdict (exit 2)" % name))
        else:
            rows.append(("FAIL", "%s: bot live test failed (exit %d); see the run" % (name, rc)))
    if not rows:
        return pending_check("livetest")
    chk = tally("livetest", rows)
    if chk["status"] == "pending":
        chk["failures"] = [short(label) for s, label in rows if s == "PEND"]
    return chk


def cmd_dynamic(args):
    base = {}
    if args.base and os.path.isfile(args.base):
        with open(args.base, encoding="utf-8") as f:
            base = json.load(f)
    needs = load_needs(args.plugins_dir)
    text = read_text(args.selftest)
    has_report = bool(text.strip())
    st = parse_selftest(text)
    env = read_env_file(args.build_env)
    buildid = str(env.get("BUILDID") or args.buildid or "")
    if base and buildid and base.get("cs2", {}).get("buildid") not in ("", buildid):
        print("::warning::static verdict is for CS2 build %s, this server runs %s; static checks kept as-is"
              % (base["cs2"]["buildid"], buildid))
    ids = [c["id"] for c in base.get("components") or []] or all_component_ids(surface_files(args.gamedata))
    by_id = {c["id"]: c for c in base.get("components") or []}
    live = [(n, int(rc)) for n, _, rc in (x.partition("=") for x in args.livetest or [])]
    components = []
    for cid in ids:
        old = by_id.get(cid, {"checks": []})
        static = [k for k in old["checks"] if k["kind"] in STATIC_KINDS]
        checks = static + dynamic_checks(cid, needs.get(cid), st, has_report)
        if cid == "match":
            checks.append(livetest_check(live) if args.stage == "live" else pending_check("livetest"))
        components.append({"id": cid, "name": component_name(cid), "status": component_status(checks), "checks": checks})
    if not has_report:
        # the server never wrote a report: that is Ready Up failing to boot on this build
        core = next((c for c in components if c["id"] == CORE_ID), None)
        if core:
            core["checks"] = [k for k in core["checks"] if k["kind"] != "selftest"] + [
                {"kind": "selftest", "status": "fail", "passed": 0, "total": 1,
                 "failures": ["no readyup_selftest.txt: the server did not finish booting with Ready Up"]}]
            core["status"] = component_status(core["checks"])
    overall = overall_status(components)
    if not args.patch and base:
        args.patch = base.get("cs2", {}).get("patch", "")
    if not args.buildid and base:
        args.buildid = base.get("cs2", {}).get("buildid", "")
    doc = document(args, overall, overall, components, now_iso(), args.build_env, stage=args.stage)
    write_json(os.path.join(args.out_dir, "compat.json"), doc)
    write_json(os.path.join(args.out_dir, "badge.json"), badge(doc))
    print("compat[%s]: overall=%s (%s)" % (args.stage, overall,
                                          ", ".join("%s=%s" % (c["id"], c["status"]) for c in components)))
    return 0


# ---------------------------------------------------------------------------------------------
# Document assembly
# ---------------------------------------------------------------------------------------------

def readyup_version(version_file, commit):
    version = read_text(version_file).strip()
    ref_type = os.environ.get("GITHUB_REF_TYPE", "")
    if version and commit and ref_type != "tag":
        version = "%s-dev.%s" % (version, commit[:7])
    return version


def document(args, state, overall, components, finished_at, build_env, stage="static"):
    env = read_env_file(build_env)
    checked = now_iso()
    return {
        "schema": SCHEMA,
        "cs2": {"buildid": str(env.get("BUILDID") or args.buildid or ""),
                "patch": env.get("PATCH_VERSION") or args.patch or ""},
        "readyup": {"version": readyup_version(args.version_file, args.commit), "commit": args.commit or ""},
        "run": {"id": str(args.run_id or ""), "url": args.run_url or "", "trigger": args.trigger,
                "stage": stage, "state": state, "started_at": args.started_at or checked,
                "finished_at": finished_at},
        "overall": overall,
        "components": components,
        "checked_at": checked,
    }


def badge(doc):
    message, color = BADGE.get(doc["overall"], BADGE["checking"])
    patch = doc["cs2"].get("patch") or doc["cs2"].get("buildid") or "?"
    return {"schemaVersion": 1, "label": "CS2 %s" % patch, "message": message, "color": color}


def write_json(path, obj):
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=1, ensure_ascii=False)
        f.write("\n")


def cmd_result(args):
    raw = args.raw_dir
    sig_text = read_text(os.path.join(raw, "sigcheck.txt"))
    hook_text = read_text(os.path.join(raw, "hookcheck.txt"))
    rcs = read_env_file(os.path.join(raw, "rc.env"))
    sig_rc = int(rcs.get("SIGCHECK_RC", "2") or 2)
    hook_rc = int(rcs.get("HOOKCHECK_RC", "2") or 2)
    components = build_components(sig_text, hook_text, sig_rc, hook_rc, surface_files(args.gamedata),
                                  load_needs(args.plugins_dir))
    overall = overall_status(components)
    doc = document(args, overall, overall, components, now_iso(), args.build_env)
    write_json(os.path.join(args.out_dir, "compat.json"), doc)
    write_json(os.path.join(args.out_dir, "badge.json"), badge(doc))
    print("compat: overall=%s (%s)" % (overall, ", ".join("%s=%s" % (c["id"], c["status"]) for c in components)))
    return 0


def cmd_phase(args):
    ids = all_component_ids(surface_files(args.gamedata))
    if args.state == "no_verdict":
        comp_status, overall, finished = "pending", "no_verdict", now_iso()
    else:
        comp_status, overall, finished = "checking", "checking", None
    components = [{"id": cid, "name": component_name(cid), "status": comp_status, "checks": []} for cid in ids]
    doc = document(args, args.state, overall, components, finished, args.build_env, stage=args.stage)
    write_json(args.out, doc)
    print("compat: phase %s -> %s" % (args.state, args.out))
    return 0


def cmd_post(args):
    url = os.environ.get("COMPAT_INGEST_URL", "").strip()
    token = os.environ.get("COMPAT_INGEST_TOKEN", "").strip()
    if not url:
        return 0
    try:
        with open(args.file, "rb") as f:
            body = f.read()
        json.loads(body)
    except (OSError, ValueError) as e:
        print("::warning::compat ingest: cannot read %s: %s" % (args.file, e))
        return 0
    headers = {"Content-Type": "application/json", "User-Agent": "readyup-cs2-watch"}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request(url, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=args.timeout) as resp:
            print("compat ingest: HTTP %d" % resp.status)
    except urllib.error.HTTPError as e:
        print("::warning::compat ingest: HTTP %d (ignored)" % e.code)
    except Exception as e:  # noqa: BLE001 - never fail the check because the POST failed
        print("::warning::compat ingest: %s (ignored)" % type(e).__name__)
    return 0


def add_run_options(p):
    e = os.environ.get
    server, repo, run_id = e("GITHUB_SERVER_URL", "https://github.com"), e("GITHUB_REPOSITORY", ""), e("GITHUB_RUN_ID", "")
    p.add_argument("--run-id", default=run_id)
    p.add_argument("--run-url", default="%s/%s/actions/runs/%s" % (server, repo, run_id) if repo and run_id else "")
    p.add_argument("--trigger", default="manual", choices=TRIGGERS)
    p.add_argument("--started-at", default="")
    p.add_argument("--commit", default=e("GITHUB_SHA", ""))
    p.add_argument("--version-file", default="VERSION")
    p.add_argument("--buildid", default="", help="used when --build-env has no BUILDID")
    p.add_argument("--patch", default="", help="used when --build-env has no PATCH_VERSION")
    p.add_argument("--build-env", default="", help="cs2-build.env from fetch-cs2-binaries.sh")
    p.add_argument("--gamedata", default="gamedata")
    p.add_argument("--plugins-dir", default=os.path.join(ROOT, "plugins"), help="plugins/<id>/needs.json")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("result")
    add_run_options(r)
    r.add_argument("--raw-dir", required=True, help="VERIFY_RAW_DIR of verify-cs2.sh")
    r.add_argument("--out-dir", required=True)
    ph = sub.add_parser("phase")
    add_run_options(ph)
    ph.add_argument("state", choices=("queued", "checking", "no_verdict"))
    ph.add_argument("--stage", default="static", choices=("static", "selftest", "live"))
    ph.add_argument("--out", required=True)
    po = sub.add_parser("post")
    po.add_argument("file")
    po.add_argument("--timeout", type=float, default=15)
    dy = sub.add_parser("dynamic")
    add_run_options(dy)
    dy.add_argument("--stage", required=True, choices=("selftest", "live"))
    dy.add_argument("--base", default="", help="compat.json of the static stage (cs2-build)")
    dy.add_argument("--selftest", default="", help="readyup_selftest.txt of the server")
    dy.add_argument("--livetest", action="append", default=[], metavar="NAME=RC",
                    help="scripts/livetest/run.sh exit code (0 pass, 1 fail, 2 no verdict); repeatable")
    dy.add_argument("--out-dir", required=True)
    nc = sub.add_parser("needs-check")
    nc.add_argument("--plugins-dir", default=os.path.join(ROOT, "plugins"))
    nc.add_argument("--gamedata", default=os.path.join(ROOT, "gamedata"))
    nd = sub.add_parser("needs-derive")
    nd.add_argument("plugin")
    nd.add_argument("--plugins-dir", default=os.path.join(ROOT, "plugins"))
    args = ap.parse_args(argv)
    return {"result": cmd_result, "phase": cmd_phase, "post": cmd_post, "needs-check": cmd_needs_check, "dynamic": cmd_dynamic,
            "needs-derive": cmd_needs_derive}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
