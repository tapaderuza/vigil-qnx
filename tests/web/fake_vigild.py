#!/usr/bin/env python3
"""A stand-in for vigild's HTTP side, used to exercise the dashboard's freshness handling in a browser.

    tests/web/fake_vigild.py [--port 8099]

Serves web/index.html at / and a synthetic /api/picture at 10 Hz of engine time, in the same JSON shape
as vigild. The behaviour is switched at run time:

    curl -X POST localhost:8099/__mode/normal     fresh picture on every request
    curl -X POST localhost:8099/__mode/error      HTTP 503
    curl -X POST localhost:8099/__mode/hang       accept the request and never answer
    curl -X POST localhost:8099/__mode/frozen     answer, but always with the same seq / t_ms
    curl -X POST localhost:8099/__mode/restart    the daemon restarted: seq and t_ms start again from 1

Synthetic patients only. This is a test tool, not part of the product.

SPDX-License-Identifier: MIT
"""
import argparse
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STATE = {"mode": "normal", "t0": time.monotonic(), "frozen": None, "requests": 0}
LOCK = threading.Lock()


def picture(seq: int) -> dict:
    """Two synthetic beds: 01 deteriorating (NEWS2 5, MEDIUM), 02 stable."""
    return {
        "seq": seq, "t_ms": seq * 100, "highest": "MEDIUM", "n_high": 0, "n_medium": 1, "n_low": 0,
        "audio": {"paused": False, "remaining_s": 0},
        "cfg": {"onset_s": 3, "clear_s": 5, "sensor_s": 5, "offline_s": 10},
        "health": {"cycles": seq, "overruns": 0, "period_us": 100000, "last_cycle_us": 120, "max_cycle_us": 300,
                   "last_latency_us": 40, "max_latency_us": 90, "samples": seq, "bad_frames": 0, "subscribers": 1},
        "beds": [
            {"bed": 1, "online": True, "age_ms": 400, "level": "MEDIUM", "audible": True, "hr": 112, "spo2": 94,
             "rr": 22, "sbp": 104, "temp": 38.4, "acvpu": "A", "o2": 0, "news2": {"total": 5, "band": "MEDIUM"},
             "alarms": [{"src": "NEWS2", "level": "MEDIUM"}], "stale": []},
            {"bed": 2, "online": True, "age_ms": 300, "level": "NONE", "audible": False, "hr": 72, "spo2": 98,
             "rr": 14, "sbp": 122, "temp": 36.8, "acvpu": "A", "o2": 0, "news2": {"total": 0, "band": "LOW"},
             "alarms": [], "stale": []},
        ],
    }


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _send(self, code, body: bytes, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.path.startswith("/__mode/"):
            mode = self.path.rsplit("/", 1)[1]
            with LOCK:
                if mode == "restart":
                    STATE["t0"] = time.monotonic()
                    mode = "normal"
                STATE["mode"] = mode
                STATE["frozen"] = None
            self._send(200, json.dumps({"mode": mode}).encode())
        else:
            self._send(404, b"{}")

    def do_GET(self):
        if self.path.split("?")[0] in ("/", "/index.html"):
            self._send(200, (ROOT / "web" / "index.html").read_bytes(), "text/html; charset=utf-8")
            return
        if self.path.startswith("/api/picture"):
            with LOCK:
                STATE["requests"] += 1
                mode = STATE["mode"]
                seq = int((time.monotonic() - STATE["t0"]) * 10) + 1
                if mode == "frozen":
                    if STATE["frozen"] is None:
                        STATE["frozen"] = seq
                    seq = STATE["frozen"]
            if mode == "error":
                self._send(503, b'{"error":"unavailable"}')
            elif mode == "hang":
                time.sleep(3600)
            else:
                self._send(200, json.dumps(picture(seq)).encode())
            return
        if self.path == "/__state":
            with LOCK:
                self._send(200, json.dumps({"mode": STATE["mode"], "requests": STATE["requests"]}).encode())
            return
        self._send(404, b"{}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8099)
    args = ap.parse_args()
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    srv.daemon_threads = True
    print(f"fake vigild on http://127.0.0.1:{args.port}/", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
