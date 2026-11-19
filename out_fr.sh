#!/usr/bin/env bash
cd /mnt/d/vigil && make -s host && python3 sim/vigil_sim.py generate ward -o out/scn/ward.txt && ./out/host/vg-replay -f 10 < out/scn/ward.txt > out/frames_ward.ndjson 2>/dev/null; wc -l out/frames_ward.ndjson
