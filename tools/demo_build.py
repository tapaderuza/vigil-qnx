#!/usr/bin/env python3
"""Build the demo MP4 and the four gallery images from real captures (docs/DEMO.md).

    python tools/demo_build.py --dir out/demo

--dir holds browser screenshots (shots/*.png), text captured from the QNX guest (*.txt) and scenes.py, which lays the
scenes out with the helpers below. Terminal text is rendered as an image; nothing in it is retyped. Every scene carries
a HOST / REPLAY / QNX chip. No audio, no latency figures. Needs Pillow and ffmpeg on PATH.

SPDX-License-Identifier: MIT
"""
import argparse
import importlib.util
import subprocess
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 1920, 1080
BG, PAPER, DIM, SEA, AMBER, RED, CALM = (11, 14, 17), (222, 218, 205), (140, 146, 150), (143, 190, 210), (232, 176, 64), (236, 96, 82), (121, 175, 140)
FONTS = Path("C:/Windows/Fonts")
MONO, SANS, SANSB = FONTS / "consola.ttf", FONTS / "segoeui.ttf", FONTS / "segoeuib.ttf"
CHIP = {"QNX": (60, 120, 200), "HOST": (130, 100, 40), "REPLAY": (100, 70, 140)}


def font(p, s):
    return ImageFont.truetype(str(p), s)


def canvas(size=(W, H)):
    im = Image.new("RGB", size, BG)
    return im, ImageDraw.Draw(im)


def chip(d, label, x=60, y=30):
    if not label:
        return
    f = font(SANSB, 34)
    w = d.textlength(label, font=f) + 44
    d.rounded_rectangle([x, y, x + w, y + 56], radius=12, fill=CHIP[label.split()[0]])
    d.text((x + 22, y + 6), label, font=f, fill=(255, 255, 255))


def cap_height(text, big):
    return len(text.split("\n")) * (big + 12) + 60


def caption(d, text, size=(W, H), col=PAPER, big=44):
    w, h = size
    lines = text.split("\n")
    top = h - 36 - len(lines) * (big + 12)
    d.rectangle([0, top - 24, w, h], fill=(8, 10, 12))
    for i, ln in enumerate(lines):
        d.text((60, top + i * (big + 12)), ln, font=font(SANSB if i == 0 else SANS, big), fill=col)


def fit(img, box):
    k = min(box[0] / img.width, box[1] / img.height)
    return img.resize((int(img.width * k), int(img.height * k)), Image.LANCZOS)


def shot_slide(path, cap, label, crop=None, size=(W, H), big=44):
    im, d = canvas(size)
    s = Image.open(path).convert("RGB")
    if crop:
        s = s.crop(crop)
    s = fit(s, (size[0] - 120, size[1] - 110 - cap_height(cap, big)))
    im.paste(s, ((size[0] - s.width) // 2, 100))
    chip(d, label)
    caption(d, cap, size, big=big)
    return im


def term_slide(text, cap, label, size=(W, H), fs=34, title="QNX 8.0 in QEMU  root@qnx", big=44, hl=()):
    im, d = canvas(size)
    chip(d, label)
    lines = text.rstrip("\n").split("\n")
    room = size[1] - 110 - cap_height(cap, big) - 40
    while fs > 14 and len(lines) * (fs + 8) + 60 > room:
        fs -= 1
    f = font(MONO, fs)
    d.rounded_rectangle([50, 100, size[0] - 50, 100 + room], radius=14, fill=(6, 8, 10), outline=(30, 36, 42), width=2)
    d.text((80, 112), title, font=font(MONO, 22), fill=DIM)
    y = 156
    for ln in lines:
        col = PAPER
        if any(h in ln for h in hl):
            col = CALM
        elif "HIGH" in ln:
            col = RED
        elif "MEDIUM" in ln:
            col = AMBER
        d.text((80, y), ln, font=f, fill=col)
        y += fs + 8
    caption(d, cap, size, big=big)
    return im


def card(lines, label="", size=(W, H)):
    im, d = canvas(size)
    chip(d, label)
    y = 200
    for ln, s, col in lines:
        d.text((120, y), ln, font=font(SANSB if s >= 60 else SANS, s), fill=col)
        y += int(s * 1.6)
    return im


def clip(png, secs, out):
    frames = int(secs * 30)
    vf = f"zoompan=z='min(zoom+0.0004,1.06)':d={frames}:s={W}x{H}:fps=30,format=yuv420p"
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-loop", "1", "-i", str(png), "-vf", vf, "-frames:v", str(frames),
                    "-r", "30", "-c:v", "libx264", "-crf", "18", str(out)], check=True)


def assemble(slides, out_mp4, tmp):
    tmp.mkdir(parents=True, exist_ok=True)
    lst = []
    for i, (im, secs) in enumerate(slides):
        p = tmp / f"s{i:02d}.png"
        im.save(p)
        c = tmp / f"s{i:02d}.mp4"
        clip(p, secs, c)
        lst.append(f"file '{c.as_posix()}'")
    (tmp / "list.txt").write_text("\n".join(lst) + "\n")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", str(tmp / "list.txt"),
                    "-c", "copy", str(out_mp4)], check=True)
    return sum(s for _, s in slides)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="out/demo")
    a = ap.parse_args()
    spec = importlib.util.spec_from_file_location("scenes", Path(a.dir) / "scenes.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    print(f"video {mod.build(Path(a.dir), globals()):.1f} s")


if __name__ == "__main__":
    main()
