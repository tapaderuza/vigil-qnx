# Demo video (about 2 minutes) and gallery captions

Every capture carries a label on screen: **HOST** (the dashboard served by the fake daemon or by vigild on a PC),
**REPLAY** (`?src=` recorded data) or **QNX** (terminal or dashboard of the QNX 8.0 guest). Enlarge the text: browser
zoom 175 % for the dashboard, terminal font 20 pt or more, and crop to one bed or one line, because Devpost's embedded
player is small. Synthetic patients, not a medical device: keep that line visible in the title and the closing card.

## Script

| Time | Label | Screen | Voice-over / caption |
|---|---|---|---|
| 0:00 | QNX | Dashboard, bed 01, NEWS2 climbing in the `deterioration` scenario, large | "Bed 01 is getting worse. The early warning score alarms first." |
| 0:08 | QNX | Same bed, overlay "NEWS2 alarm at 531 s, first single-value alarm at 737 s" | "That is 3 min 26 s of simulated time before any single value alarms." |
| 0:15 | | Title: "Vigil, a bedside alarm prototype on QNX 8.0. Synthetic patients, not a medical device." | |
| 0:25 | QNX | Terminal: `uname -a`, `ls /dev/vigil`, `cat /dev/vigil/alarms` | "Vigil runs on QNX 8.0 under QEMU and publishes the ward as files." |
| 0:40 | HOST | Dashboard, a probe is unplugged on bed 02 (scenario `probe_off`) | "A probe is disconnected: technical alarm, and that value stops being judged." |
| 0:55 | HOST | Daemon stops: banner "VIGILD NOT REACHABLE", values marked HISTORIC, time of the last valid update | "If the daemon stops, the panel says so itself. The values go historic; they are never shown as live, and never as Stable." |
| 1:15 | HOST | Daemon back: banner disappears, live again on the first fresh sample | "On the first fresh sample it recovers." |
| 1:30 | QNX | Terminal: `vigil-watch -m` receiving a transition while the scenario runs | "A transition decided by the engine reaches a client as a kernel pulse. No polling." |
| 1:45 | QNX | Terminal: `scripts/qnx-delivery.sh` ending with "received 15 of 15 expected ... in the expected order: yes" (no microsecond figures until the organisers approve) | "We compare expected and received transitions and report any lost event." |
| 1:55 | | Closing card: repository URL, `docs/CODELAB.md` | "Code, tutorial and method are in the repository." |

Do not show or say latency numbers, "zero missed deadlines" or "never stale" in absolute form.

## Exact capture steps

```bash
# HOST scenes: a fake daemon with switchable faults serves the dashboard (needs Python 3)
python3 tests/web/fake_vigild.py --port 8113 &
curl -X POST localhost:8113/__mode/normal       # 0:40
curl -X POST localhost:8113/__mode/hang         # 0:55 (also: error, frozen)
curl -X POST localhost:8113/__mode/normal       # 1:15 recovery
# open http://localhost:8113/ and record the browser window

# QNX scenes (guest booted by scripts/run-qemu.sh, SSH on 2222)
python3 sim/vigil_sim.py send deterioration --host 127.0.0.1 --port 10110 --speed 20   # NEWS2 alarm after ~27 s
UDP_PORT=10110 bash scripts/qnx-delivery.sh     # 1:45; writes out/delivery/<stamp>/report.md
```

## Gallery captions (four images, English)

1. **Bed 01 on the dashboard (QNX).** NEWS2 alarms 3 min 26 s (simulated time) before the first single-value alarm in
   the `deterioration` scenario. Synthetic patient.
2. **Connection lost (QNX 8.0).** The panel's own banner, values marked historic, and the time of the last valid update.
3. **Transition received by `vigil-watch` (QNX 8.0).** A secure event wakes the consumer; one line per transition.
4. **Delivery trial report (QNX 8.0 under QEMU).** Expected against received transitions, none lost. A run describes
   only that run. No timing figures are shown pending the organisers' written answer on the licence.

## Status

Recorded on 2026-11-20 from one QNX 8.0 run (QEMU/KVM in WSL2): the panel at `http://127.0.0.1:8080/?clean=1` (the
`clean` switch hides the engine-timing card so no timing figure appears), `vigil-watch`, the 8-bed integrity case and
the codelab example. `tools/demo_build.py --dir out/demo` renders the MP4 (about 1:49, no audio) and the four 3:2
gallery images from those captures; terminal text is rendered from captured output, not retyped. The 3 min 26 s lead
is scenario (simulated) time, from replaying the same frames; the live feed ran about 4 % slower than real time, so
the daemon's wall-clock gap on that run was longer. Latency figures stay out of every public asset.
