"""Tests for scripts/ci/compat-report.py.   Run: python3 -m unittest discover -s tests -v"""
import http.server
import importlib.util
import json
import os
import pathlib
import shutil
import tempfile
import threading
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
FIX = ROOT / "tests" / "fixtures" / "compat"

_spec = importlib.util.spec_from_file_location("compat_report", ROOT / "scripts" / "ci" / "compat-report.py")
cr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cr)

RUN = ["--run-id", "123", "--run-url", "https://example.invalid/run/123", "--trigger", "build_change",
       "--started-at", "2026-09-25T22:00:00Z", "--commit", "7f61b71ee5cecde6a1ec33ac7f9c1f03eff7e038",
       "--version-file", str(ROOT / "VERSION")]
ISO = r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$"


def assert_contract(tc, doc):
    """The exact compat.json contract the platform builds against (docs/CS2-COMPAT.md)."""
    tc.assertEqual(set(doc), {"schema", "cs2", "readyup", "run", "overall", "components", "checked_at"})
    tc.assertEqual(doc["schema"], 1)
    tc.assertEqual(set(doc["cs2"]), {"buildid", "patch"})
    tc.assertIsInstance(doc["cs2"]["buildid"], str)
    tc.assertIsInstance(doc["cs2"]["patch"], str)
    tc.assertEqual(set(doc["readyup"]), {"version", "commit"})
    run = doc["run"]
    tc.assertEqual(set(run), {"id", "url", "trigger", "stage", "state", "started_at", "finished_at"})
    tc.assertIsInstance(run["id"], str)
    tc.assertIn(run["trigger"], ("build_change", "surface_change", "nightly", "release", "manual"))
    tc.assertIn(run["stage"], ("static", "selftest", "live"))
    tc.assertIn(run["state"], ("queued", "checking", "pass", "warn", "fail", "no_verdict"))
    tc.assertRegex(run["started_at"], ISO)
    if run["finished_at"] is not None:
        tc.assertRegex(run["finished_at"], ISO)
    tc.assertIn(doc["overall"], ("pass", "warn", "fail", "checking", "no_verdict"))
    tc.assertRegex(doc["checked_at"], ISO)
    for c in doc["components"]:
        tc.assertEqual(set(c), {"id", "name", "status", "checks"})
        tc.assertIn(c["status"], ("pass", "warn", "fail", "pending", "checking"))
        for k in c["checks"]:
            tc.assertEqual(set(k), {"kind", "status", "passed", "total", "failures"})
            tc.assertIn(k["kind"], ("signature", "rtti", "vtable", "hook_site", "layout", "schema", "event",
                                    "selftest", "livetest"))
            tc.assertIn(k["status"], ("pass", "warn", "fail", "pending"))
            tc.assertIsInstance(k["passed"], int)
            tc.assertIsInstance(k["total"], int)
            tc.assertTrue(all(isinstance(f, str) for f in k["failures"]))


class CompatReportTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.gamedata = self.tmp / "gamedata"
        self.gamedata.mkdir()
        for f in ("engine-surface.json", "engine-surface.skins.json"):
            shutil.copy(FIX / f, self.gamedata / f)

    def raw(self, sig="sigcheck-pass.txt", hook="hookcheck-pass.txt", sig_rc=0, hook_rc=0):
        d = self.tmp / "raw"
        d.mkdir(exist_ok=True)
        (d / "sigcheck.txt").write_text((FIX / sig).read_text() if sig else "")
        (d / "hookcheck.txt").write_text((FIX / hook).read_text() if hook else "")
        (d / "rc.env").write_text("SIGCHECK_RC=%d\nHOOKCHECK_RC=%d\n" % (sig_rc, hook_rc))
        return d

    def result(self, **kw):
        out = self.tmp / "out"
        rc = cr.main(["result", "--raw-dir", str(self.raw(**kw)), "--gamedata", str(self.gamedata),
                      "--build-env", str(FIX / "cs2-build.env"), "--out-dir", str(out)] + RUN)
        self.assertEqual(rc, 0)
        doc = json.loads((out / "compat.json").read_text())
        assert_contract(self, doc)
        return doc, json.loads((out / "badge.json").read_text())

    @staticmethod
    def comp(doc, cid):
        return next(c for c in doc["components"] if c["id"] == cid)

    @staticmethod
    def check(comp, kind):
        return next(k for k in comp["checks"] if k["kind"] == kind)

    def test_static_pass_is_warn_live_pending(self):
        doc, badge = self.result()
        self.assertEqual(doc["cs2"], {"buildid": "25537370", "patch": "1.41.8.5"})
        self.assertEqual(doc["readyup"]["commit"], "7f61b71ee5cecde6a1ec33ac7f9c1f03eff7e038")
        self.assertTrue(doc["readyup"]["version"].endswith("-dev.7f61b71"))
        self.assertEqual(doc["run"]["stage"], "static")
        self.assertEqual(doc["run"]["state"], "warn")
        self.assertEqual(doc["overall"], "warn")
        self.assertIsNotNone(doc["run"]["finished_at"])
        ids = [c["id"] for c in doc["components"]]
        self.assertEqual(ids, ["core", "skins", "match", "practice", "essentials", "midas", "whitelist", "deathmatch", "fleet"])
        core, skins = self.comp(doc, "core"), self.comp(doc, "skins")
        self.assertEqual((core["name"], core["status"]), ("Core", "pass"))
        self.assertEqual(self.check(core, "signature"), {"kind": "signature", "status": "pass", "passed": 2,
                                                         "total": 2, "failures": []})
        # SKIP (runtime-only module) is not counted
        self.assertEqual((self.check(core, "rtti")["passed"], self.check(core, "rtti")["total"]), (1, 1))
        self.assertEqual(self.check(core, "vtable")["total"], 1)
        self.assertEqual(self.check(core, "hook_site")["total"], 1)
        self.assertEqual(self.check(core, "layout")["total"], 1)
        self.assertEqual(self.check(core, "selftest")["status"], "pending")
        self.assertEqual(skins["status"], "pass")
        self.assertEqual(self.check(skins, "signature")["total"], 2)
        self.assertEqual(self.check(skins, "vtable")["total"], 1)
        for cid in cr.RUNTIME_ONLY:
            c = self.comp(doc, cid)
            self.assertEqual(c["status"], "pending")
            self.assertEqual([k["status"] for k in c["checks"]], ["pending"])
        self.assertEqual(badge, {"schemaVersion": 1, "label": "CS2 1.41.8.5", "message": "static ok",
                                 "color": "yellow"})

    def test_fragment_failure_fails_only_its_component(self):
        doc, badge = self.result(sig="sigcheck-skins-fail.txt", sig_rc=1)
        self.assertEqual(self.comp(doc, "core")["status"], "pass")
        skins = self.comp(doc, "skins")
        self.assertEqual(skins["status"], "fail")
        sig = self.check(skins, "signature")
        self.assertEqual((sig["status"], sig["passed"], sig["total"]), ("fail", 1, 2))
        self.assertEqual(len(sig["failures"]), 1)
        self.assertTrue(sig["failures"][0].startswith("CBaseModelEntity_SetModel: optional matches=0"))
        self.assertEqual(doc["overall"], "fail")
        self.assertEqual(doc["run"]["state"], "fail")
        self.assertEqual((badge["message"], badge["color"]), ("incompatible", "red"))

    def test_core_rtti_and_hook_failure(self):
        sig = (FIX / "sigcheck-pass.txt").read_text().replace(
            "OK   rtti   CBaseEntity                            11CBaseEntity  name ok; vtable at 0x2a1b3c8",
            "FAIL rtti   CBaseEntity                            11CBaseEntity  class name not in binary")
        hook = (FIX / "hookcheck-pass.txt").read_text().replace(
            "OK   Host_Say                                 rva=0x1c3f0c0 prologue relocates",
            "FAIL Host_Say                                 rva=0x1c3f0c0 funchook_prepare=5: too short")
        d = self.raw()
        (d / "sigcheck.txt").write_text(sig)
        (d / "hookcheck.txt").write_text(hook)
        (d / "rc.env").write_text("SIGCHECK_RC=1\nHOOKCHECK_RC=1\n")
        out = self.tmp / "out2"
        cr.main(["result", "--raw-dir", str(d), "--gamedata", str(self.gamedata), "--build-env",
                 str(FIX / "cs2-build.env"), "--out-dir", str(out)] + RUN)
        doc = json.loads((out / "compat.json").read_text())
        assert_contract(self, doc)
        core = self.comp(doc, "core")
        self.assertEqual(core["status"], "fail")
        self.assertEqual(self.check(core, "rtti")["status"], "fail")
        self.assertEqual(self.check(core, "hook_site")["failures"][0][:9], "Host_Say:")
        self.assertEqual(self.comp(doc, "skins")["status"], "pass")
        self.assertEqual(doc["overall"], "fail")

    def test_abnormal_exit_without_fail_lines_is_a_failure(self):
        doc, badge = self.result(sig=None, hook=None, sig_rc=2, hook_rc=2)
        core = self.comp(doc, "core")
        self.assertEqual(core["status"], "fail")
        self.assertIn("readyup_sigcheck exited 2", self.check(core, "signature")["failures"][0])
        self.assertIn("readyup_hookcheck exited 2", self.check(core, "hook_site")["failures"][0])
        self.assertEqual(badge["color"], "red")

    def test_missing_raw_output_is_not_a_pass(self):
        out = self.tmp / "out3"
        empty = self.tmp / "empty"
        empty.mkdir()
        cr.main(["result", "--raw-dir", str(empty), "--gamedata", str(self.gamedata), "--out-dir", str(out),
                 "--buildid", "1", "--patch", "1.0"] + RUN)
        doc = json.loads((out / "compat.json").read_text())
        assert_contract(self, doc)
        self.assertEqual(doc["overall"], "fail")
        self.assertEqual(doc["cs2"], {"buildid": "1", "patch": "1.0"})

    def test_overall_rules(self):
        passed = {"id": "core", "name": "Core", "status": "pass",
                  "checks": [{"kind": "signature", "status": "pass", "passed": 1, "total": 1, "failures": []}]}
        live_pending = dict(passed, checks=passed["checks"] + [cr.pending_check()])
        pending = {"id": "match", "name": "Match", "status": "pending", "checks": [cr.pending_check()]}
        failed = dict(passed, status="fail")
        self.assertEqual(cr.overall_status([passed]), "pass")
        self.assertEqual(cr.overall_status([live_pending]), "warn")
        self.assertEqual(cr.overall_status([passed, pending]), "warn")
        self.assertEqual(cr.overall_status([passed, pending, failed]), "fail")
        self.assertEqual(cr.component_status(live_pending["checks"]), "pass")
        self.assertEqual(cr.component_status([cr.pending_check()]), "pending")
        self.assertEqual(cr.badge({"overall": "pass", "cs2": {"patch": "1.41.8.5", "buildid": "1"}}),
                         {"schemaVersion": 1, "label": "CS2 1.41.8.5", "message": "compatible",
                          "color": "brightgreen"})

    def test_phase_events(self):
        for state, overall, comp_status in (("queued", "checking", "checking"),
                                            ("checking", "checking", "checking"),
                                            ("no_verdict", "no_verdict", "pending")):
            out = self.tmp / ("%s.json" % state)
            self.assertEqual(cr.main(["phase", state, "--out", str(out), "--gamedata", str(self.gamedata),
                                      "--buildid", "25537371"] + RUN), 0)
            doc = json.loads(out.read_text())
            assert_contract(self, doc)
            self.assertEqual((doc["run"]["state"], doc["overall"]), (state, overall))
            self.assertEqual(doc["cs2"], {"buildid": "25537371", "patch": ""})
            self.assertEqual(len(doc["components"]), 9)
            self.assertTrue(all(c["status"] == comp_status and c["checks"] == [] for c in doc["components"]))
            if state == "no_verdict":
                self.assertRegex(doc["run"]["finished_at"], ISO)
            else:
                self.assertIsNone(doc["run"]["finished_at"])

    def test_real_gamedata_all_ok(self):
        """Every entry of the repo's own gamedata is attributed to core or its fragment."""
        files = cr.surface_files(str(ROOT / "gamedata"))
        self.assertEqual(files[0][0], "core")
        owner, hooked = cr.ownership(files)
        sig, hook = [], []
        for (kind, name), _ in sorted(owner.items()):
            if kind == "signature":
                sig.append("OK   %-45s optional matches=1 rva=0x1  ok" % name)
            elif kind in ("rtti", "vtable", "layout"):
                sig.append("OK   %-6s %-38s ok" % (kind, name))
        hook = ["OK   %-40s rva=0x1 prologue relocates" % n for n in sorted(hooked)]
        comps = cr.build_components("\n".join(sig), "\n".join(hook), 0, 0, files)
        self.assertEqual(cr.overall_status(comps), "warn")
        total = sum(k["total"] for c in comps for k in c["checks"])
        self.assertEqual(total, len(owner))
        for c in comps:
            self.assertIn(c["status"], ("pass", "pending"))


class _Handler(http.server.BaseHTTPRequestHandler):
    received = []

    def do_POST(self):  # noqa: N802
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        _Handler.received.append((self.headers.get("Authorization"), json.loads(body)))
        self.send_response(202)
        self.end_headers()

    def log_message(self, *a):
        pass


class PostTest(unittest.TestCase):
    def setUp(self):
        fd, self.file = tempfile.mkstemp(suffix=".json")
        os.write(fd, b'{"schema": 1}')
        os.close(fd)
        self.addCleanup(os.unlink, self.file)

    def test_skipped_without_url(self):
        with mock.patch.dict(os.environ, {"COMPAT_INGEST_URL": ""}), \
                mock.patch("urllib.request.urlopen") as op:
            self.assertEqual(cr.main(["post", self.file]), 0)
            op.assert_not_called()

    def test_posts_with_bearer(self):
        _Handler.received.clear()
        srv = http.server.HTTPServer(("127.0.0.1", 0), _Handler)
        t = threading.Thread(target=srv.handle_request, daemon=True)
        t.start()
        env = {"COMPAT_INGEST_URL": "http://127.0.0.1:%d/ingest" % srv.server_port,
               "COMPAT_INGEST_TOKEN": "t0k", "NO_PROXY": "127.0.0.1", "no_proxy": "127.0.0.1"}
        with mock.patch.dict(os.environ, env):
            self.assertEqual(cr.main(["post", self.file]), 0)
        t.join(5)
        srv.server_close()
        self.assertEqual(_Handler.received, [("Bearer t0k", {"schema": 1})])

    def test_failure_is_ignored(self):
        srv = http.server.HTTPServer(("127.0.0.1", 0), _Handler)
        port = srv.server_port
        srv.server_close()  # nothing listens there any more
        env = {"COMPAT_INGEST_URL": "http://127.0.0.1:%d/" % port, "NO_PROXY": "127.0.0.1",
               "no_proxy": "127.0.0.1"}
        with mock.patch.dict(os.environ, env):
            self.assertEqual(cr.main(["post", self.file, "--timeout", "2"]), 0)


if __name__ == "__main__":
    unittest.main()
