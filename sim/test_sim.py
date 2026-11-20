"""Clinical scenario regression tests for Vigil.

They run the *same C sources* that are built for QNX 8.0 through `vg-replay`, in simulated time,
over the synthetic patients of `vigil_sim.py`, and assert what a clinician would expect to see.
"""
from __future__ import annotations

import json
import re
import subprocess
from pathlib import Path
from typing import List, NamedTuple, Optional

import pytest

import vigil_sim as sim

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "out" / "host"
TRANSITION = re.compile(r"T\+(?P<t>\d+\.\d)s bed (?P<bed>\d) (?P<src>\w+)\s+(?P<frm>\w+)\s+-> (?P<to>\w+)")


class Tr(NamedTuple):
    t: float
    bed: int
    src: str
    frm: str
    to: str


def run(*args: str, stdin: str = "", check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run([str(BIN / "vg-replay"), *args], input=stdin, capture_output=True, text=True,
                          check=check, timeout=120)


def replay(name: str, *args: str, duration: Optional[float] = None) -> tuple[List[Tr], str]:
    cp = run(*args, stdin=sim.scenario_text(name, duration))
    trs = [Tr(float(m["t"]), int(m["bed"]), m["src"], m["frm"], m["to"])
           for m in (TRANSITION.match(ln) for ln in cp.stdout.splitlines()) if m]
    return trs, cp.stdout


def first(trs: List[Tr], bed: int, src: str, to: str) -> Optional[Tr]:
    return next((x for x in trs if x.bed == bed and x.src == src and x.to == to), None)


def test_stable_patients_never_alarm():
    trs, _ = replay("stable")
    assert trs == []


def test_gradual_decline_is_flagged_by_news2_before_any_single_limit():
    trs, _ = replay("deterioration")
    news2 = first(trs, 1, "NEWS2", "MEDIUM")
    assert news2 is not None and 480 < news2.t < 600
    single = [x for x in trs if x.bed == 1 and x.src in ("HR", "SpO2", "RR", "SBP", "TEMP") and x.to != "NONE"]
    assert single and min(x.t for x in single) > news2.t + 120   # the score warns minutes earlier
    high = first(trs, 1, "NEWS2", "HIGH")
    assert high is not None and high.t < 650                       # escalates to the clinical emergency band
    assert not [x for x in trs if x.bed == 2]                      # the other patient is untouched


def test_alarm_escalates_cleanly_without_flicker_on_the_decline():
    trs, _ = replay("deterioration")
    order = {"NONE": 0, "LOW": 1, "MEDIUM": 2, "HIGH": 3}
    for src in ("HR", "RR", "TEMP", "SBP", "SpO2", "NEWS2"):
        moves = [x for x in trs if x.bed == 1 and x.src == src and x.t < 1190]
        # Each source only ever goes up while the patient keeps worsening (value + release hysteresis).
        assert all(order[x.to] > order[x.frm] for x in moves), src


def test_acute_desaturation_is_raised_after_the_onset_delay_and_cleared_after_recovery():
    trs, _ = replay("desat")
    up = first(trs, 1, "SpO2", "HIGH")
    down = first(trs, 1, "SpO2", "NONE")
    assert up is not None and 302.0 <= up.t <= 306.0       # drop at 300 s + 3 s onset
    assert down is not None and 364.0 <= down.t <= 372.0   # recovery at 361 s + 5 s release


def test_probe_off_is_a_technical_alarm_and_the_stale_value_stops_alarming():
    trs, _ = replay("probe_off")
    t = first(trs, 1, "SENSOR", "MEDIUM")
    assert t is not None and 303 <= t.t <= 309
    # While the probe is off (about 306 to 420 s) the old, genuinely low value must not be alarming.
    spo2 = [x for x in trs if x.src == "SpO2"]
    assert [x for x in spo2 if 310 < x.t < 420] == []
    assert any(x.src == "SpO2" and x.to == "NONE" and 300 <= x.t <= 310 for x in trs)
    back = [x for x in trs if x.src == "SpO2" and x.to == "HIGH" and x.t > 400]
    assert back and 421 <= back[0].t <= 427                  # the probe returns: judged again after the onset
    assert any(x.src == "SENSOR" and x.to == "NONE" and 418 <= x.t <= 422 for x in trs)


def test_a_silent_bed_is_reported_offline_and_the_neighbour_is_not():
    trs, _ = replay("offline", duration=330)
    off = first(trs, 1, "OFFLINE", "MEDIUM")
    assert off is not None and 308 <= off.t <= 313           # about 10 s after the last frame
    assert [x for x in trs if x.bed == 2] == []


def test_busy_ward_alarms_each_bed_for_its_own_reason_only():
    trs, _ = replay("ward")
    assert first(trs, 2, "SpO2", "HIGH") is not None and first(trs, 3, "SENSOR", "MEDIUM") is not None
    assert first(trs, 4, "OFFLINE", "MEDIUM") is not None and first(trs, 1, "NEWS2", "MEDIUM") is not None
    assert not [x for x in trs if x.bed in (5, 6) and x.t < 1195]   # the two stable beds stay quiet


def test_corrupt_frames_are_counted_and_never_reach_the_engine():
    lines = sim.scenario_text("stable", 30).splitlines()
    bad = [ln[:-2] + "00" if i % 5 == 1 else ln for i, ln in enumerate(lines)]
    cp = run("-j", stdin="\n".join(bad) + "\n")
    assert "0 bad" not in cp.stderr and "bad)" in cp.stderr
    assert json.loads(cp.stdout.splitlines()[-1])["health"]["bad_frames"] > 0


def test_replay_is_deterministic():
    a = replay("ward", "-j")[1]
    assert a == replay("ward", "-j")[1]


def test_thresholds_are_configurable_and_validated():
    base, _ = replay("deterioration")
    early, _ = replay("deterioration", "-c", "hr_high_med 100")
    assert first(early, 1, "HR", "MEDIUM").t < first(base, 1, "HR", "MEDIUM").t
    bad = run("-c", "hr_high_med 999", stdin="", check=False)
    assert bad.returncode == 2 and "limits must satisfy" in bad.stderr


def test_audio_pause_is_reported_but_the_alarm_stays_visible():
    _, out = replay("deterioration", "-s", "1150", "-a")
    assert "AUDIO PAUSED" in out and "HIGH" in out


def test_bed_table_and_frames_are_well_formed():
    _, out = replay("ward", "-b", duration=1000)
    assert "NEWS2" in out and "OFFLINE" in out
    cp = run("-f", "60", stdin=sim.scenario_text("ward", 300))
    frames = [json.loads(ln) for ln in cp.stdout.splitlines() if ln.startswith("{")]
    assert 4 <= len(frames) <= 6 and all("beds" in f and "audio" in f for f in frames)


def test_replay_rejects_time_going_backwards():
    cp = run(stdin="1000 $VGVIT,1,72,98,14,122,36.8,A,0*00\n500 $VGVIT,1,72,98,14,122,36.8,A,0*00\n", check=False)
    assert cp.returncode == 2 and "backwards" in cp.stderr


def test_every_documented_scenario_exists():
    assert {"stable", "deterioration", "desat", "probe_off", "offline", "ward", "delivery"} <= set(sim.SCENARIOS)


def test_delivery_scenario_has_every_kind_of_transition_and_a_batch():
    """The delivery trial needs known transitions, including several decided in one engine cycle."""
    trs, _ = replay("delivery")
    kinds = {(x.src, x.to) for x in trs}
    assert ("SpO2", "HIGH") in kinds and ("SpO2", "NONE") in kinds         # raised after the onset, lowered after the release
    assert ("SENSOR", "MEDIUM") in kinds and ("SENSOR", "NONE") in kinds   # technical alarm and its end
    assert ("OFFLINE", "MEDIUM") in kinds                                  # a bed that goes silent
    same_cycle = {}
    for x in trs:
        same_cycle.setdefault((x.bed, x.t), []).append(x)
    assert any(len(v) >= 2 for v in same_cycle.values()), "no two transitions share an engine cycle"
    assert len(trs) >= 10


def test_delivery_scenario_is_deterministic():
    assert replay("delivery")[0] == replay("delivery")[0]


@pytest.mark.parametrize("name", sorted(sim.SCENARIOS))
def test_frames_use_the_protocol_and_have_valid_checksums(name):
    for ln in sim.scenario_text(name, 20).splitlines():
        if ln.startswith("#"):
            continue
        _, s = ln.split(" ", 1)
        body, ck = s[1:].split("*")
        assert s.startswith("$VGVIT,") and sim.checksum(body) == ck


def test_burst8_decides_48_transitions_in_one_engine_cycle():
    """Eight beds x six changes. vg-replay uses the daemon's buffer (VG_MAX_TRANSITIONS), so none may be dropped."""
    trs, _ = replay("burst8")
    first = [x for x in trs if x.t == trs[0].t]
    assert len(first) == 48 and {x.bed for x in first} == set(range(1, 9)) and len({(x.bed, x.src) for x in first}) == 48
    assert len(trs) == 96
