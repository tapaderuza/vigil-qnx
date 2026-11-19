# Devpost submission text and video script (QNX Global Hackathon)

Category to select: **Robotics & Medical**. Also tick the bonus prizes that apply: *Best use of
QNX-specific technologies* and *Best Codelab/Tutorial* (URL of the codelab: docs/CODELAB.md published).

Do not publish latency numbers as QNX benchmark claims until the organisers answer the licence question in
the forum thread "Can hackathon entries publish latency measurements made on QNX 8.0?". The text below
contains no measured timing figures.

## Project name

Vigil: a patient-monitor alarm engine for QNX

## Tagline

Alarms on schedule, with priorities, without crying wolf, and never a stale number shown as live.

## Description (paste into the story box)

### Inspiration

An alarm that fires late is a failure. An alarm that fires too often is also a failure: alarm fatigue is a
well-documented hazard on hospital wards, because staff learn to ignore a monitor that cries wolf. And the
quietest failure of all is a monitor that keeps showing the last reading after a probe has fallen off. That
combination of timing, restraint and honesty is exactly what a real-time operating system is for, so we built
a bedside alarm engine on QNX 8.0.

### What it does

Vigil reads vital-sign frames for every bed (heart rate, SpO2, respiration, blood pressure, temperature,
consciousness, oxygen) over UDP, checks them, and raises alarms the way a safety-critical system should.

* **Priorities and delays.** HIGH, MEDIUM and LOW alarms after the concepts of IEC 60601-1-8. A limit must stay
  violated for a few seconds before an alarm is raised, so one noisy sample never wakes a nurse, and the value must
  stay back in range before it is lowered, with hysteresis so a reading hovering on a limit cannot flicker.
* **NEWS2 early warning score.** A patient who is slowly getting worse is flagged by the score about three minutes
  before any single value crosses its own limit (in the bundled `deterioration` scenario).
* **Honest about faults.** A probe that falls off, or a bed that goes silent, raises a technical alarm and the
  affected value stops being judged. The stale number is never shown as if it were current.
* **Audio pause that cannot hide anything.** It silences the sound for a fixed time; the visual alarm stays.

It is shown three ways: a quiet bedside dashboard, plain files under /dev/vigil, and a live feed driven by
kernel pulses.

### How it uses QNX 8.0 (where to look)

* **Timer pulse and a SCHED_FIFO engine** (`src/qnx/vigild.c`, `on_tick`, `main`): the alarm engine wakes on a kernel
  timer pulse and measures its own wake latency and cycle time on every cycle.
* **Message passing as the only synchronisation** (`vigild.c`, `ipc.c`, `vigil_ipc.h`): the engine owns all state with
  no locks. The ingest thread, at a lower priority, decodes the network feed and MsgSends one message per valid
  frame, so a flood of garbage costs the engine almost nothing and is preempted by every timer pulse.
* **Secure events** (`tools/vigil_watch.c`, `handle_subscribe`): a nurse-station client registers a pulse with
  MsgRegisterEvent and the engine fires it with MsgDeliverEvent when an alarm changes. No polling, and only the
  daemon can trigger it.
* **Resource manager** (`src/qnx/resmgr.c`): `cat /dev/vigil/alarms`, and
  `echo "hr_high_med 110" > /dev/vigil/config` changes a limit live; an invalid limit is rejected.
* **Priority-inheriting mutex, slog2, mlockall** for bounded readers, an audit trail of every alarm, and no page faults.

### How we built it

The decision logic (frame decoding, NEWS2, the alarm engine) is portable C11 with no allocation and no QNX
dependency, so the same sources run in 106 unit checks on a PC and on QNX. A simulator generates synthetic
patients (a gradual decline, an acute desaturation, a probe fault, a silent bed, a busy ward) and 20 regression
scenarios assert what a clinician would expect. The QNX layer is a thin set of threads around that core.

### Challenges and what running on the target taught us

Running the host tests on the target exposed a real platform difference: on QNX 8.0, strtod("36.8") returns a value
one ulp above the compiler literal, so an exact comparison that passes on a PC failed on the device. We now
normalise every vital sign to one decimal, so decisions are identical everywhere, and kept the test as a guard.
Other lessons: alarms need hysteresis in value and in time or they flicker; and technical alarms must end the
moment their cause does, while physiological alarms should wait.

### What is next

Serial and HL7 input, a nurse-call integration through the secure-event channel, QNX security policies for the
daemon, and a Raspberry Pi 5 build with a real SpO2 sensor.

### Built with

C11, QNX SDP 8.0 (qcc), QNX Neutrino message passing, resource managers, slog2, Python 3, pytest, HTML.

### Safety note

Illustrative software with synthetic patients. It is not a medical device and must not be used for patient care.

## Fields to fill on the form

* Public repository URL: (the new repository; MIT licence in the root).
* Build and run instructions: README, section Build and run (QNX SDP 8.0, target QEMU x86_64, qcc gcc_ntox86_64).
* Evidence of QNX use: `src/qnx/vigild.c`, `resmgr.c`, `ipc.c`, `tools/vigil_watch.c`, the Makefile target `qnx`,
  `docs/QNX_RUN.md`, and the demo in the video.
* Video: YouTube, public, under 3 minutes, no third-party trademarks or copyrighted music.
* Codelab URL: the published `docs/CODELAB.md`.

## Video script (about 2:30; real captures from the QNX VM)

| Time | Screen | Voice-over |
|---|---|---|
| 0:00 | Title card | "Vigil is a patient-monitor alarm engine. It runs on QNX 8.0." |
| 0:08 | Why it matters | "A late alarm is a failure. A constant alarm is a failure. And a stale number shown as live is the worst." |
| 0:22 | Terminal in the QNX VM: uname, ls /dev/vigil | "This is QNX 8.0 under QEMU. The engine publishes the ward as files." |
| 0:35 | Dashboard, six stable beds | "Six synthetic patients. Everything is calm." |
| 0:50 | Dashboard, bed 1 NEWS2 rising | "One patient is slowly getting worse. No single value is alarming yet, but the early warning score is." |
| 1:10 | Dashboard, bed 3 sensor off | "A probe falls off. Vigil raises a technical alarm and stops judging that number instead of showing a stale one." |
| 1:30 | vigil-watch output | "Alarm changes reach a client as kernel pulses: a secure event, no polling." |
| 1:50 | cat alarms, pause, config write | "Audio can be paused, but the visual alarm stays. Limits change live through a file; a bad limit is rejected." |
| 2:10 | Flood card | "Under a flood with corrupt frames the engine keeps its schedule: zero missed deadlines." |
| 2:25 | Tests on QNX, repo URL | "The same core passes 106 checks on QNX. Code, tutorial and instructions are in the repository." |
