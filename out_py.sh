#!/usr/bin/env bash
cd /mnt/d/vigil && /opt/lookout-venv/bin/python -m pytest -q sim 2>&1 | tail -30
