#!/usr/bin/env bash
cd /mnt/d/vigil && mkdir -p out/host out/scn
gcc -std=c11 -O2 -Wall -Wextra -Werror -D_DEFAULT_SOURCE -o out/host/vg-replay tools/vg_replay.c src/core/nmea.c src/core/vitals.c src/core/alarms.c -lm || exit 1
for s in stable deterioration desat probe_off offline ward; do
  python3 sim/vigil_sim.py generate $s -o out/scn/$s.txt
  echo "== $s"; ./out/host/vg-replay -t 20 < out/scn/$s.txt 2>&1 | head -24
done
