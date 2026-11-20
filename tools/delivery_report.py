#!/usr/bin/env python3
"""Turn a delivery trial into a report (docs/DELIVERY.md).

    delivery_report.py --expected expected.txt --run idle=idle.tsv --run loaded=loaded.tsv \
                       [--notes idle=idle.txt --notes loaded=loaded.txt] [--out report]

expected.txt is the output of `vg-replay` on the scenario (the transitions the engine must produce).
Each run is the output of `vigil-watch -m` on the target. Writes report.md and report.json.

What is measured, all from CLOCK_MONOTONIC of the same machine:
  decided -> notified   engine thread: from the transition being decided to the subscribers being notified
  decided -> pulse      from the transition being decided to vigil-watch returning from MsgReceive()
  decided -> event      from the transition being decided to vigil-watch holding the event record
The intentional confirmation delay (onset / release, in the engine's configuration) is reported separately
and never added to those figures. One run describes that run only.

SPDX-License-Identifier: MIT
"""
import argparse
import json
import re
import statistics
import sys
from collections import Counter
from pathlib import Path
from typing import Dict, List, NamedTuple, Optional

TRANSITION = re.compile(r"T\+(?P<t>\d+\.\d)s bed (?P<bed>\d) (?P<src>\w+)\s+(?P<frm>\w+)\s+-> (?P<to>\w+)")


class Key(NamedTuple):
    bed: int
    src: str
    frm: str
    to: str


def first_time(text: str) -> float:
    for line in text.splitlines():
        m = TRANSITION.match(line)
        if m:
            return float(m["t"])
    return 0.0


def expected_from(text: str) -> List[Key]:
    out = []
    for line in text.splitlines():
        m = TRANSITION.match(line)
        if m:
            out.append(Key(int(m["bed"]), m["src"], m["frm"], m["to"]))
    return out


def parse_run(text: str) -> dict:
    events, pulses, lost, header = [], [], 0, ""
    for line in text.splitlines():
        if line.startswith("#"):
            header = line
            continue
        f = line.split("\t")
        if f[0] == "M" and len(f) == 14:
            events.append({
                "seq": int(f[1]), "key": Key(int(f[2]), f[3], f[4], f[5]), "cond_ms": int(f[6]), "decided_ms": int(f[7]),
                "t_decided": int(f[8]), "t_notified": int(f[9]), "t_pulse": int(f[10]), "t_event": int(f[11]),
                "batch": int(f[12]), "drained": int(f[13]),
            })
        elif f[0] == "P" and len(f) == 4:
            pulses.append({"t_pulse": int(f[1]), "t_event": int(f[2]), "n": int(f[3])})
        elif f[0] == "L" and len(f) == 2:
            lost += int(f[1])
    return {"events": events, "pulses": pulses, "lost": lost, "header": header}


HISTORY = re.compile(r"#(?P<seq>\d+) t=(?P<t>\d+\.\d)s bed (?P<bed>\d) (?P<src>\w+)\s+(?P<frm>\w+) -> (?P<to>\w+)")


def parse_history(text: str) -> List[dict]:
    """/dev/vigil/events of the daemon: the engine's own record, read after the run."""
    return [{"seq": int(m["seq"]), "t": float(m["t"]), "key": Key(int(m["bed"]), m["src"], m["frm"], m["to"])}
            for m in map(HISTORY.match, text.splitlines()) if m]


def compare_history(expected: List[Key], hist: List[dict], first_expected_s: float, duration_s: float) -> dict:
    if duration_s and hist:
        end = hist[0]["t"] + (duration_s - first_expected_s)
        after = [h for h in hist if h["t"] > end]
        hist = [h for h in hist if h["t"] <= end]
    else:
        after = []
    got, exp = Counter(h["key"] for h in hist), Counter(expected)
    return {"in_history": len(hist), "missing": [k._asdict() for k in (exp - got).elements()],
            "extra": [k._asdict() for k in (got - exp).elements()], "after_sender_stopped": len(after),
            "same_order": [h["key"] for h in hist] == expected}


def stats(values: List[float]) -> Optional[dict]:
    if not values:
        return None
    v = sorted(values)
    p95 = v[min(len(v) - 1, int(round(0.95 * (len(v) - 1))))]
    return {"n": len(v), "min": v[0], "median": statistics.median(v), "p95": p95, "max": v[-1]}


def analyse(expected: List[Key], run: dict, first_expected_s: float = 0.0, duration_s: float = 0.0,
            history: Optional[List[dict]] = None) -> dict:
    """With duration_s, events decided after the sender stopped are set aside (the beds going stale is a
    consequence of the sender ending, not part of the scenario). The scenario clock is anchored on the first
    received event, which the expected list says happens at first_expected_s."""
    after = []
    if duration_s and run["events"]:
        end_ns = run["events"][0]["t_decided"] + int((duration_s - first_expected_s) * 1e9)
        after = [e for e in run["events"] if e["t_decided"] > end_ns]
        run = dict(run, events=[e for e in run["events"] if e["t_decided"] <= end_ns])
    received = [e["key"] for e in run["events"]]
    exp, got = Counter(expected), Counter(received)
    missing = list((exp - got).elements())
    extra = list((got - exp).elements())
    ev = [e for e in run["events"] if e["t_decided"] and e["t_pulse"]]
    ticks: Dict[int, list] = {}
    for e in ev:
        ticks.setdefault(e["t_decided"], []).append(e)
    batches = Counter(len(v) for v in ticks.values())
    empty_pulses = sum(1 for p in run["pulses"] if p["n"] == 0)  # a pulse that found nothing new was coalesced earlier
    drains: Dict[int, set] = {}
    for e in ev:
        drains.setdefault(e["t_pulse"], set()).add(e["t_decided"])
    multi_tick_drains = sum(1 for s in drains.values() if len(s) > 1)

    def us(a: str, b: str) -> List[float]:
        return [(x[b] - x[a]) / 1000.0 for x in ev]

    seqs = Counter(e["seq"] for e in run["events"])
    return {
        "history": compare_history(expected, history, first_expected_s, duration_s) if history is not None else None,
        "duplicates_by_seq": sum(c - 1 for c in seqs.values() if c > 1),
        "largest_cycle_batch": max(batches) if batches else 0,
        "expected": len(expected), "received": len(received),
        "missing": [k._asdict() for k in missing], "extra": [k._asdict() for k in extra],
        "same_order": received == expected,
        "lost_by_ring": run["lost"],
        "after_sender_stopped": [e["key"]._asdict() for e in after],
        "decided_to_notified_us": stats(us("t_decided", "t_notified")),
        "decided_to_pulse_us": stats(us("t_decided", "t_pulse")),
        "decided_to_event_us": stats(us("t_decided", "t_event")),
        "confirmation_delay_ms": stats([e["decided_ms"] - e["cond_ms"] for e in ev if e["cond_ms"]]),
        "engine_cycles_with_transitions": len(ticks),
        "batch_sizes": {str(k): v for k, v in sorted(batches.items())},
        "pulses": len(run["pulses"]), "pulses_without_news": empty_pulses,
        "drains_spanning_several_cycles": multi_tick_drains,
        "header": run["header"],
    }


def fmt_stats(s: Optional[dict], unit: str) -> str:
    if not s:
        return "no data"
    return f"n={s['n']}  min {s['min']:.1f}  median {s['median']:.1f}  p95 {s['p95']:.1f}  max {s['max']:.1f} {unit}"


def render(expected: List[Key], results: Dict[str, dict], notes: Dict[str, str]) -> str:
    out = ["# Delivery trial report", "",
           "Generated by tools/delivery_report.py. Each run describes that run only; it is not a bound for any other run, "
           "machine or load.", "", f"Expected transitions (from vg-replay): {len(expected)}", ""]
    for name, r in results.items():
        out += [f"## Run: {name}", ""]
        if notes.get(name):
            out += ["Conditions:", "", "```", notes[name].strip(), "```", ""]
        out += [f"- received {r['received']} of {r['expected']} expected; in the expected order: "
                f"{'yes' if r['same_order'] else 'NO'}",
                f"- missing: {r['missing'] or 'none'}", f"- unexpected: {r['extra'] or 'none'}",
                f"- duplicates received (same event sequence number twice): {r['duplicates_by_seq']}",
                f"- largest number of events decided in one engine cycle (counted from the events, not the pulses): "
                f"{r['largest_cycle_batch']}",
                f"- daemon history (/dev/vigil/events) against expected: "
                + ("not captured" if not r["history"] else
                   f"{r['history']['in_history']} of {r['expected']}, missing {r['history']['missing'] or 'none'}, "
                   f"extra {r['history']['extra'] or 'none'}, in order: {'yes' if r['history']['same_order'] else 'NO'}, "
                   f"{r['history']['after_sender_stopped']} set aside after the sender stopped"),
                f"- transitions after the sender stopped (beds going stale; set aside): {len(r['after_sender_stopped'])}",
                f"- events the ring overwrote before the consumer read them: {r['lost_by_ring']}",
                f"- decided -> notified (daemon side): {fmt_stats(r['decided_to_notified_us'], 'us')}",
                f"- decided -> pulse received (R1): {fmt_stats(r['decided_to_pulse_us'], 'us')}",
                f"- decided -> event in hand (R2): {fmt_stats(r['decided_to_event_us'], 'us')}",
                f"- intentional confirmation delay (not delivery): {fmt_stats(r['confirmation_delay_ms'], 'ms')}",
                f"- engine cycles with transitions: {r['engine_cycles_with_transitions']}; events per cycle: {r['batch_sizes']}",
                f"- pulses: {r['pulses']}, of which found nothing new: {r['pulses_without_news']}; "
                f"drains that fetched several cycles at once: {r['drains_spanning_several_cycles']}", ""]
    return "\n".join(out)


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--expected", required=True)
    ap.add_argument("--run", action="append", default=[], metavar="NAME=FILE")
    ap.add_argument("--history", action="append", default=[], metavar="NAME=FILE", help="cat /dev/vigil/events")
    ap.add_argument("--notes", action="append", default=[], metavar="NAME=FILE")
    ap.add_argument("--out", default="delivery-report")
    ap.add_argument("--duration", type=float, default=0.0, help="scenario length in s; later events are set aside")
    a = ap.parse_args(argv)
    expected = expected_from(Path(a.expected).read_text())
    if not expected:
        print("no transitions found in the expected file", file=sys.stderr)
        return 2
    results = {}
    hist = {n: parse_history(Path(p).read_text()) for n, _, p in (i.partition("=") for i in a.history)}
    for item in a.run:
        name, _, path = item.partition("=")
        results[name] = analyse(expected, parse_run(Path(path).read_text()), first_time(Path(a.expected).read_text()),
                                a.duration, hist.get(name))
    notes = {n: Path(p).read_text() for n, _, p in (i.partition("=") for i in a.notes)}
    Path(a.out + ".md").write_text(render(expected, results, notes) + "\n")
    Path(a.out + ".json").write_text(json.dumps({"expected": [k._asdict() for k in expected], "runs": results}, indent=2) + "\n")
    print(render(expected, results, notes))
    bad = any(r["missing"] or r["extra"] or not r["same_order"] or r["lost_by_ring"] or r["duplicates_by_seq"]
              or (r["history"] and (r["history"]["missing"] or r["history"]["extra"])) for r in results.values())
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
