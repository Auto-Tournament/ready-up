"""Tests for tools/scenario/ru_scenario.py (no demoparser2 needed).   Run: python3 -m unittest discover -s tests -v"""
import glob
import json
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "scenario"))

import ru_scenario as rs  # noqa: E402

FE = 1000  # freeze end tick
META = {"id": "test_r3", "map": "de_nuke", "tickrate": 64, "title": "t", "teams": {"T": "Alpha", "CT": "Bravo"},
        "event": "Cup", "match": "Alpha vs Bravo", "url": "https://example.org", "winner": "T", "reason": "ct_killed"}


def sample(tick, sid, name, team, x, alive=True, hp=100, inv=("Karambit", "Glock-18")):
    return {"tick": tick, "steamid": sid, "name": name, "team_num": team, "X": x, "Y": 0.0, "Z": -416.0, "yaw": 90.0,
            "pitch": 1.0, "is_alive": alive, "health": hp, "armor_value": 100, "has_helmet": False,
            "has_defuser": False, "balance": 800, "inventory": list(inv)}


def samples():
    out = []
    for k in range(0, 5):  # ticks 1000, 1004, ..., 1016
        t = FE + 4 * k
        out.append(sample(t, "1", "zywoo", 2, 10.0 * k))
        out.append(sample(t, "2", "Apex", 2, 5.0, alive=k < 3, hp=100 if k < 2 else 40))
        out.append(sample(t, "3", "donk", 3, -3.0 * k, inv=("Butterfly Knife", "USP-S", "Smoke Grenade")))
        out.append(sample(t, "4", "spec", 1, 0.0))  # spectator / coach: not a player
    return out


def projectile(t0, pts):
    # A grenade flying +x at 640 u/s, falling with 320 u/s^2.
    out = []
    for i in range(pts):
        dt = i / 64.0
        out.append((t0 + i, 100 + 640 * dt, 0.0, 50 + 150 * dt - 160 * dt * dt))
    return out


class HelperTest(unittest.TestCase):
    def test_item_class(self):
        self.assertEqual(rs.item_class("AK-47"), "weapon_ak47")
        self.assertEqual(rs.item_class("M4A1-S"), "weapon_m4a1_silencer")
        self.assertEqual(rs.item_class("High Explosive Grenade"), "weapon_hegrenade")
        self.assertEqual(rs.item_class("Karambit"), "weapon_knife")
        self.assertEqual(rs.item_class("M9 Bayonet"), "weapon_knife")
        self.assertIsNone(rs.item_class("Something New"))
        self.assertEqual(rs.items_from_inventory(["Glock-18", "Kukri Knife", "Flashbang", "Flashbang"]),
                         ["weapon_knife", "weapon_flashbang", "weapon_flashbang", "weapon_glock"])

    def test_parse_start(self):
        self.assertEqual(rs.parse_start("25", 64, FE), 1600)
        self.assertEqual(rs.parse_start("25s", 64, FE), 1600)
        self.assertEqual(rs.parse_start("1:05", 64, FE), 65 * 64)
        self.assertEqual(rs.parse_start("t1500", 64, FE), 500)
        self.assertEqual(rs.parse_start("1500t", 64, FE), 500)
        with self.assertRaises(rs.ConvertError):
            rs.parse_start("soon", 64, FE)

    def test_round_windows(self):
        # Freeze ends at 100, 500, 900; round ends at 400, 800; the last round has no end event.
        self.assertEqual(rs.round_windows([500, 100, 900], [400, 800], 1200),
                         [(1, 100, 400), (2, 500, 800), (3, 900, 1200)])

    def test_estimate_velocity(self):
        v = rs.estimate_velocity(projectile(0, 6), 64)
        self.assertAlmostEqual(v[0], 640, delta=0.5)
        self.assertAlmostEqual(v[1], 0, delta=0.5)
        self.assertAlmostEqual(v[2], 150, delta=0.5)
        self.assertIsNone(rs.estimate_velocity([(0, 1, 2, 3)], 64))


class BuildScenarioTest(unittest.TestCase):
    def build(self, **kw):
        projectiles = {
            7: {"type": "CSmokeGrenadeProjectile", "steamid": "1", "points": projectile(FE + 6, 8)},
            8: {"type": "CMolotovProjectile", "steamid": "3", "points": projectile(FE + 9, 4)},
            9: {"type": "CHEGrenadeProjectile", "steamid": "4", "points": projectile(FE + 9, 4)},  # not a player
            10: {"type": "CFlashbangProjectile", "steamid": "1", "points": projectile(FE - 50, 4)},  # before the round
        }
        thrown = [{"tick": FE + 9, "steamid": "3", "weapon": "incgrenade"}]
        det = {7: (FE + 14, 300.0, 1.0, -410.0)}
        deaths = [{"tick": FE + 10, "victim": "2", "attacker": "3", "weapon": "usp_silencer"},
                  {"tick": FE + 11, "victim": "4", "attacker": "1", "weapon": "ak47"}]
        bomb = [{"tick": FE + 15, "event": "planted", "steamid": "1"}]
        args = dict(meta=META, window=(3, FE, FE + 16), samples=samples(), projectiles=projectiles, thrown=thrown,
                    detonations=det, deaths=deaths, bomb=bomb, sample_ticks=4, start=0)
        args.update(kw)
        return rs.build_scenario(**args)

    def test_players_and_tracks(self):
        sc = self.build()
        self.assertEqual(rs.validate(sc), [])
        self.assertEqual([p["name"] for p in sc["players"]], ["Apex", "zywoo", "donk"])  # T first, then CT; no spectator
        self.assertEqual([p["team"] for p in sc["players"]], ["T", "T", "CT"])
        zy = sc["players"][1]
        self.assertEqual(len(zy["track"]), 5 * 5)
        self.assertEqual(zy["track"][5:10], [10.0, 0, -416.0, 90.0, 1.0])
        self.assertIsNone(zy["alive_until"])
        apex = sc["players"][0]
        self.assertEqual(apex["alive_until"], 12)
        self.assertEqual(len(apex["track"]), 3 * 5)
        self.assertEqual([s["t"] for s in apex["state"]], [0, 8])  # hp 100 -> 40 at tick 8
        self.assertEqual(apex["state"][1]["hp"], 40)
        self.assertEqual(sc["players"][2]["state"][0]["items"], ["weapon_knife", "weapon_smokegrenade", "weapon_usp_silencer"])

    def test_grenades(self):
        sc = self.build()
        g = sc["grenades"]
        self.assertEqual([(x["t"], x["type"], x["player"]) for x in g], [(6, "smoke", 1), (9, "incgrenade", 2)])
        smoke = g[0]
        self.assertEqual(smoke["pos"], [100.0, 0, 50.0])
        self.assertAlmostEqual(smoke["vel"][0], 640, delta=1)
        self.assertEqual(smoke["land"], [300.0, 1.0, -410.0])  # from the detonate event
        self.assertEqual(smoke["land_t"], 14)
        self.assertEqual(g[1]["land_t"], 12)  # no event: the last projectile point

    def test_deaths_bomb_meta(self):
        sc = self.build(start=8)
        self.assertEqual(sc["deaths"], [{"t": 10, "player": 0, "killer": 2, "weapon": "usp_silencer"}])
        self.assertEqual(sc["bomb"][0]["event"], "planted")
        self.assertEqual(sc["bomb"][0]["player"], 1)
        self.assertEqual(len(sc["bomb"][0]["pos"]), 3)
        self.assertEqual(sc["default_start"], 8)
        self.assertEqual(sc["length"], 16)
        self.assertEqual(sc["freeze_end_tick"], FE)
        self.assertEqual(sc["source"]["teams"], {"T": "Alpha", "CT": "Bravo"})
        json.dumps(sc)  # serializable

    def test_no_players(self):
        with self.assertRaises(rs.ConvertError):
            self.build(samples=[s for s in samples() if s["team_num"] == 1])

    def test_position_at(self):
        sc = self.build()
        zy = sc["players"][1]
        self.assertEqual(rs.position_at(zy, 2, 4)[0], 5.0)
        self.assertIsNone(rs.position_at(zy, 17, 4))


class ValidateTest(unittest.TestCase):
    def test_rejects(self):
        sc = BuildScenarioTest("test_players_and_tracks").build()
        for mutate, needle in (
            (lambda s: s.update(format="x"), "format"),
            (lambda s: s.update(version=2), "version"),
            (lambda s: s.update(id="Bad Id"), "id"),
            (lambda s: s["players"][0].update(team="X"), "team"),
            (lambda s: s["players"][0]["track"].append(1), "track"),
            (lambda s: s["grenades"][0].update(type="nuke"), "grenade type"),
            (lambda s: s.update(source={"url": ""}), "source"),
        ):
            bad = json.loads(json.dumps(sc))
            mutate(bad)
            errs = rs.validate(bad)
            self.assertTrue(any(needle in e for e in errs), (needle, errs))

    def test_shipped_scenarios(self):
        files = sorted(glob.glob(str(ROOT / "plugins" / "practice" / "scenarios" / "*.json")))
        for path in files:
            with open(path, encoding="utf-8") as f:
                sc = json.load(f)
            self.assertEqual(rs.validate(sc), [], path)
            self.assertEqual(pathlib.Path(path).stem, sc["id"])
            # Attribution rule (docs/SCENARIOS.md): where it came from, always.
            self.assertTrue(sc["source"]["url"] and (sc["source"]["event"] or sc["source"]["match"]), path)
            self.assertLess(pathlib.Path(path).stat().st_size, 2 * 1024 * 1024, path)


if __name__ == "__main__":
    unittest.main()
