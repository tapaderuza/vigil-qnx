#!/usr/bin/env bash
cd /mnt/d/vigil && mkdir -p out/host && gcc -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wformat=2 -Wno-format-nonliteral -Werror -D_DEFAULT_SOURCE -o out/host/test_core tests/unit/test_core.c src/core/nmea.c src/core/vitals.c src/core/alarms.c -lm 2>&1 | head -30 && ./out/host/test_core | tail -22
