#!/usr/bin/env python3
"""Mock Auto Tournament platform for the fleet step-3 live test (scripts/livetest/fleet_livetest.py).

Speaks the server channel of docs/FLEET.md well enough for one Ready Up server:

  --http PORT  POST /api/fleet/enroll          any code/key -> 201 {server_id, token, ws_url}
  --ws PORT    /api/fleet/ws (python websockets) hello -> welcome (reset / resumed), ping -> pong,
               an ack for every reliable frame, platform messages with seq, replay after reconnect
  --ctl PORT   control API for the test driver (loopback only):
                 GET  /status               {connected, sessions, server_id, rx_seq, tx_seq, frames}
                 GET  /frames?since=N       frames received from the server (index >= N), each
                                            {i, at, frame, errors} (errors = JSON Schema problems)
                 POST /send {type, payload, epoch?, reliable?=true}   -> {id, seq}
                 POST /close                drop the WebSocket (the server reconnects)
                 POST /offline {seconds}    drop it and refuse reconnects that long (D12 test)

Every frame the server sends is validated against plugins/fleet/protocol/v1 (--schemas): the
envelope, then the payload against messages/<type>.json. Runs in Docker with
`pip install websockets jsonschema` (see fleet_livetest.py).
"""
import argparse
import asyncio
import base64
import http.server
import json
import os
import secrets
import string
import sys
import threading
import time

import jsonschema
from referencing import Registry, Resource
from websockets.asyncio.server import serve

CROCK = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"


def ulid() -> str:
    t = int(time.time() * 1000)
    ts = "".join(CROCK[(t >> (5 * i)) & 31] for i in reversed(range(10)))
    return ts + "".join(secrets.choice(CROCK) for _ in range(16))


class Schemas:
    def __init__(self, root: str):
        self.docs = {}
        for dirpath, _, files in os.walk(root):
            for f in files:
                if f.endswith(".json"):
                    with open(os.path.join(dirpath, f), encoding="utf-8") as fh:
                        d = json.load(fh)
                    self.docs[d["$id"]] = d
        self.registry = Registry().with_resources((i, Resource.from_contents(d)) for i, d in self.docs.items())
        self.base = "https://auto-tournament.dev/fleet/v1/"

    def check(self, frame: dict) -> list:
        errs = []
        def run(doc_id, inst, where):
            if doc_id not in self.docs:
                errs.append(f"{where}: no schema {doc_id}")
                return
            v = jsonschema.Draft202012Validator(self.docs[doc_id], registry=self.registry)
            for e in v.iter_errors(inst):
                errs.append(f"{where}{'/' + '/'.join(map(str, e.absolute_path)) if e.absolute_path else ''}: {e.message[:300]}")
        run(self.base + "envelope.json", frame, "envelope")
        if isinstance(frame.get("payload"), dict) and isinstance(frame.get("type"), str):
            run(self.base + "messages/" + frame["type"] + ".json", frame["payload"], "payload")
        return errs


class Platform:
    def __init__(self, args):
        self.a = args
        self.schemas = Schemas(args.schemas)
        self.lock = threading.Lock()
        self.frames = []            # received from the server
        self.out = []               # platform reliable messages (for replay): dicts with seq
        self.tx_seq = 0
        self.rx_seq = 0             # highest contiguous server seq received
        self.stream_id = None
        self.server_id = "srv_livetest"
        self.token = "rus_" + "".join(secrets.choice(string.ascii_lowercase + string.digits) for _ in range(12)) + "_" + \
            base64.urlsafe_b64encode(secrets.token_bytes(32)).decode().rstrip("=")
        self.ws = None
        self.loop = None
        self.sessions = 0
        self.connected = False
        self.reject_until = 0.0     # POST /offline: refuse connections until then

    # ---- server frames ------------------------------------------------------------------
    def record(self, frame: dict, errors: list) -> None:
        with self.lock:
            self.frames.append({"i": len(self.frames), "at": time.time(), "frame": frame, "errors": errors})
        tag = f" SCHEMA ERRORS: {errors}" if errors else ""
        print(f"<- {frame.get('type')} seq={frame.get('seq')} rev={frame.get('payload', {}).get('rev')}{tag}", flush=True)

    def envelope(self, type_: str, payload: dict, epoch=None, ref=None, reliable=False) -> dict:
        e = {"v": 1, "type": type_, "id": ulid(), "ts": int(time.time() * 1000), "ack": self.rx_seq}
        if reliable:
            self.tx_seq += 1
            e["seq"] = self.tx_seq
        if ref:
            e["ref"] = ref
        if epoch:
            e["epoch"] = epoch
        e["payload"] = payload
        return e

    async def handler(self, ws):
        if not ws.request.path.startswith("/api/fleet/ws"):
            await ws.close(4400, "bad path")
            return
        if time.time() < self.reject_until:
            await ws.close(1013, "platform offline (test)")
            return
        auth = ws.request.headers.get("Authorization", "")
        if auth != "Bearer " + self.token:
            print("ws: bad token", flush=True)
            await ws.close(4401, "bad token")
            return
        try:
            hello = json.loads(await asyncio.wait_for(ws.recv(), 10))
        except Exception as e:  # noqa: BLE001
            print("ws: no hello:", e, flush=True)
            return
        self.record(hello, self.schemas.check(hello))
        st = hello.get("payload", {}).get("stream", {})
        known = st.get("id") == self.stream_id
        if not known:
            self.stream_id = st.get("id")
            self.rx_seq = int(st.get("last_tx_seq", 0))  # reset: the server's next seq is the new base
        server_rx = int(st.get("last_rx_seq", 0))
        welcome = self.envelope("welcome", {
            "session_id": ulid(), "protocol": 1, "heartbeat": {"interval_ms": 10000, "timeout_ms": 30000},
            "resume": {"result": "resumed" if known else "reset", "platform_last_rx_seq": self.rx_seq},
            "server_config_rev": 0, "admins_rev": 0, "assignment": None}, ref=hello.get("id"))
        await ws.send(json.dumps(welcome))
        self.ws = ws
        self.connected = True
        self.sessions += 1
        # Replay platform messages the server has not processed.
        for m in list(self.out):
            if m["seq"] > server_rx:
                m["ack"] = self.rx_seq
                await ws.send(json.dumps(m))
        try:
            async for text in ws:
                f = json.loads(text)
                self.record(f, self.schemas.check(f))
                t = f.get("type")
                if t == "ping":
                    await ws.send(json.dumps(self.envelope("pong", {"t": f.get("payload", {}).get("t", 0)}, ref=f.get("id"))))
                seq = f.get("seq")
                if seq:
                    if seq == self.rx_seq + 1:
                        self.rx_seq = seq
                    await ws.send(json.dumps(self.envelope("ack", {})))
                if isinstance(f.get("ack"), int):
                    with self.lock:
                        self.out = [m for m in self.out if m["seq"] > f["ack"]]
        except Exception as e:  # noqa: BLE001
            print("ws: closed:", e, flush=True)
        finally:
            self.connected = False
            if self.ws is ws:
                self.ws = None

    def send(self, type_: str, payload: dict, epoch=None, reliable=True) -> dict:
        env = self.envelope(type_, payload, epoch=epoch, reliable=reliable)
        if reliable:
            with self.lock:
                self.out.append(env)
        ws = self.ws
        if ws is not None:
            asyncio.run_coroutine_threadsafe(ws.send(json.dumps(env)), self.loop).result(5)
        print(f"-> {type_} seq={env.get('seq')} id={env['id']}", flush=True)
        return {"id": env["id"], "seq": env.get("seq", 0), "sent": ws is not None}

    def close_ws(self) -> None:
        ws = self.ws
        if ws is not None:
            asyncio.run_coroutine_threadsafe(ws.close(1001, "test"), self.loop).result(5)


def http_server(port: int, handler_cls):
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler_cls)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--http", type=int, default=18095)
    ap.add_argument("--ws", type=int, default=18096)
    ap.add_argument("--ctl", type=int, default=18097)
    ap.add_argument("--schemas", default="/proto/v1")
    a = ap.parse_args()
    p = Platform(a)

    class Enroll(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(n) or b"{}")
            errs = []
            v = jsonschema.Draft202012Validator(p.schemas.docs[p.schemas.base + "http/enroll.request.json"],
                                                registry=p.schemas.registry)
            errs = [e.message for e in v.iter_errors(body)]
            print(f"enroll: install_id={body.get('install_id')} errors={errs}", flush=True)
            out = {"success": True, "server_id": p.server_id, "tenant_id": "default", "name": "livetest",
                   "token": p.token, "ws_url": f"ws://127.0.0.1:{a.ws}/api/fleet/ws", "reenrolled": False}
            data = json.dumps(out).encode()
            self.send_response(201 if self.path == "/api/fleet/enroll" and not errs else 400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    class Ctl(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, obj, code=200):
            data = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path.startswith("/status"):
                self.reply({"connected": p.connected, "sessions": p.sessions, "server_id": p.server_id,
                            "rx_seq": p.rx_seq, "tx_seq": p.tx_seq, "frames": len(p.frames)})
            elif self.path.startswith("/frames"):
                since = 0
                if "since=" in self.path:
                    since = int(self.path.split("since=")[1].split("&")[0])
                with p.lock:
                    self.reply(p.frames[since:])
            else:
                self.reply({"error": "not found"}, 404)

        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(n) or b"{}")
            if self.path.startswith("/send"):
                self.reply(p.send(body["type"], body.get("payload", {}), body.get("epoch"), body.get("reliable", True)))
            elif self.path.startswith("/offline"):
                p.reject_until = time.time() + float(body.get("seconds", 60))
                p.close_ws()
                self.reply({"ok": True, "until": p.reject_until})
            elif self.path.startswith("/close"):
                p.close_ws()
                self.reply({"ok": True})
            else:
                self.reply({"error": "not found"}, 404)

    http_server(a.http, Enroll)
    http_server(a.ctl, Ctl)

    async def run():
        p.loop = asyncio.get_running_loop()
        async with serve(p.handler, "127.0.0.1", a.ws, max_size=2 << 20):
            print(f"mock platform: enroll http://127.0.0.1:{a.http}  ws :{a.ws}  ctl :{a.ctl}  "
                  f"schemas {len(p.schemas.docs)}", flush=True)
            await asyncio.Future()

    asyncio.run(run())
    return 0


if __name__ == "__main__":
    sys.exit(main())
