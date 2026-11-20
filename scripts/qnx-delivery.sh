#!/usr/bin/env bash
# Delivery trial on QNX 8.0: how long does an alarm transition that the engine has already decided take to
# reach a consumer (vigil-watch) on the same guest? Method and exact measurement points: docs/DELIVERY.md.
#
#   scripts/qnx-delivery.sh                 builds, deploys, runs the idle trial and the loaded trial, writes the report
#   UDP_PORT=10110 scripts/qnx-delivery.sh  if your QEMU forwards another UDP port to the guest
#
# Needs what scripts/qnx-demo.sh needs: the QNX SDP 8.0 (scripts/install-sdp.sh), sshpass, and the guest image
# (scripts/run-qemu.sh boots it; this script boots it if nothing answers on the SSH port).
# Output: out/delivery/<timestamp>/{expected.txt,idle.tsv,loaded.tsv,idle.txt,loaded.txt,report.md,report.json}
# The report describes the runs it measured; it is not a promise about any other run or machine.
set -euo pipefail

QNX_SDP="${QNX_SDP:-$HOME/qnx800}"
UDP_PORT="${UDP_PORT:-10120}"
SSH_PORT="${SSH_PORT:-2222}"
LOAD_THREADS="${LOAD_THREADS:-2}"
LOAD_PRIO="${LOAD_PRIO:-15}"
LOAD_BURST_MS="${LOAD_BURST_MS:-8}"
LOAD_GAP_MS="${LOAD_GAP_MS:-2}"
cd "$(dirname "$0")/.."
# shellcheck disable=SC1091
source "$QNX_SDP/qnxsdp-env.sh" > /dev/null

SCENARIO="${SCENARIO:-delivery}"      # delivery (default, 15 transitions) or burst8 (96 transitions, 48 in one cycle)
SCEN_SECONDS="${SCEN_SECONDS:-75}"
EXPECT_TAIL="${EXPECT_TAIL:-0}"        # seconds the host replay keeps running after the last frame (burst8close: 15, to include the expiries)
WATCH_IDLE="${WATCH_IDLE:-25}"       # seconds without an event before vigil-watch exits (burst8 needs 40)
STAMP="$(date +%Y%m%d-%H%M%S)-$SCENARIO"
OUT="out/delivery/$STAMP"
mkdir -p "$OUT"
BIN=out/qnx-x86_64

SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=20"
vm() { sshpass -p root ssh $SSH_OPTS -p "$SSH_PORT" root@127.0.0.1 "$@"; }
pull() { sshpass -p root scp $SSH_OPTS -P "$SSH_PORT" "root@127.0.0.1:$1" "$2"; }

echo "== build (host tools for the expected transitions, QNX binaries for the target)"
make host > /dev/null
make qnx QNX_ARCH=x86_64 > /dev/null

echo "== expected transitions: the same frames through the portable core on the host"
python3 sim/vigil_sim.py generate "$SCENARIO" | out/host/vg-replay -t "${EXPECT_TAIL:-0}" > "$OUT/expected.txt" 2> /dev/null
echo "   $(grep -c '^T+' "$OUT/expected.txt") transitions expected"

if ! (echo > /dev/tcp/127.0.0.1/"$SSH_PORT") 2>/dev/null; then
    echo "== booting the guest"
    nohup scripts/run-qemu.sh > "$HOME/qnx-serial.log" 2>&1 &
    for _ in $(seq 1 60); do (echo > /dev/tcp/127.0.0.1/"$SSH_PORT") 2>/dev/null && break; sleep 2; done
    sleep 8
fi

echo "== deploy"
# a daemon left running by an earlier run keeps its binary busy, and scp cannot overwrite it
vm "slay -f vigil-watch; slay -f vigil-load; slay -f vigild; sleep 1; rm -f /tmp/vigild /tmp/vigil-ctl /tmp/vigil-watch /tmp/vigil-load; true" > /dev/null 2>&1 || true
sshpass -p root scp $SSH_OPTS -P "$SSH_PORT" "$BIN"/{vigild,vigil-ctl,vigil-watch,vigil-load} root@127.0.0.1:/tmp/
vm "chmod +x /tmp/vigild /tmp/vigil-ctl /tmp/vigil-watch /tmp/vigil-load"

trial() {   # trial NAME WITH_LOAD
    local name="$1" with_load="$2"
    echo "== trial: $name"
    vm "slay -f vigil-watch; slay -f vigil-load; slay -f vigild; true" > /dev/null 2>&1 || true
    sleep 1
    vm "(/tmp/vigild -f -u $UDP_PORT > /tmp/vigild.log 2>&1 &); sleep 3; /tmp/vigil-ctl reset > /dev/null; true"
    if [ "$with_load" = "yes" ]; then
        vm "(/tmp/vigil-load -n $LOAD_THREADS -p $LOAD_PRIO -b $LOAD_BURST_MS -g $LOAD_GAP_MS -t 110 > /tmp/load.log 2>&1 &); sleep 1; cat /tmp/load.log"
    fi
    vm "(on -p10 /tmp/vigil-watch -m -t $WATCH_IDLE > /tmp/watch-$name.tsv 2> /tmp/watch-$name.err &); sleep 1"
    python3 sim/vigil_sim.py send "$SCENARIO" --host 127.0.0.1 --port "$UDP_PORT" --speed 1 > /dev/null
    for _ in $(seq 1 40); do
        vm "pidin ar" 2>/dev/null | grep -q "vigil-watch" || break
        sleep 1
    done
    vm "slay -f vigil-load; true" > /dev/null 2>&1 || true
    pull "/tmp/watch-$name.tsv" "$OUT/$name.tsv"
    vm "cat /dev/vigil/events" > "$OUT/$name.history.txt" 2> /dev/null || true
    {
        echo "trial:      $name"
        echo "date:       $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "commit:     $(git rev-parse --short HEAD)$(git diff --quiet || echo '+uncommitted')"
        echo "guest:      $(vm uname -a)"
        echo "qemu:       $(ps -o args= -C qemu-system-x86_64 | head -1 | cut -c1-200)"
        echo "scenario:   $SCENARIO, speed 1, UDP port $UDP_PORT, 2 virtual CPUs (QEMU -smp 2), user-mode networking"
        echo "priorities: engine 40, resmgr 25, ingest 20, vigil-watch 10 (on -p10)"
        if [ "$with_load" = "yes" ]; then
            echo "load:       vigil-load -n $LOAD_THREADS -p $LOAD_PRIO -b $LOAD_BURST_MS -g $LOAD_GAP_MS (SCHED_FIFO $LOAD_PRIO, outranks vigil-watch)"
        else
            echo "load:       none"
        fi
        echo "--- /dev/vigil/stats after the run"
        vm "cat /dev/vigil/stats"
    } > "$OUT/$name.txt"
    vm "slay -f vigild; true" > /dev/null 2>&1 || true
}

trial idle no
trial loaded yes

echo "== report"
python3 tools/delivery_report.py --duration "$SCEN_SECONDS" --expected "$OUT/expected.txt" \
    --history "idle=$OUT/idle.history.txt" --history "loaded=$OUT/loaded.history.txt" --run "idle=$OUT/idle.tsv" --run "loaded=$OUT/loaded.tsv" \
    --notes "idle=$OUT/idle.txt" --notes "loaded=$OUT/loaded.txt" --out "$OUT/report" || echo "(the report flags a difference: read it)"
echo "report: $OUT/report.md"
