#!/usr/bin/env python3
"""Ready Up scenario converter: CS2 demo (.dem) round -> practice scenario (JSON).

A scenario is one recorded round: every player's path (position + view angles, sampled every few
ticks), their health / armor / money / inventory over time, the grenades they threw (spawn point,
velocity, landing spot), deaths and the bomb. The practice plugin (`.scen load`) puts a player
where one of the recorded players stood and has bots re-enact the other nine. Schema and usage:
docs/SCENARIOS.md.

    python3 ru_scenario.py rounds  <demo.dem>
    python3 ru_scenario.py convert <demo.dem> --round 7 [--round 9 ...] [--start 25s|1:05|t12345]
                           [--out DIR] [--id NAME] [--title TEXT] [--event TEXT] [--match TEXT]
                           [--url URL] [--sample 4]
    python3 ru_scenario.py validate <scenario.json>...

Reading demos needs demoparser2 (MIT, https://github.com/LaihoE/demoparser):
`pip install demoparser2`. `validate` and everything below `build_scenario` need nothing but the
standard library, so tests and the server-side checks run without it.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys

FORMAT = "readyup.scenario"
VERSION = 1
CONVERTER_VERSION = "1"
DEFAULT_SAMPLE_TICKS = 4
# Grenades fall with sv_gravity (800) x the projectile gravity scale (0.4).
GRENADE_GRAVITY = 320.0
MAX_PLAYERS = 10
TEAMS = {2: "T", 3: "CT"}
GRENADE_TYPES = ("smoke", "flash", "hegrenade", "molotov", "incgrenade", "decoy")

# demoparser2 item names (inventory) -> classnames `give` takes. Knives map to the default knife.
ITEM_CLASSES = {
    "glock-18": "weapon_glock", "usp-s": "weapon_usp_silencer", "p2000": "weapon_hkp2000",
    "dual berettas": "weapon_elite", "p250": "weapon_p250", "five-seven": "weapon_fiveseven",
    "tec-9": "weapon_tec9", "cz75-auto": "weapon_cz75a", "desert eagle": "weapon_deagle",
    "r8 revolver": "weapon_revolver", "zeus x27": "weapon_taser",
    "mac-10": "weapon_mac10", "mp9": "weapon_mp9", "mp7": "weapon_mp7", "mp5-sd": "weapon_mp5sd",
    "ump-45": "weapon_ump45", "p90": "weapon_p90", "pp-bizon": "weapon_bizon",
    "nova": "weapon_nova", "xm1014": "weapon_xm1014", "sawed-off": "weapon_sawedoff", "mag-7": "weapon_mag7",
    "m249": "weapon_m249", "negev": "weapon_negev",
    "galil ar": "weapon_galilar", "famas": "weapon_famas", "ak-47": "weapon_ak47", "m4a4": "weapon_m4a1",
    "m4a1-s": "weapon_m4a1_silencer", "sg 553": "weapon_sg556", "aug": "weapon_aug", "ssg 08": "weapon_ssg08",
    "awp": "weapon_awp", "g3sg1": "weapon_g3sg1", "scar-20": "weapon_scar20",
    "smoke grenade": "weapon_smokegrenade", "flashbang": "weapon_flashbang",
    "high explosive grenade": "weapon_hegrenade", "molotov": "weapon_molotov",
    "incendiary grenade": "weapon_incgrenade", "decoy grenade": "weapon_decoy",
    "c4 explosive": "weapon_c4",
}
KNIFE_WORDS = ("knife", "bayonet", "karambit", "daggers", "kukri")

# Projectile classes (parse_grenades) and grenade_thrown weapons -> scenario grenade types.
PROJECTILE_TYPES = {
    "CSmokeGrenadeProjectile": "smoke", "CFlashbangProjectile": "flash", "CHEGrenadeProjectile": "hegrenade",
    "CMolotovProjectile": "molotov", "CDecoyProjectile": "decoy",
}
THROWN_TYPES = {
    "smokegrenade": "smoke", "flashbang": "flash", "hegrenade": "hegrenade", "molotov": "molotov",
    "incgrenade": "incgrenade", "decoy": "decoy",
}


class ConvertError(Exception):
    pass


# ---- pure helpers (no demoparser2) ---------------------------------------------------------------


def item_class(name):
    """demoparser2 item name -> weapon classname, or None (unknown / not an item)."""
    if not name:
        return None
    n = str(name).strip().lower()
    if n in ITEM_CLASSES:
        return ITEM_CLASSES[n]
    if n.startswith("weapon_"):
        return n
    if any(w in n for w in KNIFE_WORDS):
        return "weapon_knife"
    return None


def items_from_inventory(inv):
    """Inventory list (display names) -> sorted list of classnames (duplicates kept), knife first."""
    out = []
    for name in inv or []:
        c = item_class(name)
        if c:
            out.append(c)
    return sorted(out, key=lambda c: (c != "weapon_knife", c))


def parse_start(text, tickrate, freeze_end_tick):
    """'25' / '25s' / '1:05' -> ticks after freeze end; 't12345' / '12345t' -> absolute demo tick."""
    s = str(text).strip().lower()
    m = re.fullmatch(r"t(\d+)|(\d+)t", s)
    if m:
        return int(m.group(1) or m.group(2)) - freeze_end_tick
    m = re.fullmatch(r"(\d+):(\d{1,2}(?:\.\d+)?)", s)
    if m:
        return int(round((int(m.group(1)) * 60 + float(m.group(2))) * tickrate))
    m = re.fullmatch(r"(\d+(?:\.\d+)?)s?", s)
    if m:
        return int(round(float(m.group(1)) * tickrate))
    raise ConvertError("bad start %r (want seconds like 25 / 25s / 1:05, or a demo tick like t12345)" % text)


def round_windows(freeze_end_ticks, round_end_ticks, last_tick):
    """[(round number, freeze end tick, end tick)]: each freeze end paired with the first round end
    after it (the next freeze end, or the demo's last tick, when a round has no end event)."""
    fe = sorted(int(t) for t in freeze_end_ticks)
    ends = sorted(int(t) for t in round_end_ticks)
    out = []
    for i, start in enumerate(fe):
        nxt = fe[i + 1] if i + 1 < len(fe) else int(last_tick)
        end = next((e for e in ends if start < e <= nxt), nxt)
        out.append((i + 1, start, end))
    return out


def estimate_velocity(points, tickrate, gravity=GRENADE_GRAVITY):
    """Launch velocity from the first projectile positions [(tick, x, y, z)] (least squares over up
    to 6 samples; z corrected for gravity). None when there are fewer than 2 samples."""
    pts = sorted(points)[:6]
    if len(pts) < 2:
        return None
    t0 = pts[0][0]
    ts = [(p[0] - t0) / float(tickrate) for p in pts]
    n = len(pts)
    mt = sum(ts) / n
    den = sum((t - mt) ** 2 for t in ts)
    if den <= 0:
        return None
    vel = []
    for axis in (1, 2, 3):
        vs = [p[axis] + (0.5 * gravity * t * t if axis == 3 else 0.0) for p, t in zip(pts, ts)]
        mv = sum(vs) / n
        vel.append(sum((t - mt) * (v - mv) for t, v in zip(ts, vs)) / den)
    return vel


def r1(v):
    """Round to 0.1 (JSON size); -0.0 -> 0."""
    x = round(float(v), 1)
    return 0 if x == 0 else x


def build_scenario(meta, window, samples, projectiles, thrown, detonations, deaths, bomb, sample_ticks=DEFAULT_SAMPLE_TICKS,
                   start=0):
    """One round -> scenario dict. Everything is plain data (lists of dicts), so tests build it by hand.

    meta:        {"map", "tickrate", "round", "id", "title", "source": {...}, "winner", "reason", "parser"}
    window:      (round number, freeze end tick, end tick)
    samples:     [{"tick", "steamid", "name", "team_num", "X", "Y", "Z", "yaw", "pitch", "is_alive", "health",
                   "armor_value", "has_helmet", "has_defuser", "balance", "inventory"}] every sample_ticks
    projectiles: {entity id: {"type": projectile class, "steamid", "points": [(tick, x, y, z)]}}
    thrown:      [{"tick", "steamid", "weapon"}]      (grenade_thrown)
    detonations: {entity id: (tick, x, y, z)}          (smoke / flash / HE / decoy detonate events)
    deaths:      [{"tick", "victim", "attacker", "weapon"}]  (steamids as strings)
    bomb:        [{"tick", "event": "planted"|"defused"|"exploded", "steamid"}]
    """
    _, fe, end = window
    if end <= fe:
        raise ConvertError("round %s has no ticks (freeze end %d, end %d)" % (window[0], fe, end))
    tickrate = int(meta.get("tickrate") or 64)
    length = end - fe

    # Players: everyone on T / CT at the first sample, ordered T then CT, then by name.
    by_player = {}
    for s in samples:
        if s["tick"] < fe or s["tick"] > end:
            continue
        by_player.setdefault(str(s["steamid"]), []).append(s)
    first = {sid: min(rows, key=lambda r: r["tick"]) for sid, rows in by_player.items()}
    roster = [sid for sid, r in first.items() if int(r.get("team_num") or 0) in TEAMS and r.get("is_alive")]
    roster.sort(key=lambda sid: (TEAMS[int(first[sid]["team_num"])] != "T", str(first[sid]["name"]).lower()))
    if not roster:
        raise ConvertError("round %s: no players alive at freeze end" % window[0])
    roster = roster[:MAX_PLAYERS]
    index = {sid: i for i, sid in enumerate(roster)}

    players = []
    for sid in roster:
        rows = sorted(by_player[sid], key=lambda r: r["tick"])
        track, state, alive_until = [], [], None
        last = None
        for r in rows:
            t = r["tick"] - fe
            if not r.get("is_alive"):
                if alive_until is None:
                    alive_until = t
                break
            if t % sample_ticks:
                continue
            k = t // sample_ticks
            here = [r1(r["X"]), r1(r["Y"]), r1(r["Z"]), r1(r["yaw"]), r1(r["pitch"])]
            # Fill gaps (missing samples) by repeating the previous position (the first one: this one).
            while len(track) // 5 < k:
                track.extend(track[-5:] if track else here)
            if len(track) // 5 != k:
                continue
            track.extend(here)
            cur = {
                "hp": int(r.get("health") or 0),
                "armor": int(r.get("armor_value") or 0),
                "helmet": bool(r.get("has_helmet")),
                "defuser": bool(r.get("has_defuser")),
                "money": int(r.get("balance") or 0),
                "items": items_from_inventory(r.get("inventory")),
            }
            if cur != last:
                state.append(dict(cur, t=t if state else 0))
                last = cur
        f = first[sid]
        players.append({
            "name": str(f["name"]),
            "steamid": sid,
            "team": TEAMS[int(f["team_num"])],
            "alive_until": alive_until,
            "track": track,
            "state": state,
        })

    # Grenades: one per projectile entity that spawned inside the round, from a roster player.
    thrown_at = {}
    for t in thrown:
        thrown_at.setdefault(str(t["steamid"]), []).append((int(t["tick"]), THROWN_TYPES.get(str(t["weapon"]).replace("weapon_", ""))))
    grenades = []
    for eid, p in projectiles.items():
        pts = sorted(pt for pt in p["points"] if pt[1] is not None and not (isinstance(pt[1], float) and math.isnan(pt[1])))
        if not pts:
            continue
        t0 = pts[0][0]
        sid = str(p["steamid"])
        if t0 < fe or t0 > end or sid not in index:
            continue
        gtype = PROJECTILE_TYPES.get(p["type"])
        if not gtype:
            continue
        if gtype == "molotov":
            # Incendiary and molotov share the projectile class; grenade_thrown tells them apart.
            near = [w for tk, w in thrown_at.get(sid, []) if abs(tk - t0) <= 2 and w in ("molotov", "incgrenade")]
            if near:
                gtype = near[0]
        vel = estimate_velocity(pts, tickrate)
        if vel is None:
            continue
        det = detonations.get(eid)
        if det and det[0] >= t0:
            land_t, land = det[0], det[1:4]
        else:
            land_t, land = pts[-1][0], pts[-1][1:4]
        grenades.append({
            "t": t0 - fe,
            "player": index[sid],
            "type": gtype,
            "pos": [r1(v) for v in pts[0][1:4]],
            "vel": [r1(v) for v in vel],
            "land": [r1(v) for v in land],
            "land_t": int(land_t) - fe,
        })
    grenades.sort(key=lambda g: (g["t"], g["player"]))

    death_list = []
    for d in deaths:
        tk = int(d["tick"])
        if tk < fe or tk > end or str(d["victim"]) not in index:
            continue
        death_list.append({"t": tk - fe, "player": index[str(d["victim"])],
                           "killer": index.get(str(d.get("attacker")), -1), "weapon": str(d.get("weapon") or "")})
    death_list.sort(key=lambda d: d["t"])

    bomb_list = []
    for b in bomb:
        tk = int(b["tick"])
        if tk < fe or tk > end:
            continue
        e = {"t": tk - fe, "event": b["event"], "player": index.get(str(b.get("steamid")), -1)}
        pl = e["player"]
        if pl >= 0 and b["event"] in ("planted", "defused"):
            pos = position_at(players[pl], e["t"], sample_ticks)
            if pos:
                e["pos"] = [r1(v) for v in pos[:3]]
        bomb_list.append(e)
    bomb_list.sort(key=lambda b: b["t"])

    start = max(0, min(int(start), length - 1))
    teams = meta.get("teams") or {}
    return {
        "format": FORMAT,
        "version": VERSION,
        "id": meta["id"],
        "title": meta.get("title") or meta["id"],
        "map": meta["map"],
        "tickrate": tickrate,
        "sample_ticks": sample_ticks,
        "round": window[0],
        "freeze_end_tick": fe,
        "length": length,
        "default_start": start,
        "winner": meta.get("winner") or "",
        "reason": meta.get("reason") or "",
        "source": {
            "event": meta.get("event") or "",
            "match": meta.get("match") or "",
            "teams": {"T": teams.get("T", ""), "CT": teams.get("CT", "")},
            "url": meta.get("url") or "",
            "demo": meta.get("demo") or "",
            "note": meta.get("note") or "",
        },
        "converter": {"name": "ru_scenario.py", "version": CONVERTER_VERSION, "parser": meta.get("parser") or ""},
        "players": players,
        "grenades": grenades,
        "deaths": death_list,
        "bomb": bomb_list,
    }


def position_at(player, t, sample_ticks):
    """[x, y, z, yaw, pitch] at t ticks after freeze end (linear), or None past the track."""
    tr = player["track"]
    n = len(tr) // 5
    if n == 0:
        return None
    f = t / float(sample_ticks)
    if f >= n - 1:
        return tr[(n - 1) * 5:n * 5] if f <= n - 1 + 1e-9 else None
    k = int(f)
    a, b, w = tr[k * 5:k * 5 + 5], tr[(k + 1) * 5:(k + 2) * 5], f - k
    return [a[i] + (b[i] - a[i]) * w for i in range(5)]


# ---- validation (the plugin's parser checks the same) ---------------------------------------------


def validate(sc):
    """List of problems; empty = valid."""
    errs = []

    def need(cond, msg):
        if not cond:
            errs.append(msg)

    need(sc.get("format") == FORMAT, "format must be %r" % FORMAT)
    need(sc.get("version") == VERSION, "version must be %d" % VERSION)
    need(isinstance(sc.get("id"), str) and re.fullmatch(r"[a-z0-9_\-]{1,64}", sc.get("id") or ""),
         "id must be 1-64 chars of [a-z0-9_-]")
    need(isinstance(sc.get("map"), str) and sc.get("map"), "map missing")
    need(isinstance(sc.get("tickrate"), int) and 16 <= sc["tickrate"] <= 256, "tickrate must be 16..256")
    st = sc.get("sample_ticks")
    need(isinstance(st, int) and 1 <= st <= 64, "sample_ticks must be 1..64")
    length = sc.get("length")
    need(isinstance(length, int) and length > 0, "length must be > 0")
    src = sc.get("source") or {}
    need(isinstance(src, dict) and (src.get("event") or src.get("match")) and src.get("url") is not None,
         "source needs event or match (attribution) and url")
    players = sc.get("players") or []
    need(1 <= len(players) <= MAX_PLAYERS, "1..%d players" % MAX_PLAYERS)
    for i, p in enumerate(players):
        need(p.get("team") in ("T", "CT"), "player %d: team must be T or CT" % i)
        tr = p.get("track") or []
        need(len(tr) % 5 == 0 and len(tr) >= 5, "player %d: track must be groups of 5 numbers" % i)
        need(all(isinstance(v, (int, float)) and abs(v) < 65536 for v in tr), "player %d: track values out of range" % i)
        need(len(p.get("state") or []) >= 1 and (p["state"][0].get("t") == 0), "player %d: state must start at t=0" % i)
    for g in sc.get("grenades") or []:
        need(g.get("type") in GRENADE_TYPES, "grenade type %r" % g.get("type"))
        need(0 <= g.get("player", -1) < len(players), "grenade player index")
        need(len(g.get("pos") or []) == 3 and len(g.get("vel") or []) == 3 and len(g.get("land") or []) == 3,
             "grenade pos / vel / land need 3 numbers")
    for d in sc.get("deaths") or []:
        need(0 <= d.get("player", -1) < len(players), "death player index")
    return errs


# ---- demoparser2 adapter ---------------------------------------------------------------------------


TICK_PROPS = ["X", "Y", "Z", "yaw", "pitch", "health", "armor_value", "has_helmet", "has_defuser", "balance", "team_num",
              "is_alive", "inventory"]


class Demo:
    def __init__(self, path):
        try:
            from demoparser2 import DemoParser  # noqa: WPS433 (optional dependency)
        except ImportError as e:  # pragma: no cover - depends on the host
            raise ConvertError("demoparser2 is not installed: pip install demoparser2 (%s)" % e)
        try:
            import importlib.metadata as md
            self.parser_version = "demoparser2 " + md.version("demoparser2")
        except Exception:  # pragma: no cover
            self.parser_version = "demoparser2"
        self.path = path
        self.p = DemoParser(path)
        self.header = self.p.parse_header()
        self._grenades = None

    def events(self, name, **kw):
        try:
            df = self.p.parse_event(name, **kw)
        except Exception:
            return []
        return df.to_dict("records") if hasattr(df, "to_dict") else list(df)

    def tickrate(self):
        # CS2 demos (GOTV and POV) run at the server's 64 ticks.
        return 64

    def rounds(self):
        fe = [r["tick"] for r in self.events("round_freeze_end")]
        ends = self.events("round_end")
        last = max([r["tick"] for r in ends] + fe + [0]) + 1
        wins = round_windows(fe, [r["tick"] for r in ends], last)
        info = []
        for n, s, e in wins:
            end_ev = next((r for r in ends if r["tick"] == e), {})
            info.append({"round": n, "freeze_end": s, "end": e, "winner": str(end_ev.get("winner") or ""),
                         "reason": str(end_ev.get("reason") or "")})
        return info

    def teams(self, tick):
        """{"T": name, "CT": name} from the team clan names at a tick, when the demo has them."""
        out = {}
        try:
            df = self.p.parse_ticks(["team_clan_name", "team_num"], ticks=[tick])
            for r in df.to_dict("records"):
                side = TEAMS.get(int(r.get("team_num") or 0))
                if side and r.get("team_clan_name"):
                    out.setdefault(side, str(r["team_clan_name"]))
        except Exception:
            pass
        return out

    def round_data(self, window, sample_ticks):
        _, fe, end = window
        ticks = list(range(fe, end + 1, sample_ticks))
        samples = self.p.parse_ticks(TICK_PROPS, ticks=ticks).to_dict("records")
        for s in samples:
            s["inventory"] = list(s.get("inventory") or [])
        if self._grenades is None:
            self._grenades = self.p.parse_grenades()
        g = self._grenades
        g = g[(g.tick >= fe) & (g.tick <= end) & g.grenade_type.str.endswith("Projectile")]
        projectiles = {}
        for r in g.dropna(subset=["x"]).to_dict("records"):
            p = projectiles.setdefault(int(r["grenade_entity_id"]), {"type": r["grenade_type"], "steamid": r["steamid"],
                                                                    "points": []})
            p["points"].append((int(r["tick"]), float(r["x"]), float(r["y"]), float(r["z"])))
        # Entity ids are reused: keep each id's points of its first flight (contiguous from the first tick).
        for p in projectiles.values():
            pts = sorted(p["points"])
            cut = len(pts)
            for i in range(1, len(pts)):
                if pts[i][0] - pts[i - 1][0] > 8:
                    cut = i
                    break
            p["points"] = pts[:cut]
        thrown = [{"tick": r["tick"], "steamid": r.get("user_steamid"), "weapon": r.get("weapon")}
                  for r in self.events("grenade_thrown") if fe <= r["tick"] <= end]
        det = {}
        for ev in ("smokegrenade_detonate", "flashbang_detonate", "hegrenade_detonate", "decoy_started"):
            for r in self.events(ev):
                if fe <= r["tick"] <= end and r.get("entityid") is not None:
                    det.setdefault(int(r["entityid"]), (int(r["tick"]), float(r["x"]), float(r["y"]), float(r["z"])))
        deaths = [{"tick": r["tick"], "victim": r.get("user_steamid"), "attacker": r.get("attacker_steamid"),
                   "weapon": r.get("weapon")} for r in self.events("player_death") if fe <= r["tick"] <= end]
        bomb = []
        for ev, name in (("bomb_planted", "planted"), ("bomb_defused", "defused"), ("bomb_exploded", "exploded")):
            for r in self.events(ev):
                if fe <= r["tick"] <= end:
                    bomb.append({"tick": r["tick"], "event": name, "steamid": r.get("user_steamid")})
        return samples, projectiles, thrown, det, deaths, bomb


def slug(s):
    s = re.sub(r"[^a-z0-9]+", "_", str(s).lower()).strip("_")
    return s[:64] or "scenario"


def cmd_rounds(args):
    d = Demo(args.demo)
    print("%s  map %s" % (os.path.basename(args.demo), d.header.get("map_name", "?")))
    for r in d.rounds():
        secs = (r["end"] - r["freeze_end"]) / 64.0
        print("round %2d  ticks %7d-%-7d  %5.1f s  winner %-2s  %s" % (r["round"], r["freeze_end"], r["end"], secs,
                                                                     r["winner"], r["reason"]))
    return 0


def cmd_convert(args):
    d = Demo(args.demo)
    rounds = {r["round"]: r for r in d.rounds()}
    wanted = []
    for spec in args.round:
        for part in str(spec).split(","):
            if part.strip():
                wanted.append(int(part))
    if not wanted:
        raise ConvertError("--round is required (see `rounds`)")
    mp = d.header.get("map_name") or ""
    os.makedirs(args.out, exist_ok=True)
    written = []
    for n in wanted:
        r = rounds.get(n)
        if not r:
            raise ConvertError("no round %d in this demo (1-%d)" % (n, len(rounds)))
        window = (n, r["freeze_end"], r["end"])
        tickrate = d.tickrate()
        start = parse_start(args.start, tickrate, r["freeze_end"]) if args.start else 0
        teams = d.teams(r["freeze_end"])
        base = args.id or slug("%s_%s_r%d" % (mp.replace("de_", ""), os.path.splitext(os.path.basename(args.demo))[0], n))
        sid = base if len(wanted) == 1 else slug("%s_r%d" % (base, n)) if args.id else base
        meta = {
            "id": sid, "map": mp, "tickrate": tickrate, "round": n,
            "title": args.title or "%s round %d%s" % (mp, n, (" (%s vs %s)" % (teams.get("T"), teams.get("CT"))) if teams else ""),
            "teams": teams, "winner": r["winner"], "reason": r["reason"], "event": args.event or "",
            "match": args.match or "", "url": args.url or "", "demo": os.path.basename(args.demo),
            "note": args.note or "", "parser": d.parser_version,
        }
        samples, projectiles, thrown, det, deaths, bomb = d.round_data(window, args.sample)
        sc = build_scenario(meta, window, samples, projectiles, thrown, det, deaths, bomb, args.sample, start)
        errs = validate(sc)
        if errs and not (errs == ["source needs event or match (attribution) and url"] and not args.strict):
            raise ConvertError("round %d: %s" % (n, "; ".join(errs)))
        path = os.path.join(args.out, sc["id"] + ".json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(sc, f, separators=(",", ":"))
            f.write("\n")
        written.append(path)
        print("wrote %s (%s, round %d, %d players, %d grenades, %.1f s)" % (
            path, mp, n, len(sc["players"]), len(sc["grenades"]), sc["length"] / float(tickrate)))
    return 0


def cmd_validate(args):
    bad = 0
    for path in args.files:
        with open(path, encoding="utf-8") as f:
            sc = json.load(f)
        errs = validate(sc)
        if errs:
            bad += 1
            print("%s: INVALID: %s" % (path, "; ".join(errs)))
        else:
            print("%s: ok (%s, %s, %d players, %d grenades)" % (path, sc["id"], sc["map"], len(sc["players"]),
                                                               len(sc["grenades"])))
    return 1 if bad else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="CS2 demo round -> Ready Up practice scenario (docs/SCENARIOS.md)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("rounds", help="list the rounds of a demo")
    r.add_argument("demo")
    c = sub.add_parser("convert", help="write one scenario per round")
    c.add_argument("demo")
    c.add_argument("--round", action="append", default=[], help="round number(s): --round 7 or --round 7,9")
    c.add_argument("--start", help="default start: seconds after freeze end (25, 25s, 1:05) or a demo tick (t12345)")
    c.add_argument("--out", default=".", help="output directory (the plugin reads readyup/plugins/practice/scenarios/)")
    c.add_argument("--id", help="scenario id ([a-z0-9_-], default <map>_<demo>_r<round>)")
    c.add_argument("--title")
    c.add_argument("--event", help="attribution: event name")
    c.add_argument("--match", help="attribution: match (e.g. 'Vitality vs Spirit, map 2')")
    c.add_argument("--url", help="attribution: where the demo came from")
    c.add_argument("--note", help="free text kept in source.note")
    c.add_argument("--sample", type=int, default=DEFAULT_SAMPLE_TICKS, help="ticks between path samples (default 4)")
    c.add_argument("--strict", action="store_true", help="require attribution (event/match + url), as seeds do")
    v = sub.add_parser("validate", help="check scenario files against the schema")
    v.add_argument("files", nargs="+")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "rounds":
            return cmd_rounds(args)
        if args.cmd == "convert":
            if not 1 <= args.sample <= 64:
                raise ConvertError("--sample must be 1..64")
            return cmd_convert(args)
        return cmd_validate(args)
    except ConvertError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
