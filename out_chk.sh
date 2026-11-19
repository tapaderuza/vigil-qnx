#!/usr/bin/env bash
cd /mnt/d/vigil
printf '<!doctype html><title>x</title>\n' > /tmp/p.html
mkdir -p web && [ -f web/index.html ] || cp /tmp/p.html web/index.html
python3 tools/embed.py web/index.html src/qnx/web_index.h VG_WEB_INDEX
FLAGS="-std=gnu11 -fsyntax-only -Wall -Wextra -Wshadow -Wformat=2 -Wno-format-nonliteral -Werror -D_GNU_SOURCE -Itests/qnx-stubs"
for f in src/qnx/vigild.c src/qnx/ipc.c src/qnx/resmgr.c src/qnx/http.c tools/vigil_ctl.c tools/vigil_watch.c; do echo "== $f"; gcc $FLAGS $f 2>&1 | head -20; done
