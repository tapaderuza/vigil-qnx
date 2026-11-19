"""The delivery report must notice a lost event, keep the confirmation delay apart from delivery, and see batches."""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import delivery_report as dr  # noqa: E402

EXPECTED = """T+00011.1s bed 1 SpO2    NONE   -> HIGH    SpO2=84
T+00011.1s bed 1 NEWS2   NONE   -> LOW     score=3
T+00016.1s bed 2 SENSOR  NONE   -> MEDIUM
"""


def m(seq, bed, src, frm, to, cond_ms, decided_ms, d, n, p, e, batch, drained):
    return "\t".join(["M", str(seq), str(bed), src, frm, to, str(cond_ms), str(decided_ms), str(d), str(n), str(p),
                      str(e), str(batch), str(drained)])


def run_text(drop_last=False):
    lines = ["# vigil-watch measurement, CLOCK_MONOTONIC resolution 1000 ns, started at 5 ns",
             # two transitions decided in the same engine cycle (d=1_000_000_000), fetched by one pulse
             m(1, 1, "SpO2", "NONE", "HIGH", 8100, 11100, 1_000_000_000, 1_000_050_000, 1_000_200_000, 1_000_500_000, 2, 2),
             m(2, 1, "NEWS2", "NONE", "LOW", 8100, 11100, 1_000_000_000, 1_000_050_000, 1_000_200_000, 1_000_500_000, 2, 2),
             "P\t1000200000\t1000500000\t2"]
    if not drop_last:
        lines += [m(3, 2, "SENSOR", "NONE", "MEDIUM", 16100, 16100, 6_000_000_000, 6_000_020_000, 6_000_100_000,
                    6_000_300_000, 1, 1), "P\t6000100000\t6000300000\t1"]
    lines.append("P\t6000100500\t6000300500\t0")      # a coalesced pulse: nothing new
    return "\n".join(lines)


def test_complete_run_matches_and_measures_each_leg():
    exp = dr.expected_from(EXPECTED)
    assert len(exp) == 3
    r = dr.analyse(exp, dr.parse_run(run_text()))
    assert r["received"] == 3 and not r["missing"] and not r["extra"] and r["same_order"]
    assert r["decided_to_notified_us"]["median"] == 50.0          # 50 000 ns
    assert r["decided_to_pulse_us"]["max"] == 200.0 and r["decided_to_event_us"]["max"] == 500.0
    assert r["batch_sizes"] == {"1": 1, "2": 1} and r["engine_cycles_with_transitions"] == 2
    assert r["pulses"] == 3 and r["pulses_without_news"] == 1


def test_confirmation_delay_is_kept_apart_from_delivery():
    r = dr.analyse(dr.expected_from(EXPECTED), dr.parse_run(run_text()))
    assert r["confirmation_delay_ms"]["max"] == 3000.0            # 3 s of onset, decided at 11 100 ms
    assert r["decided_to_event_us"]["max"] < 1000.0               # microseconds, not seconds: the two never mix


def test_a_lost_transition_is_reported():
    r = dr.analyse(dr.expected_from(EXPECTED), dr.parse_run(run_text(drop_last=True)))
    assert r["received"] == 2 and r["missing"] == [{"bed": 2, "src": "SENSOR", "frm": "NONE", "to": "MEDIUM"}]
    assert not r["same_order"]


def test_events_overwritten_in_the_ring_are_counted():
    text = run_text() + "\nL\t4"
    assert dr.analyse(dr.expected_from(EXPECTED), dr.parse_run(text))["lost_by_ring"] == 4


def test_report_text_states_it_describes_one_run_only():
    exp = dr.expected_from(EXPECTED)
    out = dr.render(exp, {"idle": dr.analyse(exp, dr.parse_run(run_text()))}, {"idle": "2 vCPUs, no load"})
    assert "describes that run only" in out and "2 vCPUs, no load" in out


def test_stale_transitions_after_the_sender_stopped_are_set_aside():
    text = run_text() + "\n" + m(9, 2, "OFFLINE", "NONE", "MEDIUM", 0, 99000, 90_000_000_000, 90_000_050_000,
                                 90_000_100_000, 90_000_300_000, 1, 1)
    r = dr.analyse(dr.expected_from(EXPECTED), dr.parse_run(text), first_expected_s=11.1, duration_s=20.0)
    assert not r["extra"] and r["same_order"] and len(r["after_sender_stopped"]) == 1
