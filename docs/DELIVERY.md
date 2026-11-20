# Alarm delivery: what is measured and how

Importers: none (documentation); it describes `src/qnx/vigild.c`, `tools/vigil_watch.c`, `tools/delivery_report.py`
and `scripts/qnx-delivery.sh`. Requested by the owner: "Measure when the alarm reaches the consumer".

Question: once the engine has *decided* an alarm transition, how long until a consumer on the same machine
(`vigil-watch`) holds it? This is **not** the same as `deadline_overruns`, and it excludes the intentional
confirmation delay.

## What the existing counters mean

`/dev/vigil/stats` `deadline_overruns` counts engine wakes that came more than one period (100 ms) late after the
10 Hz timer pulse. It says nothing about publishing, notification or the consumer. The panel labels it
"late engine wakes".

## Measurement points

All timestamps are `CLOCK_MONOTONIC` of the same guest, so no clock synchronisation is involved.

| Point | Where | Meaning |
|---|---|---|
| D | `vigild.c`, `on_tick`, right after `vg_engine_step()` and the events are pushed | the transition is decided |
| N | `vigild.c`, right after `notify_subscribers()` | the secure event has been delivered to the kernel |
| R1 | `vigil_watch.c`, right after `MsgReceive()` returns the pulse | the consumer woke up |
| R2 | `vigil_watch.c`, right after the reply carrying the events returns | the consumer holds the event record |

Reported legs: D to N (daemon side), D to R1, D to R2.

**Confirmation delay, kept apart.** Each event also carries `cond_ms`, the time its condition first held, so
`decided_ms - cond_ms` is the configured onset or release delay (for example 3 s). It is reported as its own
line and never added to the delivery figures.

**Batching.** Transitions decided in the same engine cycle share one notification, so they share D and N; the
report lists events per cycle and pulses that found nothing new (already fetched by a previous drain).

**Lost events.** The report compares the received transitions with the ones the portable core produces on the
host for the same frames (`vg-replay`), so a missing, extra or reordered transition is listed, and `L` lines
count events the ring overwrote before the consumer read them.

## The synthetic sequence

Scenario `delivery` (75 s at speed 1): a desaturation (bed 1), a probe off for 12 s (bed 2), two limits crossed
in the same second (bed 3, decided in one cycle) and a bed that goes silent (bed 4). The 15 expected
transitions are fixed because noise is off. Transitions after the sender stops (all beds going stale) are set
aside and counted.

## Integrity case: eight beds, 48 transitions in one cycle

Scenario `burst8`: eight beds cross HR, SpO2, RR, SBP and TEMP limits together and NEWS2 follows, so 8 beds x 6
sources = 48 transitions are decided in a single engine cycle (then 40 on recovery and 8 NEWS2 releases later: 96 in
all). `VG_MAX_TRANSITIONS` (beds x sources) sizes the buffer that `vigild` and `vg-replay` pass to `vg_engine_step`;
the public version used 32 and 64, and the engine drops what does not fit. The unit test
`test_one_cycle_can_report_every_bed_and_source` uses that same constant and fails if it is below 48.

```bash
SCENARIO=burst8 SCEN_SECONDS=70 WATCH_IDLE=40 UDP_PORT=10110 bash scripts/qnx-delivery.sh
```

The report compares three lists by identity (bed, source, previous and new state): the expected one from `vg-replay`,
the daemon's own history (`/dev/vigil/events`) and what `vigil-watch` fetched. It counts events, not pulses (one pulse
can announce several), lists missing and duplicated events, and sets aside what happens after the sender stops.
Whether UDP puts all 48 in one cycle is measured, not assumed: the report prints the events per cycle.

## Run it

On the host (no QNX needed):

```bash
make test
python3 sim/vigil_sim.py generate delivery | out/host/vg-replay    # the 15 expected transitions
```

On QNX 8.0 under QEMU (one command; boots the guest if nothing answers on the SSH port):

```bash
UDP_PORT=10110 bash scripts/qnx-delivery.sh     # UDP_PORT: whichever port your QEMU forwards to the guest
```

It builds, deploys, runs the trial idle and then with `vigil-load` (two SCHED_FIFO 15 threads, 8 ms burst,
2 ms gap, which outrank `vigil-watch` at 10 but not the daemon), and writes
`out/delivery/<stamp>/report.md` with the conditions (commit, guest, QEMU arguments, priorities, load).

## Status and publication

* Host: instrumentation, report tool and their tests pass (`make test`).
* QNX 8.0 under QEMU (x86_64, KVM, 2 vCPUs, user-mode networking): the trial ran on 2026-11-19; every expected
  transition was received, in order, idle and loaded, none lost (15 of 15). `burst8`: 96 of 96, 48 of them decided in
  one engine cycle, no duplicates, history and `vigil-watch` agree with the expected list.
* **Numbers are not published.** QNX's non-commercial licence (section 4.6(i)) requires written approval to
  publish performance results; the organisers' answer in the forum thread is still pending. Until it arrives,
  public materials describe the method and say a run passed or failed, not how fast. A run describes only that
  run, on that machine, under that load.
