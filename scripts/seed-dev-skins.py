#!/usr/bin/env python3
"""Dev seed: a full skins loadout for one player in loadouts.json (plugins/skins/docs/json-contract.md).

    scripts/seed-dev-skins.py --steamid 7656... --csgo /path/to/game/csgo
    scripts/seed-dev-skins.py --steamid 7656... --file csgo/readyup/plugins/skins/loadouts.json
    scripts/seed-dev-skins.py --steamid 7656... --print          # just print the player's rows

Idempotent: rows are upserted by their keys (weapon_team + weapon_defindex, weapon_team), other
players and other rows of this player are kept. The write is atomic (temp file + rename) under
flock(loadouts.json.lock), like the plugin's own files. The skins plugin picks the change up
within 45 s (or `skins_refresh <steamid64>` in the server console).

IDs verified against items_game.txt of CS2 1.41.8.3. weapon_team: 2 = T, 3 = CT, 0 = both.
"""
import argparse
import fcntl
import json
import os
import sys
import time

# (weapon_team, weapon_defindex, paint_id, wear, seed, nametag, stattrak_enabled, stattrak_count)
SKINS = [
    (2, 7, 282, 0.15, 661, None, False, 0),                 # AK-47 | Redline (FT)
    (0, 9, 279, 0.12, 0, None, True, 1337),                 # StatTrak AWP | Asiimov
    (3, 16, 309, 0.03, 0, "Ready Up test", False, 0),       # M4A4 | Howl, name tag
    (3, 60, 984, 0.02, 0, None, False, 0),                  # M4A1-S | Printstream
    (2, 4, 38, 0.01, 0, None, False, 0),                    # Glock-18 | Fade
    (3, 61, 504, 0.05, 0, None, False, 0),                  # USP-S | Kill Confirmed
    (0, 1, 37, 0.02, 0, None, False, 0),                    # Desert Eagle | Blaze
    (0, 507, 38, 0.01, 412, None, False, 0),                # ★ Karambit | Fade
    (0, 5030, 10037, 0.20, 0, None, False, 0),              # ★ Sport Gloves | Pandora's Box
]
KNIVES = [(0, "weapon_knife_karambit")]
GLOVES = [(0, 5030)]
# Special Agent Ava | FBI (CT), The Elite Mr. Muhlik | Elite Crew (T).
AGENTS = {
    "agent_ct": "agents/models/ctm_fbi/ctm_fbi_variantb.vmdl",
    "agent_t": "agents/models/tm_leet/tm_leet_variantf.vmdl",
}
VERSION = 1


def seed_rows():
    skins = []
    for team, defindex, paint, wear, seed, nametag, st_on, st_count in SKINS:
        row = {"weapon_team": team, "weapon_defindex": defindex, "paint_id": paint, "wear": wear, "seed": seed}
        if nametag:
            row["nametag"] = nametag
        if st_on:
            row["stattrak_enabled"] = True
            row["stattrak_count"] = st_count
        skins.append(row)
    return {
        "weapon_skins": skins,
        "weapon_knives": [{"weapon_team": t, "knife_classname": c} for t, c in KNIVES],
        "weapon_gloves": [{"weapon_team": t, "glove_defindex": d} for t, d in GLOVES],
        "weapon_agents": dict(AGENTS),
    }


def upsert(rows, new_rows, keys):
    index = {tuple(r.get(k) for k in keys): i for i, r in enumerate(rows)}
    for r in new_rows:
        k = tuple(r.get(k) for k in keys)
        if k in index:
            rows[index[k]] = r
        else:
            index[k] = len(rows)
            rows.append(r)
    return rows


def merge_player(player, seed):
    player["weapon_skins"] = upsert(list(player.get("weapon_skins") or []), seed["weapon_skins"],
                                    ("weapon_team", "weapon_defindex"))
    player["weapon_knives"] = upsert(list(player.get("weapon_knives") or []), seed["weapon_knives"], ("weapon_team",))
    player["weapon_gloves"] = upsert(list(player.get("weapon_gloves") or []), seed["weapon_gloves"], ("weapon_team",))
    player["weapon_agents"] = seed["weapon_agents"]
    return player


def write_atomic(path, doc):
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    tmp = "%s.tmp.%d" % (path, os.getpid())
    with open(tmp, "w") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--steamid", required=True, help="SteamID64 of the player to seed")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--file", help="path of loadouts.json")
    g.add_argument("--csgo", help="the game's csgo dir (writes readyup/plugins/skins/loadouts.json)")
    g.add_argument("--print", action="store_true", help="print the player's rows, write nothing")
    a = ap.parse_args()
    if not a.steamid.isdigit() or len(a.steamid) != 17:
        ap.error("--steamid must be a 17-digit SteamID64")

    seed = seed_rows()
    if a.print:
        json.dump({"version": VERSION, "players": {a.steamid: seed}}, sys.stdout, indent=2)
        print()
        return 0

    path = a.file or os.path.join(a.csgo, "readyup", "plugins", "skins", "loadouts.json")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path + ".lock", "a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        doc = {"version": VERSION, "players": {}}
        if os.path.exists(path):
            try:
                with open(path) as f:
                    doc = json.load(f)
                if not isinstance(doc, dict) or doc.get("version") != VERSION:
                    raise ValueError("unexpected version %r" % (doc.get("version") if isinstance(doc, dict) else None))
            except ValueError as e:
                backup = "%s.corrupt-%d" % (path, int(time.time()))
                os.replace(path, backup)
                print("warning: %s: %s; moved to %s, starting fresh" % (path, e, backup), file=sys.stderr)
                doc = {"version": VERSION, "players": {}}
        players = doc.setdefault("players", {})
        players[a.steamid] = merge_player(players.get(a.steamid) or {}, seed)
        write_atomic(path, doc)
    print("seeded %s: %d paint(s), knife, gloves, agents -> %s" % (a.steamid, len(seed["weapon_skins"]), path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
