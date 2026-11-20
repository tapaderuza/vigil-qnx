#!/usr/bin/env python3
"""Record the dashboard while vigild is cut and brought back (docs/DEMO.md), as one continuous video.

    python tools/record_disconnect.py --out out/demo/live --cut CMD --restore CMD

CMD are shell commands run on the host (for the QNX guest: ssh or wsl wrappers). Needs Playwright for Python and
Chrome. Writes <out>/disconnect.webm and <out>/markers.json (seconds since the video started).
SPDX-License-Identifier: MIT
"""
import argparse
import json
import subprocess
import time
from pathlib import Path

from playwright.sync_api import sync_playwright


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:8080/?clean=1")
    ap.add_argument("--out", default="out/demo/live")
    ap.add_argument("--cut", required=True)
    ap.add_argument("--restore", required=True)
    ap.add_argument("--live-s", type=float, default=8)
    ap.add_argument("--down-s", type=float, default=13)
    ap.add_argument("--after-s", type=float, default=11)
    ap.add_argument("--zoom", type=float, default=1.3)
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    marks = {}
    with sync_playwright() as p:
        br = p.chromium.launch(channel="chrome", headless=True)
        ctx = br.new_context(viewport={"width": 1280, "height": 720}, record_video_dir=str(out),
                             record_video_size={"width": 1280, "height": 720})
        t0 = time.monotonic()
        page = ctx.new_page()
        page.goto(a.url)
        page.evaluate(f"document.body.style.zoom={a.zoom}")
        page.wait_for_timeout(int(a.live_s * 1000))
        marks["cut"] = time.monotonic() - t0
        subprocess.run(a.cut, shell=True, check=False)
        page.wait_for_timeout(int(a.down_s * 1000))
        marks["restore"] = time.monotonic() - t0
        subprocess.run(a.restore, shell=True, check=False)
        page.wait_for_timeout(int(a.after_s * 1000))
        marks["end"] = time.monotonic() - t0
        vid = page.video
        ctx.close()
        vid.save_as(str(out / "disconnect.webm"))
        br.close()
    (out / "markers.json").write_text(json.dumps(marks, indent=1))
    print(marks)


if __name__ == "__main__":
    main()
