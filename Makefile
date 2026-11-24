# Vigil - build for the host (tests and tools) and for QNX 8.0 (the target).
#
#   make            host build: unit tests + vg-replay
#   make test       host unit tests + Python scenario tests
#   make qnx        QNX 8.0 build with qcc (source qnxsdp-env.sh first); QNX_ARCH=x86_64 or aarch64le
#   make check-qnx-syntax   type-check QNX sources against stand-in headers (no SDP needed)
#
# SPDX-License-Identifier: MIT

CORE_SRC  := src/core/nmea.c src/core/vitals.c src/core/alarms.c
CORE_HDR  := $(wildcard src/core/*.h)
QNX_SRC   := src/qnx/vigild.c src/qnx/resmgr.c src/qnx/ipc.c src/qnx/http.c src/qnx/privs.c
QNX_HDR   := $(wildcard src/qnx/*.h) src/qnx/web_index.h

WARN      := -Wall -Wextra -Wshadow -Wformat=2 -Wno-format-nonliteral
WERROR    ?= -Werror
OPT       ?= -O2 -g

HOST_CC   ?= gcc
HOST_OUT  := out/host
HOST_CFLAGS := -std=c11 $(OPT) $(WARN) $(WERROR) -D_DEFAULT_SOURCE

QNX_ARCH  ?= x86_64
ifeq ($(QNX_ARCH),x86_64)
QNX_VARIANT := gcc_ntox86_64
else ifeq ($(QNX_ARCH),aarch64le)
QNX_VARIANT := gcc_ntoaarch64le
else
$(error QNX_ARCH must be x86_64 or aarch64le)
endif
QNX_CC    ?= qcc
QNX_OUT   := out/qnx-$(QNX_ARCH)
QNX_CFLAGS := -V$(QNX_VARIANT) -std=gnu11 $(OPT) $(WARN) $(WERROR) -D_QNX_SOURCE
QNX_LIBS  := -lsocket -lm -lslog2

HOST_BINS := $(HOST_OUT)/test_core $(HOST_OUT)/vg-replay
QNX_BINS  := $(QNX_OUT)/vigild $(QNX_OUT)/vigil-watch $(QNX_OUT)/vigil-ctl $(QNX_OUT)/vigil-load $(QNX_OUT)/test_core $(QNX_OUT)/vg-replay

.PHONY: all host test test-unit test-sim test-web qnx check-qnx-syntax clean
all: host
host: $(HOST_BINS)

$(HOST_OUT):
	mkdir -p $@

$(HOST_OUT)/test_core: tests/unit/test_core.c $(CORE_SRC) $(CORE_HDR) | $(HOST_OUT)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ tests/unit/test_core.c $(CORE_SRC) -lm

$(HOST_OUT)/vg-replay: tools/vg_replay.c $(CORE_SRC) $(CORE_HDR) | $(HOST_OUT)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ tools/vg_replay.c $(CORE_SRC) -lm

test: test-unit test-sim test-web
test-unit: $(HOST_OUT)/test_core
	$(HOST_OUT)/test_core
test-sim: $(HOST_OUT)/vg-replay
	python3 -m pytest -q sim
# Dashboard freshness logic (needs Node 18+). Without Node the step says so loudly instead of passing silently.
test-web:
	@if command -v node >/dev/null 2>&1; then node --test tests/web/freshness.test.js; \
	else echo "test-web: SKIPPED, node not found (install Node 18+ to run the dashboard tests)"; fi

# The dashboard is embedded in vigild so the target needs no filesystem assets.
src/qnx/web_index.h: web/index.html tools/embed.py
	python3 tools/embed.py web/index.html $@ VG_WEB_INDEX

qnx: $(QNX_BINS)
$(QNX_OUT):
	mkdir -p $@

$(QNX_OUT)/vigild: $(QNX_SRC) $(QNX_HDR) $(CORE_SRC) $(CORE_HDR) | $(QNX_OUT)
	$(QNX_CC) $(QNX_CFLAGS) -o $@ $(QNX_SRC) $(CORE_SRC) $(QNX_LIBS)
$(QNX_OUT)/vigil-watch: tools/vigil_watch.c src/qnx/vigil_ipc.h $(CORE_SRC) $(CORE_HDR) | $(QNX_OUT)
	$(QNX_CC) $(QNX_CFLAGS) -o $@ tools/vigil_watch.c $(CORE_SRC) -lm
$(QNX_OUT)/vigil-ctl: tools/vigil_ctl.c src/qnx/vigil_ipc.h $(CORE_SRC) $(CORE_HDR) | $(QNX_OUT)
	$(QNX_CC) $(QNX_CFLAGS) -o $@ tools/vigil_ctl.c $(CORE_SRC) -lm
$(QNX_OUT)/vigil-load: tools/vigil_load.c | $(QNX_OUT)
	$(QNX_CC) $(QNX_CFLAGS) -o $@ tools/vigil_load.c
$(QNX_OUT)/test_core: tests/unit/test_core.c $(CORE_SRC) $(CORE_HDR) | $(QNX_OUT)
	$(QNX_CC) $(QNX_CFLAGS) -o $@ tests/unit/test_core.c $(CORE_SRC) -lm
$(QNX_OUT)/vg-replay: tools/vg_replay.c $(CORE_SRC) $(CORE_HDR) | $(QNX_OUT)
	$(QNX_CC) $(QNX_CFLAGS) -o $@ tools/vg_replay.c $(CORE_SRC) -lm

STUB_FLAGS := -std=gnu11 -fsyntax-only -Wall -Wextra -Wshadow -Wformat=2 -Wno-format-nonliteral -Werror -D_GNU_SOURCE -Itests/qnx-stubs
check-qnx-syntax: src/qnx/web_index.h
	for f in $(QNX_SRC) tools/vigil_ctl.c tools/vigil_watch.c tools/vigil_load.c; do $(HOST_CC) $(STUB_FLAGS) $$f || exit 1; done
	@echo "QNX sources type-check against the stand-in headers (this is NOT a qcc build)"

clean:
	$(RM) -r out
