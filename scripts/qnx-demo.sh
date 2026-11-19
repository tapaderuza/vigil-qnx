#!/usr/bin/env bash
# Build Lookout for QNX 8.0, boot QNX under QEMU (user-mode networking, WSL2 friendly), deploy,
# run the unit tests ON QNX and stream a scenario at the daemon.
#
#   scripts/qnx-demo.sh [scenario]        default scenario: ward
#
# Needs QNX SDP 8.0 installed (scripts/install-sdp.sh) and an image built once with:
#   source ~/qnx800/qnxsdp-env.sh; mkdir -p ~/qnx-vm; cd ~/qnx-vm
#   mkqnximage --type=qemu --arch=x86_64 --graphics=no --ssh-ident=none --ram=1G --cpu=2 --noprompt --build
# The VM keeps the default mkqnximage account (root/root): it is a local throw-away VM reachable
# only on 127.0.0.1.
set -euo pipefail
SCENARIO="${1:-ward}"
QNX_SDP="${QNX_SDP:-$HOME/qnx800}"
cd "$(dirname "$0")/.."
source "$QNX_SDP/qnxsdp-env.sh" > /dev/null

echo "== build for QNX x86_64"
make qnx QNX_ARCH=x86_64
OUT=out/qnx-x86_64

SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
vm() { sshpass -p root ssh $SSH_OPTS -p 2222 root@127.0.0.1 "$@"; }

if ! (echo > /dev/tcp/127.0.0.1/2222) 2>/dev/null; then
    echo "== booting QNX in QEMU"
    nohup scripts/run-qemu.sh > "$HOME/qnx-serial.log"  &
    for _ in $(seq 1 60); do (echo > /dev/tcp/127.0.0.1/2222) 2>/dev/null && break; sleep 2; done
    sleep 5
fi

echo "== deploy and run the unit tests ON QNX"
sshpass -p root scp $SSH_OPTS -P 2222 "$OUT"/{vigild,vigil-ctl,vigil-watch,test_core,vg-replay} root@127.0.0.1:/tmp/
vm "chmod +x /tmp/vigild /tmp/vigil-* /tmp/test_core /tmp/vg-*; /tmp/test_core | tail -3"

echo "== start the daemon"
vm "(/tmp/vigild -f > /tmp/vigild.log  &); sleep 3; ls /dev/vigil"
vm "/tmp/vigil-ctl reset > /dev/null; (/tmp/vigil-watch -n 6 -t 120 > /tmp/watch.log  &); sleep 1"

echo "== stream the $SCENARIO scenario at 20x"
python3 sim/vigil_sim.py send "$SCENARIO" --host 127.0.0.1 --port 10120 --speed 20
sleep 2
vm "cat /tmp/watch.log; echo; cat /dev/vigil/events; grep -E deadline /dev/vigil/stats"
echo "dashboard: http://127.0.0.1:8080/"
