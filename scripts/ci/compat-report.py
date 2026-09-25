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
# Plugins that never touch the engine directly: nothing to check statically, they are loaded
# at runtime and only the dynamic stage (selftest/livetest, not built yet) can verify them.
RUNTIME_ONLY = ["match", "practice", "essentials", "midas", "whitelist", "fleet"]
NAMES = {"core": "Core", "skins": "Skins", "match": "Match", "practice": "Practice",
         "essentials": "Essentials", "midas": "Midas", "whitelist": "Whitelist", "fleet": "Fleet"}
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


def build_components(sig_text, hook_text, sig_rc, hook_rc, files):
    owner, _ = ownership(files)
    rows = parse_sigcheck(sig_text) + parse_hookcheck(hook_text)
    static_ids = [cid for cid, _ in files] or [CORE_ID]
    per = {cid: {k: {"passed": 0, "total": 0, "failures": []} for k in STATIC_KINDS} for cid in static_ids}

    for kind, name, ok, detail in rows:
        cid = owner.get((kind, name), CORE_ID)
        if cid not in per:
            cid = CORE_ID
        bucket = per[cid][kind]
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
    for cid in static_ids:
        checks = []
        for kind in STATIC_KINDS:
            b = per[cid][kind]
            if b["total"] == 0:
                continue
            checks.append({"kind": kind, "status": "fail" if b["failures"] else "pass",
                           "passed": b["passed"], "total": b["total"], "failures": b["failures"]})
        checks.append(pending_check("selftest"))
        components.append({"id": cid, "name": component_name(cid), "status": component_status(checks),
                           "checks": checks})
    for cid in RUNTIME_ONLY:
        if cid in static_ids:
            continue
        # static: n/a (loaded at runtime) -> only the dynamic stage can verify these
        checks = [pending_check("selftest")]
        components.append({"id": cid, "name": component_name(cid), "status": "pending", "checks": checks})
    return components


def all_component_ids(files):
    ids = [cid for cid, _ in files] or [CORE_ID]
    return ids + [c for c in RUNTIME_ONLY if c not in ids]


# ---------------------------------------------------------------------------------------------
# Document assembly
# ---------------------------------------------------------------------------------------------

def readyup_version(version_file, commit):
    version = read_text(version_file).strip()
    ref_type = os.environ.get("GITHUB_REF_TYPE", "")
    if version and commit and ref_type != "tag":
        version = "%s-dev.%s" % (version, commit[:7])
    return version


def document(args, state, overall, components, finished_at, build_env):
    env = read_env_file(build_env)
    checked = now_iso()
    return {
        "schema": SCHEMA,
        "cs2": {"buildid": str(env.get("BUILDID") or args.buildid or ""),
                "patch": env.get("PATCH_VERSION") or args.patch or ""},
        "readyup": {"version": readyup_version(args.version_file, args.commit), "commit": args.commit or ""},
        "run": {"id": str(args.run_id or ""), "url": args.run_url or "", "trigger": args.trigger,
                "stage": "static", "state": state, "started_at": args.started_at or checked,
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
    components = build_components(sig_text, hook_text, sig_rc, hook_rc, surface_files(args.gamedata))
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
    doc = document(args, args.state, overall, components, finished, args.build_env)
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
    ph.add_argument("--out", required=True)
    po = sub.add_parser("post")
    po.add_argument("file")
    po.add_argument("--timeout", type=float, default=15)
    args = ap.parse_args(argv)
    return {"result": cmd_result, "phase": cmd_phase, "post": cmd_post}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
