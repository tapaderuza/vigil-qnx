#!/usr/bin/env bash
# Boot the mkqnximage-built QNX 8.0 image under QEMU with user-mode networking, so it works in WSL2
# without a bridge. Host ports (all bound to 127.0.0.1 only): ssh 2222, NMEA/AIS UDP 10110, dashboard 8080.
#   scripts/run-qemu.sh [image-dir]      default image dir: ~/qnx-vm  (created by mkqnximage --type=qemu --build)
set -euo pipefail
DIR="${1:-$HOME/qnx-vm}"
ACCEL=""
[ -w /dev/kvm ] && ACCEL="--enable-kvm --cpu host"
exec qemu-system-x86_64 $ACCEL -smp 2 -m 1G \
  -drive file="$DIR/output/disk-qemu.vmdk",if=ide,id=drv0 \
  -kernel "$DIR/output/ifs.bin" \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22,hostfwd=udp:127.0.0.1:10110-:10110,hostfwd=tcp:127.0.0.1:8080-:8080 \
  -device virtio-net-pci,netdev=net0 \
  -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
  -nographic -serial mon:stdio -pidfile "$DIR/qemu.pid"
