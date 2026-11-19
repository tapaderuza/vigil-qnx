#!/usr/bin/env bash
# One-time host setup inside Ubuntu 24.04 (WSL2 or native): compilers, QEMU and Python test deps.
# Run as root:  sudo bash scripts/setup-wsl.sh
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential make python3 python3-venv python3-pip \
    qemu-system-x86 qemu-utils socat netcat-openbsd iproute2 jq curl
python3 -m venv /opt/vigil-venv
/opt/vigil-venv/bin/pip install -q -r "$(dirname "$0")/../requirements-dev.txt"
echo "ok: use /opt/vigil-venv/bin/python -m pytest -q sim"
