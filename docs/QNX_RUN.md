# Verified run on QNX 8.0

Environment: QNX SDP 8.0 (QNX Everywhere, non-commercial licence), `qcc` 12.2.0 (`gcc_ntox86_64`), image built with
`mkqnximage --type=qemu --arch=x86_64`, booted with QEMU + KVM inside WSL2 on Windows 11, user-mode networking.
Guest: `QNX noname 8.0.0 2026/10/30-09:10:05EDT x86pc x86_64`.

| Step | Result |
|---|---|
| `make qnx QNX_ARCH=x86_64` (`-Werror`) | all six binaries build: vigild, vigil-ctl, vigil-watch, vigil-load, test_core, vg-replay |
| `test_core` executed ON QNX | 149 checks, 0 failures |
| `vigild` start | engine (priority 40), resource manager (25), ingest UDP (20), HTTP 8080 (10); five devices under `/dev/vigil` |
| `ward` scenario streamed over UDP | 6905 frames, 0 rejected; bed 2 desaturation raised and cleared, bed 1 NEWS2 escalates to HIGH, bed 3 probe-off as a technical alarm, bed 4 reported offline |
| `vigil-watch` (secure event + pulse) | received the transitions without polling |
| flood: 93,674 datagrams with 30 percent corrupt frames | 21,524 corrupt frames rejected, all beds still tracked |
| `vigil-ctl pause` | audible state paused and reported, the visual alarm stays (Vigil produces no sound) |
| `echo "hr_high_med 110" > /dev/vigil/config` | applied; `hr_high_med 999` is rejected with exit 1 |

The same scenarios give the same alarm transitions on the host (`sim/test_sim.py`) and on QNX.
No latency figures are published here: the non-commercial licence restricts publishing performance results (see `docs/DELIVERY.md`).
The counts above (tests, frames, transitions) come from the 2026-10 run on that guest; later runs describe only themselves.
The delivery trial of 2026-11-19 on that guest received all 15 expected transitions, in order, idle and under load.

## Finding: strtod on QNX 8.0

Running the unit tests on the target failed one check that passes on the host: `strtod("36.8")` returns
`0x4042666666666667` on QNX 8.0, one ulp above the compiler literal `36.8` (`0x4042666666666666`); `98.6` shows the same.
`38.5`, `37.1`, `0.1` are fine. Vigil therefore normalises each vital sign to one decimal (`round(v*10)/10.0`),
which is correctly rounded on every platform, and the check stays in the test-suite as a guard.
