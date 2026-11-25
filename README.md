# Vigil: a patient-monitor alarm engine for QNX 8.0

Vigil watches the vital signs of a ward and raises alarms the way a safety-critical system should:
with priorities, with a confirmation delay so one noisy sample does not raise an alarm, and with a freshness
rule: a reading that stops arriving is not shown as live. It runs on **QNX 8.0** (x86_64 under QEMU, and aarch64le for Raspberry Pi 4/5).
Category: *Robotics & Medical*.

> Illustrative software, not a medical device. It uses synthetic patients only and must not be used
> for patient care.

## What it does

* Reads frames of vital signs per bed (heart rate, SpO2, respiration, blood pressure, temperature,
  consciousness, oxygen) over UDP, with checksums, in a small NMEA-style protocol.
* Raises **alarms by priority** (HIGH, MEDIUM, LOW), after the concepts of IEC 60601-1-8: a condition
  must persist before the alarm is raised (so one noisy sample never wakes a nurse), and must stay
  clear before it is lowered, with value hysteresis so a reading hovering on a limit cannot flicker.
* Computes the **NEWS2** early warning score (Royal College of Physicians, 2017) using **SpO2 scale 1**
  (the scale for patients without hypercapnic respiratory failure; scale 2 is not implemented). If a needed value
  (including consciousness or oxygen) is missing or older than the freshness limit, NEWS2 is shown as
  *not calculable*, never as a number. In the bundled `deterioration` scenario the first NEWS2 alarm comes 206 s
  (3 min 26 s, simulated time) before the first single-value alarm: 531.1 s against 737.1 s.
* Treats **technical faults separately**: a probe that falls off, or a bed that goes silent, raises a
  technical alarm and the affected value stops being judged. Each value expires on its own clock (default 5 s),
  and the dashboard marks its own readings *historic* if vigild stops answering or stops advancing.
* **Audible state and pause.** Vigil does not produce sound. It exposes whether an alarm is *audible* and lets
  an operator pause that state for a fixed time; the visual indication never pauses. A real speaker or
  nurse-call output is left to the integrator.

## Why QNX

| QNX feature | Where | What it buys |
|---|---|---|
| Timer **pulses** (`timer_create` + `SIGEV_PULSE`) | `src/qnx/vigild.c` | the alarm engine wakes on a kernel-delivered 10 Hz pulse; its own wake latency is measured every cycle (`deadline_overruns` counts engine wakes that were more than one period late; it does not cover publishing or notification) |
| **Fixed priorities** (SCHED_FIFO 40 / 25 / 20 / 10) | `vgd_spawn()` | engine preempts the resource manager, which preempts ingest, which preempts the web view |
| **Synchronous message passing** (`name_attach`, `MsgSend`, `MsgReceive`, `MsgReply`) | `vigild.c`, `ipc.c` | the engine owns the decision state and is fed by messages from ingest and clients; one priority-inheriting mutex guards the published snapshot that readers copy |
| **Secure events** (`MsgRegisterEvent`, `MsgDeliverEvent`) | `tools/vigil_watch.c` | alarm changes pushed to a nurse-station client with no polling; only the daemon can fire the event |
| **Resource manager** (`resmgr_attach`, `iofunc_*`) | `src/qnx/resmgr.c` | `cat /dev/vigil/alarms`; `echo "hr_high_med 110" > /dev/vigil/config` |
| Priority-inheriting mutex, slog2, mlockall | `vigild.c` | bounded readers, an audit trail of every alarm, no page faults |
| **Least privilege** with process-manager abilities (`procmgr_ability`, deny + lock) | `src/qnx/privs.c` | vigild needs root only to set up. Once its threads hold their channel, `/dev/vigil` paths and sockets, it denies and locks, for root and non-root alike, spawn, fork, setuid, new path names, physical memory, port I/O, interrupts, setting the clock, reboot and cross-process debug. Locked abilities cannot come back, so a compromised monitor cannot start programs or touch hardware. It proves it at start-up: a spawn attempt must fail with EPERM (logged to slog2). `-L` keeps the abilities for debugging |

```
 UDP vital-sign frames            (priority 20)                 (priority 40)
 ─────────────────────►  ingest thread ──MsgSend──►  ALARM ENGINE  ◄── 10 Hz timer pulse
                         decode + checksum           beds, limits, NEWS2,     ▲   ▲
                                                     delays, technical alarms │   │ MsgSend / MsgDeliverEvent
 /dev/vigil/{beds,alarms,stats,events,config} ◄── resmgr thread (25) ─────────┘   │
 http://host:8080  bedside dashboard + /api/*   ◄── http thread (10) ──────────────┘   vigil-ctl, vigil-watch
```

## No AI in the alarm path, on purpose

Alarms that wake a nurse must be explainable and repeatable, so Vigil decides with published rules (IEC 60601-1-8 alarm concepts and the NEWS2 score), never with a model. Every alarm traces back to the value, the limit and the delay that caused it, and the same input always gives the same alarms.

## Repository map

* `src/core/`: portable C11, no allocation, no QNX dependency (frame decoding, NEWS2, alarm engine,
  event ring, renderers). Built with gcc for tests and with `qcc` for QNX.
* `src/qnx/`: the QNX-only part (daemon, resource manager, ingest, HTTP) and the message protocol.
* `tools/`: `vigil-ctl` and `vigil-watch` (native QNX clients), `vg-replay` (run the engine over a recorded stream).
* `sim/`: synthetic patient simulator and the pytest scenario suite.
* `web/index.html`: the bedside dashboard, compiled into the daemon.
* `docs/CODELAB.md`: tutorial, *Your first QNX resource manager and secure events*.

## Build and run

```bash
sudo bash scripts/setup-wsl.sh     # Ubuntu 24.04 / WSL2: gcc, QEMU, pytest
make test                           # 149 C checks + 34 pytest tests + 11 dashboard tests
source ~/qnx800/qnxsdp-env.sh       # QNX SDP 8.0 (free, QNX Everywhere)
make qnx                            # qcc -Vgcc_ntox86_64 ; QNX_ARCH=aarch64le for the Pi
bash scripts/qnx-demo.sh ward       # boots QNX in QEMU, runs the tests ON QNX, streams a ward
```

On the target:

```sh
vigild -f &                         # engine + resource manager + ingest (UDP 10120) + HTTP 8080
cat /dev/vigil/alarms               # active alarms by priority
vigil-watch                         # live alarm feed driven by kernel pulses
vigil-ctl pause                     # audio pause (visual alarms stay)
slog2info -b vigil                  # audit trail
```

Feed it with `python3 sim/vigil_sim.py send ward --host <vm-ip> --speed 20`.

## Verified on QNX 8.0

See `docs/QNX_RUN.md`: built with `qcc` (`-Werror`), all 149 core checks executed on QNX, the `ward`
scenario streamed over UDP, a flood with corrupt frames, live configuration through the resource manager.
Running the host tests on the target also exposed a real platform difference: `strtod("36.8")` on
QNX 8.0 is one ulp above the compiler's literal. Vigil normalises vital signs to one decimal, so
decisions are identical on every platform.

## Delivery measurement

`docs/DELIVERY.md` describes how to measure the time from a decided alarm transition to its reception by
`vigil-watch` (`scripts/qnx-delivery.sh`). No figures are published here: the QNX non-commercial licence asks for
written approval first, and the organisers' answer is pending.

## Licence

MIT (see `LICENSE`). QNX and related marks belong to BlackBerry Limited; this project is independent
and not affiliated with or endorsed by BlackBerry or QNX. NEWS2 is published by the Royal College of
Physicians; IEC 60601-1-8 is referenced only for its published concepts.
