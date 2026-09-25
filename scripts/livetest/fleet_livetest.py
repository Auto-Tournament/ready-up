#!/usr/bin/env python3
"""Fleet step-3 live test: a mock platform assigns a bot match to the readyup-test server.

  scripts/livetest/fleet_livetest.py [--play-out] [--no-offline] [--save-examples DIR]

Runs on the cs2 box as sivert (like livetest.py, whose console helpers it uses):

  1. starts scripts/livetest/fleet_mock_platform.py in Docker (python:3.12-slim, host network,
     127.0.0.1:18095-18097); it validates every frame against plugins/fleet/protocol/v1,
  2. points fleet.so at it (csgo/cfg/ReadyUp/fleet.cfg: url, insecure_dev, a one-time code,
     offline_pause_minutes=1) and reloads it: enroll + hello,
  3. match.assign (epoch 2, empty rosters, knife, 4 rounds): cmd.result ok, state.snapshot, bots
     play (empty roster = everyone ready), phase events warmup -> knife -> side_pick -> live,
     round_start / round_end (RoundSummary) / backup (inline round backup) / patches,
  4. fencing: a second match.assign (busy), a cmd with epoch 1 (stale_epoch), another match_id
     (not_assigned), match.update with a wrong base (conflict + snapshot) and a right one (ok, rev 2),
  5. cmd pause / unpause (acks + event.pause), exec (root, audit id echoed; refused without root),
  6. D12: the mock refuses connections for 75 s -> auto-pause at 60 s offline, the spooled
     event.pause {type: offline} arrives after the reconnect, cmd unpause,
  7. round backups arrive inline (sha256 checked) and are restored with cmd restore_round (local
     file, then the inline copy): rounds_voided + match_restored; a wrong sha256 is refused,
  8. cmd end_match (or --play-out: map_result with MapStats + series_end), match.unassign,
  9. checks: no schema errors, cmd.result per command (ref = its id), live_rev +1 per message,
     and the snapshots equal the state rebuilt from the assign snapshot + every patch.

Afterwards the server is standalone again: fleet.cfg and fleet.so's data dir are moved to
~/readyup-test/.fleet-livetest/<time>/, fleet.so reloaded, `ru idle`, `ru scrim`, bot_quota back.
Exit 0 PASS, 1 FAIL, 2 no verdict (server busy / down).
"""
import argparse
import json
import os
import shlex
import subprocess
import sys
import time
import urllib.request
from typing import Optional

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from livetest import STATE_RE, KV_RE, CRASH_RE, Server, log  # noqa: E402

MOCK_NAME = "ru-fleet-mock"
CODE = "RUE-7F3K-9QX2-LM4D-P8TW"


def merge_patch(target, patch):
    if not isinstance(patch, dict):
        return patch
    if not isinstance(target, dict):
        target = {}
    out = dict(target)
    for k, v in patch.items():
        if v is None:
            out.pop(k, None)
        else:
            out[k] = merge_patch(out.get(k), v)
    return out


class Mock:
    def __init__(self, a):
        self.a = a
        self.base = f"http://127.0.0.1:{a.ctl_port}"

    def start(self) -> None:
        subprocess.run(["docker", "rm", "-f", MOCK_NAME], capture_output=True)
        cmd = ("pip install -q --disable-pip-version-check --root-user-action=ignore websockets jsonschema && "
               f"exec python -u /mock.py --http {self.a.http_port} --ws {self.a.ws_port} --ctl {self.a.ctl_port} "
               "--schemas /proto/v1")
        subprocess.run(["docker", "run", "-d", "--rm", "--name", MOCK_NAME, "--network", "host",
                        "-v", f"{REPO}/plugins/fleet/protocol:/proto:ro",
                        "-v", f"{HERE}/fleet_mock_platform.py:/mock.py:ro",
                        "python:3.12-slim", "sh", "-c", cmd], check=True, capture_output=True)
        deadline = time.time() + 180
        while time.time() < deadline:
            try:
                self.get("/status")
                return
            except Exception:  # noqa: BLE001
                time.sleep(1)
        raise RuntimeError("mock platform did not start: " + self.logs())

    def logs(self) -> str:
        r = subprocess.run(["docker", "logs", "--tail", "60", MOCK_NAME], capture_output=True, text=True)
        return r.stdout + r.stderr

    def stop(self) -> None:
        subprocess.run(["docker", "rm", "-f", MOCK_NAME], capture_output=True)

    def get(self, path: str):
        with urllib.request.urlopen(self.base + path, timeout=10) as r:
            return json.loads(r.read())

    def post(self, path: str, body: dict):
        req = urllib.request.Request(self.base + path, data=json.dumps(body).encode(), method="POST",
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read())

    def send(self, type_: str, payload: dict, epoch: Optional[int] = None) -> str:
        body = {"type": type_, "payload": payload}
        if epoch:
            body["epoch"] = epoch
        return self.post("/send", body)["id"]


class Test:
    def __init__(self, a):
        self.a = a
        self.srv = Server(a.ssh, a.target, a.session)
        self.mock = Mock(a)
        self.frames = []
        self.follower = None
        self.lines = []
        self.results = []
        self.failures = []
        self.cfg_path = f"{a.target}/game/csgo/cfg/ReadyUp/fleet.cfg"
        self.data_dir = f"{a.target}/game/csgo/readyup/plugins/fleet"
        self.touched = False
        self.match_id = f"lt-{int(time.time())}"
        self.epoch = 2
        self.scrim_flag = False

    # ---- plumbing -------------------------------------------------------------------------
    def pump(self, wait: float = 0.3) -> None:
        if self.follower:
            for ln in self.follower.lines(wait):
                self.lines.append(ln)
                if CRASH_RE.search(ln):
                    raise RuntimeError("crash in the console log: " + ln)
        new = self.mock.get(f"/frames?since={len(self.frames)}")
        self.frames.extend(new)

    def frames_of(self, type_: str, since: int = 0, pred=None):
        return [f for f in self.frames[since:] if f["frame"].get("type") == type_ and (pred is None or pred(f["frame"]))]

    def wait_frame(self, type_: str, since: int, pred=None, timeout: float = 30.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.pump(0.3)
            got = self.frames_of(type_, since, pred)
            if got:
                return got[0]["frame"]
        return None

    def wait_line(self, needle: str, since: int, timeout: float) -> Optional[str]:
        deadline = time.time() + timeout
        while time.time() < deadline:
            for ln in self.lines[since:]:
                if needle in ln:
                    return ln
            self.pump(0.5)
        return None

    def step(self, name: str, ok: bool, detail: str = "") -> bool:
        ok = bool(ok)
        detail = detail or ""
        self.results.append((name, "PASS" if ok else "FAIL", detail))
        log(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")
        if not ok:
            self.failures.append(name)
        return ok

    def cmd(self, name: str, args: dict, match: bool = True, epoch: Optional[int] = None, root: bool = False,
            audit: str = "", match_id: Optional[str] = None, timeout: float = 20.0):
        payload = {"name": name, "args": args, "issued_by": {"user_id": "u_lt", "name": "livetest", "root": root},
                   "expires_at": int(time.time() * 1000) + 60000}
        e = None
        if match:
            payload["match_id"] = match_id or self.match_id
            e = epoch if epoch is not None else self.epoch
            payload["epoch"] = e
        if audit:
            payload["audit_id"] = audit
        since = len(self.frames)
        mid = self.mock.send("cmd", payload, e)
        return self.wait_frame("cmd.result", since, lambda f: f.get("ref") == mid, timeout)

    def result_ok(self, res) -> bool:
        return bool(res) and res["payload"]["status"] == "ok"

    def err_code(self, res) -> str:
        return (res or {}).get("payload", {}).get("error", {}).get("code", "") if res else "(no cmd.result)"

    def state_line(self, since: int, pred, timeout: float) -> Optional[dict]:
        deadline = time.time() + timeout
        while time.time() < deadline:
            for ln in self.lines[since:]:
                m = STATE_RE.search(ln)
                if m:
                    kv = dict(KV_RE.findall(m.group(1)))
                    if pred(kv):
                        return kv
            self.pump(0.5)
        return None

    # ---- server config ----------------------------------------------------------------------
    def configure_fleet(self) -> None:
        cfg = (f"url=http://127.0.0.1:{self.a.http_port}\ninsecure_dev=1\nenroll_code={CODE}\n"
               "offline_pause_minutes=1\n")
        self.srv.run(f"test ! -e {shlex.quote(self.cfg_path)} || cp -p {shlex.quote(self.cfg_path)} "
                     f"{shlex.quote(self.cfg_path)}.pre-livetest; printf %s {shlex.quote(cfg)} > {shlex.quote(self.cfg_path)}",
                     check=True)
        self.touched = True

    def unconfigure_fleet(self) -> None:
        trash = f"{self.a.target}/.fleet-livetest/{time.strftime('%Y%m%d-%H%M%S')}"
        q = shlex.quote
        self.srv.run(f"mkdir -p {q(trash)}; test -e {q(self.cfg_path)} && mv {q(self.cfg_path)} {q(trash)}/; "
                     f"test -e {q(self.cfg_path)}.pre-livetest && mv {q(self.cfg_path)}.pre-livetest {q(self.cfg_path)}; "
                     f"test -d {q(self.data_dir)} && mv {q(self.data_dir)} {q(trash)}/fleet-data; true")
        log(f"fleet.cfg and fleet data moved to {trash}")

    # ---- the test ---------------------------------------------------------------------------
    def assign_payload(self, map_name: str) -> dict:
        r = self.a.max_rounds
        return {
            "match_id": self.match_id, "epoch": self.epoch, "config_rev": 1,
            "config": {
                "num_maps": 1,
                "maps": [{"number": 1, "name": map_name, "sides": "knife"}],
                "team1": {"id": "lt_a", "name": "LiveTestA", "players": []},
                "team2": {"id": "lt_b", "name": "LiveTestB", "players": []},
                "password": "",
                "rules": {"max_rounds": r, "overtime": {"enabled": False, "rounds_per_half": 3},
                          "knife": {"side_pick_seconds": 5}, "clinch_series": True},
                "cvars": {"mp_halftime": 1, "mp_halftime_duration": 3, "mp_freezetime": 1, "mp_roundtime": 1,
                          "mp_roundtime_defuse": 1, "mp_roundtime_hostage": 1, "mp_round_restart_delay": 2,
                          "mp_c4timer": 20, "mp_win_panel_display_time": 1, "mp_team_timeout_max": 0},
            },
        }

    def run(self) -> int:
        humans = self.srv.connected_humans()
        if not self.srv.session_alive():
            log("tmux session missing")
            return 2
        if humans and not self.a.force:
            log("humans connected: " + ", ".join(humans))
            return 2
        self.follower = self.srv.follow(self.srv.log_size())
        log("starting the mock platform (docker) ...")
        self.mock.start()
        try:
            return self.body()
        finally:
            self.cleanup()

    def body(self) -> int:
        a = self.a
        # ---- enroll + hello
        mark = len(self.lines)
        self.srv.send("ru idle")
        self.srv.send("bot_kick")
        self.configure_fleet()
        self.srv.send("ru plugin reload fleet")
        hello = self.wait_frame("hello", 0, timeout=60)
        st = self.mock.get("/status")
        if not self.step("enroll + hello", bool(hello) and st["connected"],
                         f"server {st['server_id']}, capabilities {hello and hello['payload'].get('capabilities')}"):
            return self.finish()
        caps = hello["payload"].get("capabilities", [])
        self.step("match.v1 capability", "match.v1" in caps, ",".join(caps))

        # ---- assign
        cur_map = ""
        for ln in reversed(self.lines):
            m = STATE_RE.search(ln)
            if m:
                cur_map = dict(KV_RE.findall(m.group(1))).get("map", "")
                if cur_map:
                    break
        map_name = a.map or cur_map or "de_dust2"
        if a.from_scrim:
            # D16: a bots-only scrim is running when the assignment arrives.
            mark = len(self.lines)
            self.scrim_flag = True
            self.srv.send("ru_dev_bots_scrim 1")
            self.srv.send("ru scrim")
            self.srv.send(f"bot_quota {2 * a.bots_per_side}")
            kv = self.state_line(mark, lambda kv: kv.get("match", "").startswith("scrim:"), 120)
            self.step("scrim running before the assignment", bool(kv), kv and f"mode={kv.get('mode')} match={kv.get('match')}")
        mark = len(self.lines)
        since = len(self.frames)
        t_assign = time.time()
        aid = self.mock.send("match.assign", self.assign_payload(map_name), self.epoch)
        res = self.wait_frame("cmd.result", since, lambda f: f.get("ref") == aid)
        self.step("match.assign ack", self.result_ok(res) and res.get("epoch") == self.epoch,
                  json.dumps(res and res["payload"]))
        snap = self.wait_frame("state.snapshot", since, lambda f: f["payload"].get("reason") == "assign")
        self.step("state.snapshot (assign)", bool(snap) and snap["payload"]["state"]["match_id"] == self.match_id
                  and snap.get("epoch") == self.epoch,
                  snap and f"phase={snap['payload']['state']['phase']} live_rev={snap['payload']['state']['live_rev']}")
        if a.from_scrim:
            loaded = self.wait_line(f"fleet: match {self.match_id} epoch {self.epoch} loaded", mark, 20)
            took = time.time() - t_assign
            ho = self.wait_line("hand-over: scrim ended", mark, 1)
            notice = self.wait_line("assigned a tournament match. Thanks for playing!", mark, 1)
            self.step("D16 hand-over: notice, scrim ended, load after 5 s", bool(ho and notice and loaded) and took >= 4.5,
                      f"{ho} | loaded {took:.1f} s after the assign")
            self.srv.send("ru_dev_bots_scrim cfg")
        time.sleep(3)  # the load kicks bots (bot_kick / bot_quota 0); add them after it
        self.srv.send(f"bot_quota {2 * a.bots_per_side}")

        # fencing while it warms up
        since = len(self.frames)
        other = self.assign_payload(map_name)
        other["match_id"] = self.match_id + "-other"
        other["epoch"] = 1
        bid = self.mock.send("match.assign", other, 1)
        res = self.wait_frame("cmd.result", since, lambda f: f.get("ref") == bid)
        self.step("second assign -> busy", self.err_code(res) == "busy", self.err_code(res))
        res = self.cmd("say", {"text": "fleet livetest"}, epoch=1)
        self.step("cmd with a lower epoch -> stale_epoch", self.err_code(res) == "stale_epoch", self.err_code(res))
        res = self.cmd("pause", {"type": "admin"}, match_id="not-this-one")
        self.step("cmd for another match -> not_assigned", self.err_code(res) == "not_assigned", self.err_code(res))
        since = len(self.frames)
        uid = self.mock.send("match.update", {"match_id": self.match_id, "epoch": self.epoch, "base_config_rev": 0,
                                              "config_rev": 2, "ops": [{"op": "rename_team", "team": "team1",
                                                                        "name": "LT Alpha"}]}, self.epoch)
        res = self.wait_frame("cmd.result", since, lambda f: f.get("ref") == uid)
        snapc = self.wait_frame("state.snapshot", since, lambda f: f["payload"].get("reason") == "request")
        self.step("match.update wrong base -> conflict", self.err_code(res) == "conflict" and
                  res["payload"].get("rev") == 1 and bool(snapc), f"{self.err_code(res)} rev={res and res['payload'].get('rev')}")
        since = len(self.frames)
        uid = self.mock.send("match.update", {"match_id": self.match_id, "epoch": self.epoch, "base_config_rev": 1,
                                              "config_rev": 2, "ops": [{"op": "rename_team", "team": "team1",
                                                                        "name": "LT Alpha"}]}, self.epoch)
        res = self.wait_frame("cmd.result", since, lambda f: f.get("ref") == uid)
        deadline = time.time() + 10
        renamed = False
        while time.time() < deadline and not renamed:
            self.pump(0.3)
            renamed = any("LT Alpha" in json.dumps(f["frame"]["payload"].get("patch", {})) for f in self.frames[since:])
        self.step("match.update CAS ok (+ patch)", self.result_ok(res) and res["payload"].get("rev") == 2 and renamed,
                  f"rev={res and res['payload'].get('rev')} renamed_in_patch={renamed}")
        res = self.cmd("exec", {"command": "echo fleet-livetest"}, match=False, root=False, audit="aud_lt_1")
        self.step("exec without root -> forbidden", self.err_code(res) == "forbidden" and
                  res["payload"].get("audit_id") == "aud_lt_1", self.err_code(res))
        res = self.cmd("exec", {"command": "echo fleet-livetest-exec"}, match=False, root=True, audit="aud_lt_2")
        self.step("exec (root) ok + audit id echoed", self.result_ok(res) and res["payload"].get("audit_id") == "aud_lt_2",
                  json.dumps(res and {k: v for k, v in res["payload"].items() if k != "output"}) +
                  f" output={len((res or {}).get('payload', {}).get('output', ''))}B")

        # ---- the match goes live
        live = self.wait_frame("event.phase", 0, lambda f: f["payload"]["data"]["to"] == "live", timeout=300)
        phases = [f["frame"]["payload"]["data"]["from"] + "->" + f["frame"]["payload"]["data"]["to"]
                  for f in self.frames_of("event.phase")]
        if not self.step("phase events to live", bool(live), " ".join(phases)):
            return self.finish()
        self.step("knife_result + side_picked", bool(self.frames_of("event.knife_result")) and
                  bool(self.frames_of("event.side_picked")),
                  json.dumps([f["frame"]["payload"]["data"] for f in self.frames_of("event.knife_result")]))
        rs = self.wait_frame("event.round_start", 0, timeout=120)
        re1 = self.wait_frame("event.round_end", 0, timeout=a.round_timeout)
        ok = bool(re1) and isinstance(re1["payload"]["data"]["round"].get("players"), list)
        self.step("round_start + round_end (RoundSummary)", bool(rs) and ok,
                  re1 and f"round {re1['payload']['data']['round']['round_number']} winner_team="
                          f"{re1['payload']['data']['round']['winner_team']} "
                          f"players={len(re1['payload']['data']['round']['players'])} rev={re1['payload']['rev']}")
        rnums = [f["frame"]["payload"]["data"]["round"] for f in self.frames_of("event.round_start")]
        self.step("round numbers start at 1 on a fresh match", bool(rnums) and rnums[0] == 1, f"round_start rounds {rnums}")

        # ---- pause / unpause (right after round 1: the match is live)
        res = self.cmd("pause", {"type": "admin"})
        pz = self.wait_frame("event.pause", 0, lambda f: f["payload"]["data"]["action"] == "paused")
        self.step("cmd pause -> ok + event.pause", self.result_ok(res) and bool(pz),
                  pz and json.dumps(pz["payload"]["data"]))
        res = self.cmd("pause", {"type": "admin"})
        self.step("pause when paused -> already_paused", self.err_code(res) == "already_paused", self.err_code(res))
        time.sleep(3)
        res = self.cmd("unpause", {})
        up = self.wait_frame("event.pause", 0, lambda f: f["payload"]["data"]["action"] == "unpaused")
        self.step("cmd unpause -> ok + event.pause", self.result_ok(res) and bool(up),
                  up and json.dumps(up["payload"]["data"]))
        res = self.cmd("unpause", {})
        self.step("unpause when not paused -> not_paused", self.err_code(res) == "not_paused", self.err_code(res))

        # ---- round backups: forwarded inline, then restored (local file, then inline)
        bk = self.wait_frame("event.backup", 0, timeout=a.round_timeout)
        if bk:  # the newest backup so far (a restore then really goes back)
            self.pump(2.0)
            bk = self.frames_of("event.backup")[-1]["frame"]
        bd = bk and bk["payload"]["data"]
        self.step("inline round backup (event.backup)", bool(bk),
                  bk and f"{bd['file']} round {bd['round']} {bd['size']} B sha256 {bd['sha256'][:12]} "
                         f"base64 {len(bd['data'])} chars")
        if bk:
            import base64
            import hashlib
            raw = base64.b64decode(bd["data"])
            self.step("backup sha256 matches its data", hashlib.sha256(raw).hexdigest() == bd["sha256"]
                      and len(raw) == bd["size"], f"{len(raw)} B")
            for how, args in (("local file", {"map_number": bd["map_number"], "round": bd["round"]}),
                              ("inline", {"map_number": bd["map_number"], "round": bd["round"], "backup": bd})):
                since = len(self.frames)
                mark = len(self.lines)
                res = self.cmd("restore_round", args)
                rv = self.wait_frame("event.rounds_voided", since)
                mr = self.wait_frame("event.match_restored", since)
                nxt = self.wait_frame("event.round_start", since, timeout=15)
                self.step(f"round counter after restore ({how})", bool(nxt) and nxt["payload"]["data"]["round"] == bd["round"]
                          and not self.frames_of("event.round_end", since),
                          f"next round_start {nxt and nxt['payload']['data']['round']}, want {bd['round']}; "
                          f"round_end since restore: {len(self.frames_of('event.round_end', since))}")
                self.step(f"restore_round ({how}) -> ok + rounds_voided + match_restored",
                          self.result_ok(res) and bool(rv) and bool(mr),
                          (self.err_code(res) or "ok") + (mr and f" sha256 {mr['payload']['data']['backup_sha256'][:12]}" or ""))
                eng = self.wait_line("Loaded server checkpoint", mark, 10)
                self.step(f"CS2 loaded the backup ({how})", bool(eng), eng or "no `Loaded server checkpoint` line")
                time.sleep(3)
                res = self.cmd("unpause", {})
                self.step(f"unpause after restore ({how})", self.result_ok(res), self.err_code(res) or "ok")
            bad = self.cmd("restore_round", {"map_number": bd["map_number"], "round": bd["round"],
                                             "backup": dict(bd, sha256="0" * 64)})
            self.step("restore with a wrong sha256 -> checksum", self.err_code(bad) == "checksum", self.err_code(bad))

        # ---- D12: platform unreachable -> auto-pause
        if not a.no_offline:
            mark = len(self.lines)
            self.mock.post("/offline", {"seconds": 75})
            line = self.wait_line("auto-paused match", mark, 120)
            self.step("offline 60 s -> auto-pause", bool(line), line or "no auto-pause line")
            off = self.wait_frame("event.pause", 0, lambda f: f["payload"]["data"].get("type") == "offline"
                                  and f["payload"]["data"]["action"] == "paused", timeout=90)
            self.step("spooled event.pause {offline} after reconnect", bool(off),
                      off and f"seq {off.get('seq')} rev {off['payload']['rev']}")
            res = self.cmd("unpause", {}, timeout=60)
            self.step("platform unpause after reconnect", self.result_ok(res), self.err_code(res) or "ok")

        # ---- end
        if a.play_out:
            mr = self.wait_frame("event.map_result", 0, timeout=a.round_timeout * (a.max_rounds + 2))
            se = self.wait_frame("event.series_end", 0, timeout=120)
            self.step("map_result (MapStats) + series_end", bool(mr) and bool(se),
                      mr and f"{mr['payload']['data']['team1_score']}-{mr['payload']['data']['team2_score']} "
                             f"players={len(mr['payload']['data']['stats']['players'])}")
        elif self.frames_of("event.series_end"):
            self.step("cmd end_match", True, "SKIP: the map already ended on its own (series_end seen)")
        else:
            res = self.cmd("end_match", {"reason": "livetest"})
            se = self.wait_frame("event.series_end", 0)
            self.step("cmd end_match -> ok + event.series_end", self.result_ok(res) and bool(se),
                      se and json.dumps(se["payload"]["data"]))
        since = len(self.frames)
        uid = self.mock.send("match.unassign", {"match_id": self.match_id, "epoch": self.epoch, "reason": "ended"},
                             self.epoch)
        res = self.wait_frame("cmd.result", since, lambda f: f.get("ref") == uid)
        av = self.wait_frame("server.availability", 0, lambda f: f["payload"]["availability"] == "available")
        self.step("match.unassign -> ok + available", self.result_ok(res) and bool(av), self.err_code(res) or "ok")
        res = self.cmd("pause", {"type": "admin"})
        self.step("cmd after unassign -> stale_epoch", self.err_code(res) == "stale_epoch", self.err_code(res))
        self.pump(2.0)
        return self.finish()

    # ---- checks over everything received -----------------------------------------------------
    def verify_stream(self) -> None:
        bad = [(f["frame"].get("type"), f["errors"]) for f in self.frames if f["errors"]]
        self.step("every frame matches the schemas", not bad, f"{len(self.frames)} frames" if not bad else json.dumps(bad[:5])[:600])
        # live_rev: +1 per state.patch / event.*, and patches rebuild the snapshots.
        state = None
        rev = None
        gaps, mismatches, checked = [], [], 0
        for f in self.frames:
            fr = f["frame"]
            t = fr.get("type", "")
            p = fr.get("payload", {})
            if t == "state.snapshot" and p.get("state") and p["state"].get("match_id") == self.match_id:
                if state is None or p.get("reason") == "assign":
                    state, rev = p["state"], p["state"]["live_rev"]
                    continue
                if p["state"]["live_rev"] == rev:
                    checked += 1
                    if json.dumps(state, sort_keys=True) != json.dumps(p["state"], sort_keys=True):
                        mismatches.append(rev)
            elif (t == "state.patch" or t.startswith("event.")) and state is not None and p.get("match_id") == self.match_id:
                if p["rev"] != rev + 1:
                    gaps.append((rev, p["rev"], t))
                rev = p["rev"]
                state = merge_patch(state, p["patch"])
        self.step("live_rev +1 per message", not gaps and rev is not None, f"last rev {rev}" if not gaps else str(gaps[:5]))
        self.step("patches rebuild the snapshots", not mismatches and checked > 0,
                  f"{checked} snapshot(s) compared" if not mismatches else f"mismatch at rev {mismatches[:5]}")
        # One cmd.result per platform message.
        results = self.frames_of("cmd.result")
        refs = [f["frame"].get("ref") for f in results]
        self.step("one cmd.result per command", len(refs) == len(set(refs)), f"{len(refs)} results")
        types = sorted({f["frame"].get("type") for f in self.frames})
        log("frame types: " + " ".join(types))
        if self.a.save_examples:
            os.makedirs(self.a.save_examples, exist_ok=True)
            seen = set()
            for f in self.frames:
                t = f["frame"].get("type")
                if t in seen or t in ("ping", "hello"):
                    continue
                seen.add(t)
                with open(os.path.join(self.a.save_examples, f"live.{t}.json"), "w") as fh:
                    json.dump(f["frame"], fh, indent=2)
                    fh.write("\n")
            log(f"saved {len(seen)} example frames to {self.a.save_examples}")

    def finish(self) -> int:
        try:
            self.pump(0.5)
        except Exception as e:  # noqa: BLE001
            log(f"pump: {e}")
        self.verify_stream()
        print()
        print("=" * 100)
        for name, status, detail in self.results:
            print(f"{name:<55} {status:<5} {(detail or '')[:200]}")
        print("=" * 100)
        verdict = "FAIL" if self.failures else "PASS"
        print(f"FLEET LIVETEST {verdict}" + (f" ({', '.join(self.failures)})" if self.failures else ""))
        if self.a.out:
            os.makedirs(self.a.out, exist_ok=True)
            with open(os.path.join(self.a.out, "frames.json"), "w") as fh:
                json.dump(self.frames, fh, indent=1)
            with open(os.path.join(self.a.out, "console.log"), "w") as fh:
                fh.write("\n".join(self.lines) + "\n")
            with open(os.path.join(self.a.out, "mock.log"), "w") as fh:
                fh.write(self.mock.logs())
        return 1 if self.failures else 0

    def cleanup(self) -> None:
        try:
            if self.scrim_flag:
                self.srv.send("ru_dev_bots_scrim cfg")
            if self.touched:
                self.srv.send("ru idle")
                self.srv.send("bot_kick")
                self.unconfigure_fleet()
                mark = len(self.lines)
                self.srv.send("ru plugin reload fleet")
                ok = self.wait_line("standalone (no [fleet] url)", mark, 20)
                log("fleet.so standalone again" if ok else "WARNING: fleet.so did not report standalone")
                self.srv.send("ru scrim")
                self.srv.send(f"bot_quota {self.a.restore_bot_quota}")
        except Exception as e:  # noqa: BLE001
            log(f"cleanup error: {e}")
        if self.a.out:
            os.makedirs(self.a.out, exist_ok=True)
            with open(os.path.join(self.a.out, "mock.log"), "w") as fh:
                fh.write(self.mock.logs())
        self.mock.stop()
        if self.follower:
            self.follower.close()


def parse_args(argv=None):
    env = os.environ.get
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--ssh", default=env("RU_SSH", "cs2servermanager@localhost"))
    p.add_argument("--target", default=env("RU_TARGET", "/home/cs2servermanager/readyup-test"))
    p.add_argument("--session", default=env("RU_SESSION", "ru-test"))
    p.add_argument("--map", default="")
    p.add_argument("--max-rounds", type=int, default=4)
    p.add_argument("--bots-per-side", type=int, default=2)
    p.add_argument("--round-timeout", type=float, default=200)
    p.add_argument("--http-port", type=int, default=18095)
    p.add_argument("--ws-port", type=int, default=18096)
    p.add_argument("--ctl-port", type=int, default=18097)
    p.add_argument("--play-out", action="store_true", help="play the map to the end instead of cmd end_match")
    p.add_argument("--no-offline", action="store_true", help="skip the 75 s platform outage (D12 auto-pause)")
    p.add_argument("--from-scrim", action="store_true",
                   help="assign while a bots-only scrim runs (dev_bots_scrim): D16 hand-over")
    p.add_argument("--force", action="store_true")
    p.add_argument("--restore-bot-quota", type=int, default=2)
    p.add_argument("--out", default="")
    p.add_argument("--save-examples", default="", help="write the first frame of each type the server sent here")
    return p.parse_args(argv)


if __name__ == "__main__":
    sys.exit(Test(parse_args()).run())
