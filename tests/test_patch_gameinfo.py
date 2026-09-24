"""Tests for scripts/patch_gameinfo.py.   Run: python3 -m unittest discover -s tests -v"""
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import patch_gameinfo as pg  # noqa: E402

GAME = "csgo/readyup"

BASE = """"GameInfo"
{
\tFileSystem
\t{
\t\tSearchPaths
\t\t{
\t\t\tGame_LowViolence\tcsgo_lv // Perfect World content override
%s\t\t\tGame\tcsgo
\t\t\tGame\tcsgo_imported
\t\t\tGame\tcsgo_core
\t\t\tGame\tcore
\t\t}
\t}
}
"""
METAMOD = "                        Game    csgo/addons/metamod\n"


def game_lines(text):
    return [line.split("//")[0].split() for line in text.splitlines()
            if line.strip().startswith("Game") and not line.strip().startswith("Game_")]


def order(text):
    return [parts[1] for parts in game_lines(text)]


class ComputePatchTest(unittest.TestCase):
    def test_plain_inserts_before_csgo(self):
        out = pg.compute_patch(BASE % "", GAME)
        self.assertEqual(order(out), [GAME, "csgo", "csgo_imported", "csgo_core", "core"])

    def test_metamod_inserts_directly_after_metamod(self):
        out = pg.compute_patch(BASE % METAMOD, GAME)
        self.assertEqual(order(out)[:3], ["csgo/addons/metamod", GAME, "csgo"])
        # Keeps Metamod's indentation style.
        self.assertIn("                        Game\tcsgo/readyup\n", out)

    def test_moves_readyup_from_above_metamod_to_below(self):
        text = BASE % ("\t\t\tGame\tcsgo/readyup\n" + METAMOD)
        out = pg.compute_patch(text, GAME)
        self.assertEqual(order(out)[:3], ["csgo/addons/metamod", GAME, "csgo"])
        self.assertEqual(order(out).count(GAME), 1)

    def test_readyup_already_after_metamod_is_left_alone(self):
        # The old patcher inserted a second line in exactly this case.
        text = BASE % (METAMOD + "\t\t\tGame\tcsgo/readyup\n")
        self.assertEqual(pg.compute_patch(text, GAME), text)

    def test_already_before_csgo_is_left_alone(self):
        text = BASE % "\t\t\tGame    csgo/readyup // ReadyUp\n"
        self.assertEqual(pg.compute_patch(text, GAME), text)

    def test_idempotent(self):
        for extra in ("", METAMOD):
            once = pg.compute_patch(BASE % extra, GAME)
            self.assertEqual(pg.compute_patch(once, GAME), once)

    def test_readyup_after_csgo_is_moved(self):
        text = (BASE % "").replace("\t\t\tGame\tcore\n", "\t\t\tGame\tcore\n\t\t\tGame\tcsgo/readyup\n")
        out = pg.compute_patch(text, GAME)
        self.assertEqual(order(out), [GAME, "csgo", "csgo_imported", "csgo_core", "core"])

    def test_duplicates_are_collapsed(self):
        text = BASE % ("\t\t\tGame\tcsgo/readyup\n" + METAMOD + "\t\t\tGame\tcsgo/readyup\n")
        out = pg.compute_patch(text, GAME)
        self.assertEqual(order(out).count(GAME), 1)
        self.assertEqual(order(out)[:3], ["csgo/addons/metamod", GAME, "csgo"])

    def test_metamod_below_csgo_is_ignored(self):
        # A Metamod line below `Game csgo` never loads; Ready Up still has to precede csgo.
        text = (BASE % "").replace("\t\t\tGame\tcore\n", "\t\t\tGame\tcore\n" + METAMOD)
        out = pg.compute_patch(text, GAME)
        self.assertEqual(order(out)[0], GAME)

    def test_crlf_preserved(self):
        text = (BASE % "").replace("\n", "\r\n")
        out = pg.compute_patch(text, GAME)
        self.assertIn("\tGame\tcsgo/readyup\r\n", out)
        self.assertNotIn("\n", out.replace("\r\n", ""))

    def test_no_csgo_line_uses_searchpaths_block(self):
        text = "SearchPaths\n{\n\t\tGame\tcore\n}\n"
        out = pg.compute_patch(text, GAME)
        self.assertEqual(order(out), [GAME, "core"])

    def test_nothing_to_patch_returns_none(self):
        self.assertIsNone(pg.compute_patch('"GameInfo" { }\n', GAME))


class CliTest(unittest.TestCase):
    def run_cli(self, path, *args):
        return subprocess.run([sys.executable, str(ROOT / "scripts" / "patch_gameinfo.py"), str(path), *args],
                              capture_output=True, text=True)

    def test_check_patch_backup_and_rerun(self):
        with tempfile.TemporaryDirectory() as d:
            gi = pathlib.Path(d) / "gameinfo.gi"
            gi.write_text(BASE % METAMOD)
            self.assertEqual(self.run_cli(gi, "--game", GAME, "--check").returncode, 3)
            self.assertEqual(gi.read_text(), BASE % METAMOD)  # --check never writes

            self.assertEqual(self.run_cli(gi, "--game", GAME).returncode, 0)
            self.assertEqual(order(gi.read_text())[:3], ["csgo/addons/metamod", GAME, "csgo"])
            self.assertEqual(len(list(pathlib.Path(d).glob("gameinfo.gi.bak.*"))), 1)

            r = self.run_cli(gi, "--game", GAME)
            self.assertEqual(r.returncode, 0)
            self.assertIn("Already present", r.stdout)
            self.assertEqual(len(list(pathlib.Path(d).glob("gameinfo.gi.bak.*"))), 1)


if __name__ == "__main__":
    unittest.main()
