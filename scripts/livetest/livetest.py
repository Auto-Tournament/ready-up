#!/usr/bin/env python3
"""Ready Up live regression test: a bot-only match or scrim on a real CS2 server.

Drives the readyup-test server through its tmux console and checks Ready Up's
state transitions from the console log. See scripts/livetest/README.md.

Match flow (default, all driven from the server console, no human needed):
  preflight   tmux session up, nobody (human) connected
  selftest    `ru selftest` must print `selftest: PASS`
  reset       `ru idle`, kick bots
  load        `ru match load http://127.0.0.1:<port>/match.json` (served by this
              script): empty roster, map_sides=[knife], maxRounds=4, no OT
  bots        bot_quota 4 -> 2 CT + 2 T bots in the `state:` line
  knife       match_knife (starting -> running), knife winner, pick
  pick        bots-only winner -> sides stay after 3s (or `ru side ...`)
  live        mode=match_live
  rounds      round ends 1..2, halftime side swap, rounds 3.., `Game Over`
  postgame    mode=postgame (match still loaded) after the final round
  idle        Ready Up clears the match: mode=idle match=none

Scrim flow (--scrim): selftest, reset, `ru_dev_bots_scrim 1` (core dev flag), bots
only -> scrim_warmup -> countdown -> scrim created -> knife -> pick -> match_live.
The flag goes back to `cfg` in cleanup.

Exit codes: 0 PASS, 1 FAIL, 2 server busy / unavailable, or restarted by someone
else mid-test (no verdict).
"""

from __future__ import annotations

import argparse
import http.server
import json
import os
import queue
import re
import shlex
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
STATE_RE = re.compile(r"\[ReadyUp\] state: (.*?) reason=(\S+)")
KV_RE = re.compile(r"(\w+)=(\S+)")
SELFTEST_RE = re.compile(r"\[ReadyUp\] selftest: (PASS|FAIL)\b(.*)")
LOADED_RE = re.compile(r"match-load\[\d+\]: match context set: matchid=(\d+) slug=(\S+)")
LOAD_ERR_RE = re.compile(r"match-load\[\d+\]: error: (.*)")
KNIFE_START_RE = re.compile(r"\[ReadyUp\] knife: starting knife round")
KNIFE_RUN_RE = re.compile(r"\[ReadyUp\] knife: round started")
KNIFE_WIN_RE = re.compile(r"\[ReadyUp\] knife: winner=(\S+) \((team\d)\)(.*)")
KNIFE_PICK_RE = re.compile(r"\[ReadyUp\] knife: side picked by (.+?): (\S+) \((.*?)\) -> (\S+);")
KNIFE_SHORT_RE = re.compile(r"\[ReadyUp\] knife: dev flag on and no humans on CT/T - knife round time (\S+) min")
DEV_SCRIM_RE = re.compile(r"\[ReadyUp\] dev_bots_scrim: ([01]) \(")
ROUND_END_RE = re.compile(r'Team "(CT|TERRORIST)" triggered "(SFUI_Notice_\w+)" \(CT "(\d+)"\) \(T "(\d+)"\)')
GAME_OVER_RE = re.compile(r"Game Over: (.*)")
BOT_SIDE_RE = re.compile(r'"([^"<]+)<\d+><BOT><(CT|TERRORIST)>"')
BOT_SWITCH_RE = re.compile(r'"([^"<]+)<\d+><BOT>" switched from team <(CT|TERRORIST)> to <(CT|TERRORIST)>')
BOT_QUOTA_RE = re.compile(r'"?bot_quota"?\s*=\s*"?(\d+)')
CRASH_RE = re.compile(r"\[ReadyUp\] signal=SIG|Segmentation fault|\bAborted\b \(core dumped\)")
PLAYER_RE = r'"(?P<name>.*?)<(?P<uid>\d+)><(?P<sid>[^>]*)><[^>]*>"'
ENTERED_RE = re.compile(PLAYER_RE + r" entered the game")
LEFT_RE = re.compile(PLAYER_RE + r" disconnected")
BOOT_MARKERS = ("Loaded real libserver.so", "player server started")

MATCH_SLUG = "livetest"


def log(msg: str) -> None:
    print(f"[livetest {time.strftime('%H:%M:%S')}] {msg}", flush=True)


# --------------------------------------------------------------------------- server access


class Server:
    """Console access to the test server: tmux send-keys + the console log."""

    def __init__(self, ssh: str, target: str, session: str):
        self.ssh = ssh
        self.target = target.rstrip("/")
        self.session = session
        self.log_path = f"{self.target}/console.log"

    def _argv(self, remote_cmd: str) -> list[str]:
        if not self.ssh:
            return ["bash", "-c", remote_cmd]
        return ["ssh", "-o", "BatchMode=yes", self.ssh, remote_cmd]

    def run(self, remote_cmd: str, check: bool = False, timeout: int = 60) -> subprocess.CompletedProcess:
        return subprocess.run(self._argv(remote_cmd), capture_output=True, text=True, errors="replace",
                              check=check, timeout=timeout)

    def session_alive(self) -> bool:
        return self.run(f"tmux has-session -t {shlex.quote(self.session)}").returncode == 0

    def send(self, cmd: str) -> None:
        log(f"console> {cmd}")
        s = shlex.quote(self.session)
        self.run(f"tmux send-keys -t {s} -l {shlex.quote(cmd)} && tmux send-keys -t {s} Enter", check=True)

    def log_size(self) -> int:
        r = self.run(f"stat -c %s {shlex.quote(self.log_path)}", check=True)
        return int(r.stdout.strip())

    def follow(self, start_offset: int) -> "LogFollower":
        return LogFollower(self._argv(f"exec tail -c +{start_offset + 1} -F {shlex.quote(self.log_path)}"))

    def connected_humans(self) -> list[str]:
        """Humans that entered the game since the last server boot and have not disconnected."""
        pats = "|".join(re.escape(m) for m in BOOT_MARKERS) + "|entered the game|disconnected"
        r = self.run(f"grep -a -E {shlex.quote(pats)} {shlex.quote(self.log_path)} | tail -n 5000")
        present: dict[tuple[str, str], str] = {}
        for raw in r.stdout.splitlines():
            line = ANSI_RE.sub("", raw)
            if any(m in line for m in BOOT_MARKERS):
                present.clear()
                continue
            m = ENTERED_RE.search(line)
            if m and m.group("sid") != "BOT":
                present[(m.group("uid"), m.group("sid"))] = m.group("name")
                continue
            m = LEFT_RE.search(line)
            if m:
                present.pop((m.group("uid"), m.group("sid")), None)
        return sorted(present.values())

    def boot(self, timeout: int) -> bool:
        """Start the tmux session (same recipe as scripts/dev-deploy.sh --restart)."""
        t, s, lg = shlex.quote(self.target), shlex.quote(self.session), shlex.quote(self.log_path)
        script = (f"touch {lg}; start=$(( $(stat -c %s {lg}) + 1 )); "
                  f"tmux new-session -d -s {s} -x 250 -y 50 {t}/run.sh && "
                  f"tmux pipe-pane -t {s} -o 'cat >> {self.log_path}'; "
                  f"for i in $(seq {timeout}); do "
                  f"  tail -c +$start {lg} | grep -a -q 'player server started' && exit 0; "
                  f"  tmux has-session -t {s} 2>/dev/null || exit 1; sleep 1; done; exit 1")
        return self.run(script, timeout=timeout + 30).returncode == 0


class LogFollower:
    def __init__(self, argv: list[str]):
        self.proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                                     errors="replace", bufsize=1)
        self.q: "queue.Queue[str]" = queue.Queue()
        self.t = threading.Thread(target=self._pump, daemon=True)
        self.t.start()

    def _pump(self) -> None:
        assert self.proc.stdout
        for raw in self.proc.stdout:
            self.q.put(ANSI_RE.sub("", raw).replace("\r", "").rstrip("\n"))

    def lines(self, wait: float) -> list[str]:
        out: list[str] = []
        try:
            out.append(self.q.get(timeout=wait))
            while True:
                out.append(self.q.get_nowait())
        except queue.Empty:
            pass
        return out

    def close(self) -> None:
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


# --------------------------------------------------------------------------- match config server


def serve_match_json(bind: str, doc: dict) -> tuple[http.server.HTTPServer, int]:
    body = json.dumps(doc).encode()

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):  # noqa: N802
            if self.path.split("?")[0] != "/match.json":
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, fmt, *args):
            log("http: " + fmt % args)

    srv = http.server.HTTPServer((bind, 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def match_doc(map_name: str, max_rounds: int) -> dict:
    matchid = int(time.time() * 1000)
    return {
        "id": matchid,
        "slug": MATCH_SLUG,
        "config": {
            "matchid": matchid,
            "num_maps": 1,
            "maplist": [map_name],
            "map_sides": ["knife"],
            "maxRounds": max_rounds,
            "overtimeMode": "disabled",
            "knifeDecisionSeconds": 5,
            "team1": {"name": "LiveTestA", "players": {}},
            "team2": {"name": "LiveTestB", "players": {}},
            # Applied after live.cfg on the knife -> live path (modes.cpp ApplyMatchCvarsLocked).
            "cvars": {
                "mp_maxrounds": max_rounds,
                "mp_overtime_enable": 0,
                "mp_halftime": 1,
                "mp_halftime_duration": 3,
                "mp_freezetime": 1,
                "mp_roundtime": 1,
                "mp_roundtime_defuse": 1,
                "mp_roundtime_hostage": 1,
                "mp_round_restart_delay": 2,
                "mp_c4timer": 20,
                "mp_win_panel_display_time": 1,
                "mp_team_timeout_max": 0,
            },
        },
    }


# --------------------------------------------------------------------------- facts + steps


@dataclass
class Facts:
    seq: int = 0
    states: list = field(default_factory=list)  # (seq, fields, reason)
    modes: list = field(default_factory=list)  # deduped mode sequence
    selftest: Optional[tuple] = None
    selftest_after: int = 0
    loaded: Optional[tuple] = None
    load_error: Optional[str] = None
    knife_start: int = 0
    knife_run: int = 0
    knife_win: Optional[tuple] = None
    knife_pick: Optional[tuple] = None
    live_seq: int = 0
    round_ends: list = field(default_factory=list)  # (seq, total, ct, t, notice)
    bot_sides: dict = field(default_factory=dict)  # name -> [(seq, side)] (side changes only)
    bot_switches: list = field(default_factory=list)  # (seq, name, from, to) CT<->T only
    game_over: Optional[tuple] = None
    crash: Optional[str] = None
    restarted: Optional[str] = None
    bot_quota: Optional[int] = None
    notes: list = field(default_factory=list)
    armed: bool = False  # scrim mode: a scrim may go live (there is no match-load line)
    dev_scrim: Optional[tuple] = None  # (seq, "0"|"1") reply to ru_dev_bots_scrim
    knife_short: Optional[str] = None  # dev-flag knife round time (min)

    def last_state(self) -> Optional[dict]:
        return self.states[-1][1] if self.states else None

    def feed(self, line: str) -> None:
        self.seq += 1
        s = self.seq
        if (m := STATE_RE.search(line)):
            fields = dict(KV_RE.findall(m.group(1)))
            self.states.append((s, fields, m.group(2)))
            mode = fields.get("mode", "?")
            label = mode + (f"/knife={fields['knife']}" if "knife" in fields else "") + \
                (" golive=pending" if fields.get("golive") == "pending" else "") + \
                (" countdown" if "countdown" in fields else "")
            if not self.modes or self.modes[-1] != label:
                self.modes.append(label)
            if mode == "match_live" and not self.live_seq and (self.loaded or self.armed):
                self.live_seq = s
            return
        if (m := DEV_SCRIM_RE.search(line)):
            self.dev_scrim = (s, m.group(1))
            return
        if (m := KNIFE_SHORT_RE.search(line)):
            self.knife_short = m.group(1)
            return
        if (m := SELFTEST_RE.search(line)) and s > self.selftest_after and not self.selftest:
            self.selftest = (m.group(1), m.group(2).strip(), line.strip())
            return
        if (m := LOADED_RE.search(line)):
            self.loaded = (s, m.group(1), m.group(2))
            return
        if (m := LOAD_ERR_RE.search(line)):
            self.load_error = m.group(1)
            return
        if KNIFE_START_RE.search(line):
            self.knife_start = self.knife_start or s
            return
        if KNIFE_RUN_RE.search(line):
            self.knife_run = self.knife_run or s
            return
        if (m := KNIFE_WIN_RE.search(line)):
            self.knife_win = (s, m.group(1), m.group(2), m.group(3).strip())
            return
        if (m := KNIFE_PICK_RE.search(line)):
            self.knife_pick = (s, m.group(1), m.group(2), m.group(4))
            return
        if any(mk in line for mk in BOOT_MARKERS):
            self.restarted = self.restarted or line.strip()
        if CRASH_RE.search(line):
            self.crash = line.strip()
        if (m := BOT_QUOTA_RE.search(line)) and "bot_quota" in line and self.bot_quota is None:
            self.bot_quota = int(m.group(1))
        if (m := BOT_SWITCH_RE.search(line)):
            self.bot_switches.append((s, m.group(1), m.group(2), m.group(3)))
        for name, side in BOT_SIDE_RE.findall(line):
            hist = self.bot_sides.setdefault(name, [])
            if not hist or hist[-1][1] != side:
                hist.append((s, side))
        if self.live_seq and (m := ROUND_END_RE.search(line)):
            ct, t = int(m.group(3)), int(m.group(4))
            total = ct + t
            if not self.round_ends or self.round_ends[-1][1] != total:
                self.round_ends.append((s, total, ct, t, m.group(2)))
            return
        if self.live_seq and (m := GAME_OVER_RE.search(line)):
            self.game_over = (s, m.group(1).strip())

    def states_since(self, seq: int):
        return [(s, f, r) for (s, f, r) in self.states if s > seq]

    def halftime_swap(self, half: int) -> Optional[str]:
        """Evidence that sides swapped after round `half` (bot side flip or score swap)."""
        ends = {total: (s, ct, t) for (s, total, ct, t, _) in self.round_ends}
        if half not in ends:
            return None
        half_seq = ends[half][0]
        flipped = [f"{name} {a}->{b}" for (s, name, a, b) in self.bot_switches if s > half_seq]
        if not flipped:
            # Fallback: a bot's side in log headers before vs after the halftime round end.
            for name, hist in self.bot_sides.items():
                before = [side for (s, side) in hist if s <= half_seq]
                after = [side for (s, side) in hist if s > half_seq]
                if before and after and after[-1] != before[-1]:
                    flipped.append(f"{name} {before[-1]}->{after[-1]}")
        if flipped:
            return "bot side flip: " + ", ".join(sorted(flipped))
        if half + 1 in ends:
            _, ct1, t1 = ends[half]
            _, ct2, t2 = ends[half + 1]
            if ct1 != t1 and ct2 >= t1 and t2 >= ct1 and not (ct2 >= ct1 and t2 >= t1):
                return f"score swap: CT {ct1}:T {t1} -> CT {ct2}:T {t2}"
        return None


@dataclass
class Step:
    name: str
    timeout: float
    check: Callable[[Facts], "bool | str"]  # True = pass, str = hard failure reason
    action: Optional[Callable[[], None]] = None
    detail: Callable[[Facts], str] = lambda f: ""
    status: str = "PEND"
    elapsed: float = 0.0
    info: str = ""


class Runner:
    def __init__(self, args):
        self.a = args
        self.srv = Server(args.ssh, args.target, args.session)
        self.f = Facts()
        self.follower: Optional[LogFollower] = None
        self.http: Optional[http.server.HTTPServer] = None
        self.results: list[Step] = []
        self.captured: list[str] = []
        self.match_loaded = False
        self.touched = False
        self.timescale_set = False
        self.flag_set = False

    # ---- plumbing
    def pump(self, wait: float = 0.5) -> None:
        assert self.follower
        for line in self.follower.lines(wait):
            self.captured.append(line)
            self.f.feed(line)

    def run_step(self, st: Step) -> bool:
        log(f"step {st.name} (timeout {st.timeout:.0f}s)")
        t0 = time.time()
        if st.action:
            st.action()
        last_alive_check = t0
        while True:
            self.pump(0.5)
            res = st.check(self.f)
            if self.f.crash:
                res = f"crash in log: {self.f.crash}"
            elif self.f.restarted:
                res = "server restarted during the test (another deploy?) - result not valid"
            now = time.time()
            if res is not True and now - last_alive_check > 10:
                last_alive_check = now
                if not self.srv.session_alive():
                    res = f"tmux session {self.a.session} died"
            if res is True:
                st.status = "PASS"
            elif isinstance(res, str) and res.startswith("SKIP"):
                st.status, st.info = "SKIP", res[5:].strip()
            elif isinstance(res, str) and res:
                st.status, st.info = "FAIL", res
            elif now - t0 > st.timeout:
                st.status, st.info = "FAIL", f"timeout after {st.timeout:.0f}s"
            if st.status != "PEND":
                st.elapsed = now - t0
                d = st.detail(self.f)
                if d:
                    st.info = (st.info + "; " if st.info else "") + d
                log(f"step {st.name}: {st.status} {st.info}")
                self.results.append(st)
                return st.status in ("PASS", "SKIP")

    # ---- scenario
    def steps(self) -> list[Step]:
        f = self.f
        a = self.a
        half = a.max_rounds // 2
        mark: dict[str, int] = {}

        def since(key: str) -> int:
            return mark.get(key, 0)

        def state_where(pred, after_key: str):
            for s, fl, _ in f.states_since(since(after_key)):
                if pred(fl):
                    return fl
            return None

        def act_selftest():
            f.selftest_after = f.seq
            self.srv.send("ru selftest")

        def chk_selftest(_f):
            if not f.selftest:
                return False
            return True if f.selftest[0] == "PASS" else f"selftest: {f.selftest[0]} {f.selftest[1]}"

        def act_reset():
            mark["reset"] = f.seq
            self.touched = True
            self.srv.send("bot_quota")  # echo current value so it can be restored
            self.srv.send("ru idle")
            self.srv.send("bot_kick")
            self.srv.send("ru state")

        def chk_reset(_f):
            return state_where(lambda fl: fl.get("mode") == "idle" and fl.get("match") == "none", "reset") is not None

        def act_load():
            mark["load"] = f.seq
            cur = f.last_state() or {}
            map_name = a.map or cur.get("map") or "de_dust2"
            if map_name == "?":
                map_name = "de_dust2"
            doc = match_doc(map_name, a.max_rounds)
            self.http, port = serve_match_json(a.http_bind, doc)
            url = f"http://{a.http_host}:{port}/match.json"
            log(f"serving match {doc['config']['matchid']} on {url} (map {map_name})")
            self.srv.send(f"ru match load {url}")

        def chk_load(_f):
            if f.load_error:
                return f"match load error: {f.load_error}"
            if not f.loaded or f.loaded[0] <= since("load"):
                return False
            if f.loaded[2] != MATCH_SLUG:
                return f"loaded unexpected slug {f.loaded[2]}"
            self.match_loaded = True
            return True

        def act_bots():
            # The match load kicks bots and sets bot_quota 0; bring 2 per side back.
            time.sleep(0.3)
            self.srv.send(f"bot_quota {a.bots_per_side * 2}")

        def chk_warmup(_f):
            return state_where(lambda fl: fl.get("mode") == "match_warmup" and fl.get("warmup") == "1",
                               "load") is not None

        def bots_ok(fl):
            # bot_quota counts differ with bot_quota_mode; require >= N bots and no humans per side.
            def side_ok(v):
                h, _, b = (v or "").partition("+")
                return h == "0" and b.isdigit() and int(b) >= a.bots_per_side
            return side_ok(fl.get("ct")) and side_ok(fl.get("t"))

        def chk_bots(_f):
            return any(bots_ok(fl) for _, fl, _ in f.states_since(since("load")))

        def chk_countdown(_f):
            return ("SKIP: scrim-only; the match flow goes straight to the knife round. "
                    "Covered by `run.sh --scrim`")

        def chk_knife_start(_f):
            if f.live_seq:
                return "went live without a knife round"
            return state_where(lambda fl: fl.get("mode") == "match_knife", "load") is not None and f.knife_start > 0

        def chk_knife_run(_f):
            return f.knife_run > 0 and state_where(lambda fl: fl.get("knife") == "running", "load") is not None

        def chk_knife_end(_f):
            return f.knife_win is not None and state_where(lambda fl: fl.get("knife") == "pick", "load") is not None

        def act_pick():
            if a.side != "auto":
                self.srv.send(f"ru side {a.side}")

        def chk_pick(_f):
            if not f.knife_pick:
                return False
            if a.side == "auto" and f.knife_pick[1] != "timeout":
                return f"side picked by {f.knife_pick[1]}, expected the bots-only timeout"
            want = {"auto": "stay", "stay": "stay", "switch": "mp_swapteams"}.get(a.side)
            if want and f.knife_pick[3] != want:
                return f"pick result {f.knife_pick[3]}, expected {want}"
            return state_where(lambda fl: fl.get("mode") == "match_warmup" and fl.get("golive") == "pending",
                               "load") is not None

        def chk_live(_f):
            return f.live_seq > 0

        def act_timescale():
            if a.timescale and a.timescale != 1:
                self.srv.send("sv_cheats 1")
                self.srv.send(f"host_timescale {a.timescale}")
                self.timescale_set = True

        def final_round_seq() -> int:
            win = a.max_rounds // 2 + 1
            for (s, total, ct, t, _) in f.round_ends:
                if ct >= win or t >= win or total >= a.max_rounds:
                    return s
            return 0

        def no_early_exit():
            if f.game_over or final_round_seq():
                return None
            for _, fl, _ in f.states_since(f.live_seq):
                if fl.get("mode") != "match_live":
                    return f"left match_live early: mode={fl.get('mode')}"
            return None

        def chk_round(n):
            def _c(_f):
                if (e := no_early_exit()):
                    return e
                return any(total >= n for (_, total, _, _, _) in f.round_ends)
            return _c

        def chk_half(_f):
            if (e := no_early_exit()):
                return e
            return f.halftime_swap(half) is not None

        def chk_game_over(_f):
            if (e := no_early_exit()):
                return e
            return f.game_over is not None

        def chk_postgame(_f):
            # Postgame is entered at the final round end (same frame as its log line); the
            # state: line follows on the next tick. The match stays loaded until the reset.
            if not final_round_seq():
                return False
            for s, fl, _ in f.states_since(f.live_seq):
                if fl.get("mode") == "postgame":
                    if fl.get("match") in (None, "none"):
                        return "postgame without a match loaded"
                    mark["postgame"] = s
                    return True
                if fl.get("mode") == "idle":
                    return "went to idle without postgame"
            return False

        def chk_idle_after_postgame(_f):
            for _, fl, _ in f.states_since(since("postgame")):
                if fl.get("mode") == "idle" and fl.get("match") == "none":
                    return True
                if fl.get("mode") not in ("postgame", "idle"):
                    return f"left postgame for {fl.get('mode')}, expected idle"
            return False

        def rounds_detail(_f):
            return " ".join(f"r{total}={ct}:{t}" for (_, total, ct, t, _) in f.round_ends)

        def knife_detail(f_):
            if not f_.knife_win:
                return ""
            short = f" [dev knife time {f_.knife_short} min]" if f_.knife_short else ""
            return (f"winner={f_.knife_win[1]} ({f_.knife_win[2]}) {f_.knife_win[3]}"[:140]) + short

        knife_steps = [
            Step("knife: match_knife starting", 60, chk_knife_start),
            Step("knife: round running", 60, chk_knife_run),
            Step("knife: winner decided", 240, chk_knife_end, None, knife_detail),
            Step(f"pick ({a.side}) -> golive pending", 30, chk_pick, act_pick,
                 lambda f_: f"by {f_.knife_pick[1]}: {f_.knife_pick[2]} -> {f_.knife_pick[3]}" if f_.knife_pick else ""),
            Step("match_live", 30, chk_live, None),
        ]

        if a.scrim:
            return self.scrim_steps(f, a, mark, since, state_where, bots_ok, act_selftest, chk_selftest,
                                    act_reset, chk_reset, knife_steps)

        steps = [
            Step("selftest", 45, chk_selftest, act_selftest,
                 lambda f_: f_.selftest[2].split("] ", 1)[-1] if f_.selftest else ""),
            Step("reset (idle, no match)", 30, chk_reset, act_reset),
            Step("match load (ru match load)", 30, chk_load, act_load,
                 lambda f_: f"matchid={f_.loaded[1]} slug={f_.loaded[2]}" if f_.loaded else ""),
            Step("warmup (match_warmup)", 20, chk_warmup, act_bots),
            Step(f"bots (>= {a.bots_per_side} per side, no humans)", 60, chk_bots, None,
                 lambda f_: next((f"ct={fl.get('ct')} t={fl.get('t')}" for _, fl, _ in reversed(f_.states)), "")),
            Step("countdown", 1, chk_countdown),
        ] + knife_steps
        steps.append(Step("round 1 ends", a.round_timeout, chk_round(1), act_timescale, rounds_detail))
        for n in range(2, half + 1):
            steps.append(Step(f"round {n} ends", a.round_timeout, chk_round(n), None, rounds_detail))
        steps.append(Step(f"halftime swap after round {half}", a.round_timeout + 30, chk_half, None,
                          lambda f_: f_.halftime_swap(half) or ""))
        steps.append(Step(f"round {half + 1} ends", a.round_timeout, chk_round(half + 1), None, rounds_detail))
        steps.append(Step("map end (Game Over)", a.round_timeout * max(1, a.max_rounds - half - 1) + 30,
                          chk_game_over, None,
                          lambda f_: (f_.game_over[1] + " | " + rounds_detail(f_)) if f_.game_over else rounds_detail(f_)))
        steps.append(Step("postgame (mode=postgame, match loaded)", 30, chk_postgame))
        steps.append(Step("idle: match cleared (idle, match=none)", 90, chk_idle_after_postgame))
        return steps

    def scrim_steps(self, f, a, mark, since, state_where, bots_ok, act_selftest, chk_selftest,
                    act_reset, chk_reset, knife_steps) -> list[Step]:
        """Bots-only scrim: dev_bots_scrim on, bots on both sides, no humans, no match config."""

        def act_flag():
            self.flag_set = True
            self.srv.send("ru_dev_bots_scrim 1")

        def chk_flag(_f):
            if not f.dev_scrim:
                return False
            return True if f.dev_scrim[1] == "1" else "ru_dev_bots_scrim 1 did not turn the flag on"

        def act_scrim():
            mark["load"] = f.seq
            f.armed = True
            self.srv.send("ru scrim")  # `ru idle` (reset) turned auto scrim warmup off
            time.sleep(0.3)
            self.srv.send(f"bot_quota {a.bots_per_side * 2}")

        def chk_scrim_warmup(_f):
            fl = state_where(lambda fl: fl.get("mode") == "scrim_warmup", "load")
            if fl is None:
                return False
            if fl.get("dev_bots_scrim") != "1":
                return "state: line does not show dev_bots_scrim=1"
            return True

        def chk_bots(_f):
            return any(bots_ok(fl) for _, fl, _ in f.states_since(since("load")))

        def chk_countdown(_f):
            return state_where(lambda fl: fl.get("mode") == "scrim_warmup" and "countdown" in fl, "load") is not None

        def chk_created(_f):
            fl = state_where(lambda fl: (fl.get("match") or "").startswith("scrim:"), "load")
            if fl is None:
                return False
            return True

        return [
            Step("selftest", 45, chk_selftest, act_selftest,
                 lambda f_: f_.selftest[2].split("] ", 1)[-1] if f_.selftest else ""),
            Step("reset (idle, no match)", 30, chk_reset, act_reset),
            Step("dev flag (ru_dev_bots_scrim 1)", 15, chk_flag, act_flag),
            Step("scrim warmup (bots only, dev_bots_scrim=1)", 60, chk_scrim_warmup, act_scrim),
            Step(f"bots (>= {a.bots_per_side} per side, no humans)", 60, chk_bots, None,
                 lambda f_: next((f"ct={fl.get('ct')} t={fl.get('t')}" for _, fl, _ in reversed(f_.states)), "")),
            Step("countdown (all ready -> 5s)", 30, chk_countdown),
            Step("scrim created (match=scrim:...)", 30, chk_created, None,
                 lambda f_: next((f"match={fl.get('match')}" for _, fl, _ in reversed(f_.states)
                                  if (fl.get("match") or "").startswith("scrim:")), "")),
        ] + knife_steps

    # ---- main
    def preflight(self) -> Optional[str]:
        deadline = time.time() + self.a.wait_session
        while not self.srv.session_alive():
            if time.time() > deadline:
                if self.a.boot:
                    log(f"tmux session {self.a.session} missing; booting the server (--boot)")
                    if self.srv.boot(240):
                        break
                    return "server did not boot"
                return f"tmux session {self.a.session} missing (server down or mid-restart)"
            log(f"tmux session {self.a.session} missing; waiting (another deploy restarting?)")
            time.sleep(10)
        humans = self.srv.connected_humans()
        if humans and not self.a.force:
            return "humans connected: " + ", ".join(humans)
        return None

    def cleanup(self) -> None:
        try:
            if self.timescale_set:
                self.srv.send("host_timescale 1")
                self.srv.send("sv_cheats 0")
            if self.touched:
                self.srv.send("ru idle")    # clears the match context + persisted match
                if self.flag_set:
                    # Before `ru scrim`: with the flag on, the restored bots would start a scrim.
                    self.srv.send("ru_dev_bots_scrim cfg")
                self.srv.send("ru scrim")   # re-enable auto scrim warmup (ru idle turns it off)
                quota = self.f.bot_quota if self.f.bot_quota is not None else self.a.restore_bot_quota
                self.srv.send(f"bot_quota {quota}")
                self.pump(2.0)
        except Exception as e:  # noqa: BLE001
            log(f"cleanup error: {e}")
        if self.http:
            self.http.shutdown()
        if self.follower:
            self.follower.close()

    def report(self, verdict: str) -> None:
        rows = [("STEP", "RESULT", "TIME", "DETAIL")]
        for st in self.results:
            rows.append((st.name, st.status, f"{st.elapsed:5.1f}s", st.info))
        w0 = max(len(r[0]) for r in rows)
        print()
        print("=" * 100)
        for r in rows:
            print(f"{r[0]:<{w0}}  {r[1]:<6} {r[2]:>7}  {r[3]}")
        print("=" * 100)
        print("mode sequence: " + " -> ".join(self.f.modes))
        print(f"LIVETEST {verdict}")
        summ = os.environ.get("GITHUB_STEP_SUMMARY")
        if summ:
            with open(summ, "a") as fh:
                fh.write(f"## Ready Up live test: {verdict}\n\n| step | result | time | detail |\n|---|---|---|---|\n")
                for st in self.results:
                    fh.write(f"| {st.name} | {st.status} | {st.elapsed:.1f}s | {st.info.replace('|', '/')} |\n")
                fh.write("\nMode sequence: `" + " -> ".join(self.f.modes) + "`\n")
        if self.a.out:
            os.makedirs(self.a.out, exist_ok=True)
            with open(os.path.join(self.a.out, "console-capture.log"), "w") as fh:
                fh.write("\n".join(self.captured) + "\n")
            with open(os.path.join(self.a.out, "result.json"), "w") as fh:
                json.dump({"verdict": verdict, "modes": self.f.modes,
                           "steps": [{"name": s.name, "status": s.status, "elapsed": round(s.elapsed, 1),
                                      "detail": s.info} for s in self.results]}, fh, indent=2)

    def main(self) -> int:
        why = self.preflight()
        if why:
            log(f"BUSY/UNAVAILABLE: {why}")
            print(f"LIVETEST SKIPPED ({why})")
            return 2
        self.follower = self.srv.follow(self.srv.log_size())
        failed = False
        deadline = time.time() + self.a.total_timeout
        try:
            for st in self.steps():
                if failed:
                    st.status = "SKIP"
                    self.results.append(st)
                    continue
                if time.time() > deadline:
                    st.status, st.info = "FAIL", "total timeout"
                    self.results.append(st)
                    failed = True
                    continue
                if not self.run_step(st):
                    failed = True
        except KeyboardInterrupt:
            failed = True
            log("interrupted")
        finally:
            self.cleanup()
        if failed and self.f.restarted:
            self.report("SKIPPED (server restarted mid-test)")
            return 2
        verdict = "FAIL" if failed else "PASS"
        self.report(verdict)
        return 1 if failed else 0


def parse_args(argv=None):
    env = os.environ.get
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--ssh", default=env("RU_SSH", "cs2servermanager@localhost"),
                   help="ssh destination owning the server ('' = run locally)")
    p.add_argument("--target", default=env("RU_TARGET", "/home/cs2servermanager/readyup-test"))
    p.add_argument("--session", default=env("RU_SESSION", "ru-test"))
    p.add_argument("--http-bind", default=env("LIVETEST_HTTP_BIND", "127.0.0.1"))
    p.add_argument("--http-host", default=env("LIVETEST_HTTP_HOST", "127.0.0.1"),
                   help="host the CS2 server uses to fetch match.json from this script")
    p.add_argument("--map", default=env("LIVETEST_MAP", ""), help="default: the current map")
    p.add_argument("--scrim", action="store_true", default=env("LIVETEST_MODE", "match") == "scrim",
                   help="bots-only scrim (dev_bots_scrim): warmup -> countdown -> knife -> pick -> live")
    p.add_argument("--max-rounds", type=int, default=int(env("LIVETEST_MAX_ROUNDS", "4")))
    p.add_argument("--bots-per-side", type=int, default=int(env("LIVETEST_BOTS_PER_SIDE", "2")))
    p.add_argument("--side", choices=["auto", "stay", "switch"], default=env("LIVETEST_SIDE", "auto"),
                   help="knife pick: auto = bots-only timeout (stay); stay/switch = `ru side ...` from console")
    p.add_argument("--timescale", type=float, default=float(env("LIVETEST_TIMESCALE", "1")),
                   help="host_timescale during match_live only (needs sv_cheats 1; reverted after)")
    p.add_argument("--round-timeout", type=float, default=float(env("LIVETEST_ROUND_TIMEOUT", "200")))
    p.add_argument("--total-timeout", type=float, default=float(env("LIVETEST_TOTAL_TIMEOUT", "1500")))
    p.add_argument("--wait-session", type=float, default=float(env("LIVETEST_WAIT_SESSION", "300")),
                   help="how long to wait for a missing tmux session (deploy mid-restart)")
    p.add_argument("--boot", action="store_true", help="start the server if the session is still missing")
    p.add_argument("--force", action="store_true", help="run even if humans are connected")
    p.add_argument("--restore-bot-quota", type=int, default=2)
    p.add_argument("--out", default=env("LIVETEST_OUT", ""), help="write console-capture.log + result.json here")
    a = p.parse_args(argv)
    if a.max_rounds < 2 or a.max_rounds % 2:
        p.error("--max-rounds must be even and >= 2")
    return a


if __name__ == "__main__":
    sys.exit(Runner(parse_args()).main())
