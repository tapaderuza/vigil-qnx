#!/usr/bin/env python3
"""Vigil patient simulator: synthetic vital-sign streams for a small ward.

Every patient, value and event here is invented. Frames use the Vigil protocol
(``$VGVIT,<bed>,<hr>,<spo2>,<rr>,<sbp>,<temp>,<acvpu>,<o2>*hh``, one per bed per second).

    python3 sim/vigil_sim.py generate ward -o ward.txt          # timestamped file for vg-replay
    python3 sim/vigil_sim.py send ward --host 127.0.0.1 --speed 8   # stream over UDP to vigild

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import argparse
import io
import random
import socket
import sys
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, Iterator, List, Optional, Tuple

Vitals = Dict[str, Optional[float]]  # hr, spo2, rr, sbp, temp; acvpu (str) and o2 (int) handled apart


def checksum(body: str) -> str:
    x = 0
    for ch in body:
        x ^= ord(ch)
    return f"{x:02X}"


def frame(bed: int, hr, spo2, rr, sbp, temp, acvpu: str, o2: int) -> str:
    def f(v, d=0):
        return "" if v is None else f"{v:.{d}f}"
    body = f"VGVIT,{bed},{f(hr)},{f(spo2)},{f(rr)},{f(sbp)},{f(temp, 1)},{acvpu},{o2}"
    return f"${body}*{checksum(body)}"


def lerp(points: List[Tuple[float, float]], t: float) -> float:
    """Piecewise linear value at time t from (time, value) breakpoints."""
    if t <= points[0][0]:
        return points[0][1]
    for (t0, v0), (t1, v1) in zip(points, points[1:]):
        if t <= t1:
            return v0 + (v1 - v0) * (t - t0) / (t1 - t0)
    return points[-1][1]


@dataclass
class Patient:
    bed: int
    name: str
    base: Dict[str, float]
    traj: Dict[str, List[Tuple[float, float]]] = field(default_factory=dict)  # per-parameter breakpoints
    noise: Dict[str, float] = field(default_factory=lambda: {"hr": 1.5, "spo2": 0.4, "rr": 0.8, "sbp": 2.0, "temp": 0.05})
    missing: Dict[str, List[Tuple[float, float]]] = field(default_factory=dict)  # (start, end) windows
    silent: List[Tuple[float, float]] = field(default_factory=list)              # bed sends nothing
    o2_from: Optional[float] = None
    acvpu: List[Tuple[float, str]] = field(default_factory=lambda: [(0.0, "A")])

    def sample(self, t: float, rnd: random.Random) -> Optional[str]:
        if any(a <= t < b for a, b in self.silent):
            return None
        vals: Vitals = {}
        for k in ("hr", "spo2", "rr", "sbp", "temp"):
            v = lerp(self.traj[k], t) if k in self.traj else self.base[k]
            v += rnd.gauss(0.0, self.noise[k])
            if any(a <= t < b for a, b in self.missing.get(k, [])):
                v = None
            vals[k] = v
        if vals["spo2"] is not None:
            vals["spo2"] = min(100.0, vals["spo2"])
        acvpu = [c for s, c in self.acvpu if s <= t][-1]
        o2 = 1 if self.o2_from is not None and t >= self.o2_from else 0
        return frame(self.bed, vals["hr"], vals["spo2"], vals["rr"], vals["sbp"], vals["temp"], acvpu, o2)


NORMAL = {"hr": 72, "spo2": 97.5, "rr": 15, "sbp": 124, "temp": 36.8}


def stable(bed: int, name: str, **delta) -> Patient:
    b = dict(NORMAL)
    b.update(delta)
    return Patient(bed, name, b)


@dataclass
class Scenario:
    name: str
    description: str
    patients: List[Patient]
    duration_s: float = 900.0


def sc_stable() -> Scenario:
    return Scenario("stable", "Three stable patients: no alarm may ever be raised",
                    [stable(1, "Bed 1"), stable(2, "Bed 2", hr=64, sbp=132), stable(3, "Bed 3", hr=80, temp=37.1)], 900.0)


def deteriorating(bed: int = 1) -> Patient:
    """A gradual, sepsis-like decline: no single value is dramatic at first, the pattern is."""
    p = stable(bed, "Deteriorating")
    p.traj = {
        "hr": [(0, 78), (240, 80), (600, 108), (900, 124), (1100, 132)],
        "rr": [(0, 16), (240, 17), (600, 22), (900, 26), (1100, 29)],
        "spo2": [(0, 97), (300, 96), (600, 94), (900, 91), (1100, 89)],
        "sbp": [(0, 124), (300, 120), (600, 108), (900, 96), (1100, 88)],
        "temp": [(0, 36.9), (300, 37.4), (600, 38.2), (900, 38.9), (1100, 39.2)],
    }
    p.o2_from = 780.0
    p.acvpu = [(0.0, "A"), (1000.0, "V")]
    return p


def sc_deterioration() -> Scenario:
    return Scenario("deterioration", "One patient declines gradually; NEWS2 warns before any single limit is crossed",
                    [deteriorating(1), stable(2, "Bed 2")], 1200.0)


def sc_desat() -> Scenario:
    p = stable(1, "Desaturation")
    p.traj = {"spo2": [(0, 97), (299, 97), (300, 82), (360, 82), (361, 96), (900, 97)]}
    return Scenario("desat", "Acute desaturation from t=300 s to 360 s, then recovery", [p], 600.0)


def sc_probe_off() -> Scenario:
    p = stable(1, "Probe off")
    p.traj = {"spo2": [(0, 80), (600, 80)]}  # a genuinely low value, so a stale number would keep alarming
    p.missing = {"spo2": [(300, 420)]}
    return Scenario("probe_off", "The SpO2 probe falls off for 120 s: a technical alarm, never a stale value", [p], 600.0)


def sc_offline() -> Scenario:
    p = stable(1, "Silent bed")
    p.silent = [(300, 10000)]
    return Scenario("offline", "A bed stops transmitting at t=300 s", [p, stable(2, "Bed 2")], 600.0)


def sc_ward() -> Scenario:
    d = deteriorating(1)
    desat = stable(2, "Desaturation")
    desat.traj = {"spo2": [(0, 97), (499, 97), (500, 83), (560, 83), (561, 96), (1200, 97)]}
    probe = stable(3, "Probe off")
    probe.missing = {"spo2": [(700, 820)]}
    silent = stable(4, "Silent bed")
    silent.silent = [(900, 10000)]
    return Scenario("ward", "A busy ward: one decline, one desaturation, one probe fault, one silent bed, two stable",
                    [d, desat, probe, silent, stable(5, "Bed 5", hr=60), stable(6, "Bed 6", hr=84)], 1200.0)


def sc_delivery() -> Scenario:
    """A short, noise-free script for the delivery-latency trial (docs/DELIVERY.md).

    Every kind of transition happens at a known second, at speed 1 it lasts 75 s, and the same frames give the
    same transitions on the host (vg-replay) and on QNX, so the trial can compare "expected" with "received".
    Bed 3 crosses two limits in the same second on purpose: those transitions are decided in one engine
    cycle and share one notification.
    """
    quiet = {"hr": 0.0, "spo2": 0.0, "rr": 0.0, "sbp": 0.0, "temp": 0.0}
    desat = stable(1, "Desaturation")
    desat.noise = dict(quiet)
    desat.traj = {"spo2": [(0, 97), (7, 97), (7.01, 84), (30, 84), (30.01, 97), (75, 97)]}
    probe = stable(2, "Probe off")
    probe.noise = dict(quiet)
    probe.missing = {"spo2": [(12, 24)]}
    pair = stable(3, "Two limits at once")
    pair.noise = dict(quiet)
    pair.traj = {"hr": [(0, 72), (14, 72), (14.01, 150), (34, 150), (34.01, 72), (75, 72)],
                 "rr": [(0, 15), (14, 15), (14.01, 32), (34, 32), (34.01, 15), (75, 15)]}
    silent = stable(4, "Silent bed")
    silent.noise = dict(quiet)
    silent.silent = [(40, 10000)]
    return Scenario("delivery", "Delivery trial: desaturation, probe off, two limits at once, a bed that goes silent",
                    [desat, probe, pair, silent], 75.0)


def sc_burst8() -> Scenario:
    """Integrity case: eight beds cross five limits together, so NEWS2 follows on each: 6 changes x 8 beds = 48
    transitions that the engine decides in the same cycle (a bed's frames arrive within milliseconds of the others').
    Everything returns to normal at t=18 s, which is another batch. No noise, so the expected list is exact."""
    quiet = {"hr": 0.0, "spo2": 0.0, "rr": 0.0, "sbp": 0.0, "temp": 0.0}
    beds = []
    for i in range(1, 9):
        p = stable(i, f"Bed {i}")
        p.noise = dict(quiet)
        p.traj = {"hr": [(0, 72), (7, 72), (7.01, 150), (18, 150), (18.01, 72), (70, 72)],
                  "spo2": [(0, 97), (7, 97), (7.01, 84), (18, 84), (18.01, 97), (70, 97)],
                  "rr": [(0, 15), (7, 15), (7.01, 32), (18, 32), (18.01, 15), (70, 15)],
                  "sbp": [(0, 124), (7, 124), (7.01, 80), (18, 80), (18.01, 124), (70, 124)],
                  "temp": [(0, 36.8), (7, 36.8), (7.01, 39.5), (18, 39.5), (18.01, 36.8), (70, 36.8)]}
        beds.append(p)
    return Scenario("burst8", "Integrity: eight beds, six changes each, decided in one engine cycle", beds, 70.0)


SCENARIOS: Dict[str, Callable[[], Scenario]] = {
    f.__name__[3:]: f for f in (sc_stable, sc_deterioration, sc_desat, sc_probe_off, sc_offline, sc_ward, sc_delivery, sc_burst8)
}


def events(sc: Scenario, seed: int = 4242) -> Iterator[Tuple[int, str]]:
    rnd = random.Random(seed)
    t = 0
    while t <= sc.duration_s:
        for p in sc.patients:
            s = p.sample(float(t), rnd)
            if s is not None:
                yield int(t * 1000), s
        t += 1


def generate(sc: Scenario, out) -> int:
    out.write(f"# vigil scenario {sc.name}: {sc.description}\n")
    n = 0
    for t_ms, s in events(sc):
        out.write(f"{t_ms} {s}\n")
        n += 1
    return n


def scenario_text(name: str, duration: Optional[float] = None) -> str:
    sc = SCENARIOS[name]()
    if duration:
        sc.duration_s = duration
    buf = io.StringIO()
    generate(sc, buf)
    return buf.getvalue()


def send(sc: Scenario, host: str, port: int, speed: float, loop: bool, burst: int, garbage: bool) -> None:
    """Stream over UDP in (scaled) real time. `burst` repeats each frame; `garbage` adds corrupt frames."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rnd = random.Random(1)
    while True:
        start = time.monotonic()
        sent = 0
        for t_ms, s in events(sc):
            delay = start + (t_ms / 1000.0) / speed - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            data = (s + "\r\n").encode("ascii")
            for _ in range(burst):
                sock.sendto(data, (host, port))
                sent += 1
                if garbage and rnd.random() < 0.3:
                    sock.sendto(data[:-4] + b"00\r\n", (host, port))  # wrong checksum
                    sent += 1
        print(f"vigil-sim: {sc.name} done, {sent} datagrams", file=sys.stderr)
        if not loop:
            return


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    g = sub.add_parser("generate")
    g.add_argument("scenario", choices=sorted(SCENARIOS))
    g.add_argument("--duration", type=float)
    g.add_argument("-o", "--output", default="-")
    s = sub.add_parser("send")
    s.add_argument("scenario", choices=sorted(SCENARIOS))
    s.add_argument("--host", default="127.0.0.1")
    s.add_argument("--port", type=int, default=10120)
    s.add_argument("--speed", type=float, default=1.0)
    s.add_argument("--duration", type=float)
    s.add_argument("--loop", action="store_true")
    s.add_argument("--burst", type=int, default=1)
    s.add_argument("--garbage", action="store_true", help="also send frames with a wrong checksum")
    a = ap.parse_args(argv)
    if a.cmd == "list":
        for k in sorted(SCENARIOS):
            sc = SCENARIOS[k]()
            print(f"{k:14s} {len(sc.patients):2d} beds  {sc.description}")
        return 0
    sc = SCENARIOS[a.scenario]()
    if a.duration:
        sc.duration_s = a.duration
    if a.cmd == "generate":
        if a.output == "-":
            generate(sc, sys.stdout)
        else:
            with open(a.output, "w", encoding="ascii", newline="\n") as f:
                generate(sc, f)
        return 0
    if a.speed <= 0 or a.burst < 1:
        ap.error("--speed must be > 0 and --burst >= 1")
    send(sc, a.host, a.port, a.speed, a.loop, a.burst, a.garbage)
    return 0


if __name__ == "__main__":
    sys.exit(main())
