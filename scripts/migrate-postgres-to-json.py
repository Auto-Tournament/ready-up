#!/usr/bin/env python3
"""One-time migration: an existing Ready Up Postgres -> the JSON files that replace it (D13).

Ready Up no longer uses Postgres (docs/FLEET.md D13). Run this once on an install that had one,
before (or right after) deploying the Postgres-free build:

    # Postgres from readyup_db.json next to the core (csgo/readyup/bin/linuxsteamrt64/):
    scripts/migrate-postgres-to-json.py --csgo /path/to/game/csgo

    # Postgres in a container, psql run inside it (no psql needed on the host):
    scripts/migrate-postgres-to-json.py --csgo /path/to/game/csgo --docker readyup-postgres

    # explicit libpq conninfo, preview only:
    scripts/migrate-postgres-to-json.py --csgo ... --conninfo "host=127.0.0.1 port=5449 dbname=readyup user=readyup" --dry-run

What it writes (under <csgo>/readyup/plugins/):
    match/admins.json     readyup_admins        -> {"version":1,"admins":[{"steamid64","name"}]}
    match/state.json      readyup_settings      -> {"version":1,"settings":{key: value}}
    skins/loadouts.json   readyup_weapon_skins / _knives / _gloves / _agents
                                                -> {"version":1,"players":{steamid64: {tables}}}
                                                   (plugins/skins/docs/json-contract.md)

Existing JSON files are merged, not replaced: what is already in them wins, Postgres fills in the
rest (--prefer-db flips that). Missing tables are skipped. Writes are atomic (temp file + rename)
under flock(<file>.lock), the lock the plugins take. The database is only read.

Needs `psql` (on the host, or in the container with --docker).
"""
import argparse
import fcntl
import json
import os
import shlex
import subprocess
import sys
import time

VERSION = 1


def conninfo_from_db_config(path):
    with open(path) as f:
        cfg = json.load(f)
    if cfg.get("conninfo"):
        return cfg["conninfo"]
    parts = []
    for key in ("host", "port", "dbname", "user", "password", "sslmode"):
        if cfg.get(key) not in (None, ""):
            parts.append("%s=%s" % (key, cfg[key]))
    if not parts:
        raise ValueError("%s has neither conninfo nor host/port/dbname/user" % path)
    return " ".join(parts)


class Db:
    def __init__(self, args):
        if args.docker:
            self.cmd = ["docker", "exec", "-i", args.docker, "psql", "-U", args.db_user, "-d", args.db_name]
            self.where = "docker %s (%s@%s)" % (args.docker, args.db_user, args.db_name)
        else:
            conninfo = args.conninfo
            if not conninfo:
                cfg = args.db_config or os.path.join(args.csgo, "readyup", "bin", "linuxsteamrt64", "readyup_db.json")
                conninfo = conninfo_from_db_config(cfg)
            self.cmd = ["psql", conninfo]
            self.where = " ".join(p for p in conninfo.split() if not p.startswith("password="))

    def json(self, sql):
        """Runs `sql` (one JSON value per result) and returns it parsed."""
        out = subprocess.run(self.cmd + ["-X", "-q", "-A", "-t", "-v", "ON_ERROR_STOP=1", "-c", sql],
                             check=True, capture_output=True, text=True).stdout.strip()
        return json.loads(out) if out else None

    def rows(self, table, columns, order):
        if not self.json("SELECT to_json(to_regclass('public.%s') IS NOT NULL)" % table):
            print("  %s: not there, skipped" % table)
            return []
        rows = self.json("SELECT COALESCE(json_agg(t ORDER BY %s), '[]'::json) FROM (SELECT %s FROM %s) t"
                         % (order, ", ".join(columns), table)) or []
        print("  %s: %d row(s)" % (table, len(rows)))
        return rows


def load(path, move_corrupt=True):
    if not os.path.exists(path):
        return None
    try:
        with open(path) as f:
            doc = json.load(f)
        if isinstance(doc, dict) and isinstance(doc.get("version"), int) and doc["version"] <= VERSION:
            return doc
        raise ValueError("unexpected version")
    except ValueError as e:
        if not move_corrupt:
            print("warning: %s: %s; would be moved aside" % (path, e), file=sys.stderr)
            return None
        backup = "%s.corrupt-%d" % (path, int(time.time()))
        os.replace(path, backup)
        print("warning: %s: %s; moved to %s" % (path, e, backup), file=sys.stderr)
        return None


def write(path, merge, dry_run):
    """merge(existing_doc_or_None) -> new doc; under the file's lock."""
    if dry_run:
        doc = merge(load(path, move_corrupt=False))
        print("--- %s (dry run)" % path)
        print(json.dumps(doc, indent=2))
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + ".lock", "a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        doc = merge(load(path))
        tmp = "%s.tmp.%d" % (path, os.getpid())
        with open(tmp, "w") as f:
            json.dump(doc, f, indent=2)
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    print("wrote %s" % path)


def upsert(existing, new, keys, prefer_db):
    out = list(existing)
    index = {tuple(r.get(k) for k in keys): i for i, r in enumerate(out)}
    for r in new:
        k = tuple(r.get(k) for k in keys)
        if k not in index:
            index[k] = len(out)
            out.append(r)
        elif prefer_db:
            out[index[k]] = r
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--csgo", required=True, help="the game's csgo dir (output goes to readyup/plugins/)")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--db-config", help="readyup_db.json (default: <csgo>/readyup/bin/linuxsteamrt64/readyup_db.json)")
    src.add_argument("--conninfo", help="libpq conninfo string")
    src.add_argument("--docker", metavar="CONTAINER", help="run psql inside this container")
    ap.add_argument("--db-user", default="readyup", help="with --docker (default readyup)")
    ap.add_argument("--db-name", default="readyup", help="with --docker (default readyup)")
    ap.add_argument("--prefer-db", action="store_true", help="Postgres values win over existing JSON entries")
    ap.add_argument("--dry-run", action="store_true", help="print the resulting files, write nothing")
    a = ap.parse_args()

    try:
        db = Db(a)
    except (OSError, ValueError) as e:
        ap.error("no database: %s (use --docker, --conninfo or --db-config)" % e)
    plugins = os.path.join(a.csgo, "readyup", "plugins")
    print("reading %s" % db.where)
    try:
        admins = db.rows("readyup_admins", ["steamid64::text AS steamid64", "display_name"], "display_name, steamid64")
        settings = db.rows("readyup_settings", ["key", "value"], "key")
        skins = db.rows("readyup_weapon_skins",
                        ["steamid64::text AS steamid64", "weapon_team", "weapon_defindex", "paint_id", "wear", "seed",
                         "nametag", "stattrak_enabled", "stattrak_count"], "steamid64, weapon_team, weapon_defindex")
        knives = db.rows("readyup_weapon_knives", ["steamid64::text AS steamid64", "weapon_team", "knife_classname"],
                         "steamid64, weapon_team")
        gloves = db.rows("readyup_weapon_gloves", ["steamid64::text AS steamid64", "weapon_team", "glove_defindex"],
                         "steamid64, weapon_team")
        agents = db.rows("readyup_weapon_agents", ["steamid64::text AS steamid64", "agent_ct", "agent_t"], "steamid64")
    except subprocess.CalledProcessError as e:
        print("psql failed (%s): %s" % (" ".join(shlex.quote(c) for c in e.cmd[:6]), e.stderr.strip()), file=sys.stderr)
        return 1
    except FileNotFoundError as e:
        print("cannot run %s: %s" % (db.cmd[0], e), file=sys.stderr)
        return 1

    # match/admins.json
    new_admins = [{"steamid64": r["steamid64"], "name": r.get("display_name") or ""} for r in admins]

    def merge_admins(doc):
        doc = doc or {"version": VERSION, "admins": []}
        doc["version"] = VERSION
        doc["admins"] = upsert(doc.get("admins") or [], new_admins, ("steamid64",), a.prefer_db)
        return doc

    if new_admins:
        write(os.path.join(plugins, "match", "admins.json"), merge_admins, a.dry_run)

    # match/state.json
    new_settings = {r["key"]: r["value"] for r in settings if r.get("value") not in (None, "")}

    def merge_state(doc):
        doc = doc or {"version": VERSION, "settings": {}}
        doc["version"] = VERSION
        cur = doc.get("settings") or {}
        for k, v in new_settings.items():
            if a.prefer_db or k not in cur:
                cur[k] = v
        doc["settings"] = cur
        return doc

    if new_settings:
        write(os.path.join(plugins, "match", "state.json"), merge_state, a.dry_run)

    # skins/loadouts.json
    players = {}

    def player(sid):
        return players.setdefault(sid, {"weapon_skins": [], "weapon_knives": [], "weapon_gloves": []})

    for r in skins:
        row = {"weapon_team": r["weapon_team"], "weapon_defindex": r["weapon_defindex"], "paint_id": r["paint_id"],
               "wear": r["wear"], "seed": r["seed"]}
        if r.get("nametag"):
            row["nametag"] = r["nametag"]
        if r.get("stattrak_enabled"):
            row["stattrak_enabled"] = True
            row["stattrak_count"] = r.get("stattrak_count") or 0
        player(r["steamid64"])["weapon_skins"].append(row)
    for r in knives:
        player(r["steamid64"])["weapon_knives"].append({"weapon_team": r["weapon_team"],
                                                         "knife_classname": r["knife_classname"]})
    for r in gloves:
        player(r["steamid64"])["weapon_gloves"].append({"weapon_team": r["weapon_team"],
                                                         "glove_defindex": r["glove_defindex"]})
    for r in agents:
        player(r["steamid64"])["weapon_agents"] = {"agent_ct": r.get("agent_ct"), "agent_t": r.get("agent_t")}

    def merge_loadouts(doc):
        doc = doc or {"version": VERSION, "players": {}}
        doc["version"] = VERSION
        cur = doc.setdefault("players", {})
        for sid, p in players.items():
            c = cur.setdefault(sid, {})
            c["weapon_skins"] = upsert(c.get("weapon_skins") or [], p["weapon_skins"],
                                       ("weapon_team", "weapon_defindex"), a.prefer_db)
            c["weapon_knives"] = upsert(c.get("weapon_knives") or [], p["weapon_knives"], ("weapon_team",), a.prefer_db)
            c["weapon_gloves"] = upsert(c.get("weapon_gloves") or [], p["weapon_gloves"], ("weapon_team",), a.prefer_db)
            if "weapon_agents" in p and (a.prefer_db or "weapon_agents" not in c):
                c["weapon_agents"] = p["weapon_agents"]
        return doc

    if players:
        write(os.path.join(plugins, "skins", "loadouts.json"), merge_loadouts, a.dry_run)

    print("done: %d admin(s), %d setting(s), %d player loadout(s)%s" %
          (len(new_admins), len(new_settings), len(players), " (dry run)" if a.dry_run else ""))
    if not a.dry_run:
        print("readyup_db.json and the Postgres data are left alone; remove them when you no longer need them.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
