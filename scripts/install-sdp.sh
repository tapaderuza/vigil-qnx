#!/usr/bin/env bash
# Install QNX SDP 8.0 and the QEMU quick-start image with the QNX Software Center CLI.
# Run INSIDE Ubuntu (WSL) as root, in your own terminal: it asks for your myQNX password
# (hidden, never stored, never passed through any assistant).
#
#   wsl -d Ubuntu-24.04 -u root
#   bash /mnt/d/vigil/scripts/install-sdp.sh you@example.com
set -euo pipefail
USER_EMAIL="${1:?usage: install-sdp.sh <myqnx-email>}"
QSC="${QSC:-/opt/qsc-installer/qnxsoftwarecenter/qnxsoftwarecenter_clt}"
DEST="${DEST:-$HOME/qnx800}"

read -r -s -p "myQNX password for $USER_EMAIL: " PW; echo
trap 'unset PW' EXIT

q() { "$QSC" -myqnx.user "$USER_EMAIL" -myqnx.password "$PW" "$@"; }

echo "== sync licence keys from your myQNX account"
q -syncLicenseKeys
"$QSC" -listLicenseKeys


echo "== mirror the remote package catalogue into the local repository"
q -mirrorBaseline qnx800

echo "== activate the licence key on this machine"
if [ -n "${2:-}" ]; then q -addLicenseKey "$2"; fi   # optional 2nd argument: your licence key
q -activateAll
"$QSC" -listLicenseKeys

echo "== install the SDP 8.0 baseline into $DEST"
q -installBaseline com.qnx.qnx800 -destination "$DEST"

echo "== install the QEMU quick-start image"
q -destination "$DEST" -installPackage com.qnx.qnx800.quickstart.qemu

echo "done. Next: source $DEST/qnxsdp-env.sh && make qnx"
