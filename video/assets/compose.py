#!/usr/bin/env python3
"""把一条 take 的某个时间窗合成成 1080p 成片片段：背景 + 录屏 + 外壳 + 按键动效 + 字幕。

用法:
  python3 video/assets/compose.py --take video/raw/t13-en.mp4 --first 1794042508.53 \
      --events video/raw/t13-en.events --in 61.0 --out 69.5 --speed 1.6 \
      --caption "LB 上屏 · ZL 空格 · A 回车" --out video/work/t13-a.mp4

--in/--out 用的是「视频时间」（t_video = t_event − first）。
"""
import argparse
import os
import subprocess
import sys
from functools import lru_cache

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 1920, 1080
SCREEN = (360, 190, 1200, 675)
FPS = 60
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MONO = "/home/brejce/.local/share/fonts/MapleMono-NF-CN-Regular.ttf"
MONO_B = "/home/brejce/.local/share/fonts/MapleMono-NF-CN-Bold.ttf"
CJK = "/usr/share/fonts/noto-cjk/NotoSansCJK-Medium.ttc"

BUTTONS = {
    "A": (1664, 350, 60, 60, "c"), "B": (1614, 400, 60, 60, "c"),
    "X": (1614, 300, 60, 60, "c"), "Y": (1564, 350, 60, 60, "c"),
    "L": (167, 141, 130, 46, "r"), "ZL": (168, 115, 120, 42, "r"),
    "R": (1753, 141, 130, 46, "r"), "ZR": (1754, 115, 120, 42, "r"),
    "UP": (206, 546, 44, 44, "r"), "DOWN": (206, 634, 44, 44, "r"),
    "LEFT": (162, 590, 44, 44, "r"), "RIGHT": (250, 590, 44, 44, "r"),
    "L3": (206, 350, 100, 100, "c"), "PLUS": (1615, 233, 30, 30, "r"),
    "MINUS": (299, 226, 30, 30, "r"),
}
# evdev code -> 外壳键名（与 src/input/evdev_gamepad.cpp 一致）
CODE = {
    304: "B", 305: "A", 307: "X", 308: "Y",      # BTN_SOUTH/EAST/NORTH/WEST
    310: "L", 311: "R", 312: "ZL", 313: "ZR",     # BTN_TL/TR/TL2/TR2
    317: "L3", 318: "R3",
    544: "UP", 545: "DOWN", 546: "LEFT", 547: "RIGHT",  # BTN_DPAD_*
    314: "MINUS", 315: "PLUS",
}
GLOW_MS = 190


def bg():
    top, bot = (30, 37, 52), (6, 8, 12)
    img = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(img)
    for y in range(H):
        t = y / H
        d.line([(0, y), (W, y)], fill=tuple(int(top[i] * (1 - t) + bot[i] * t) for i in range(3)))
    glow = Image.new("L", (W, H), 0)
    ImageDraw.Draw(glow).ellipse((240, 20, 1680, 920), fill=80)
    glow = glow.filter(ImageFilter.GaussianBlur(200))
    out = Image.composite(Image.new("RGB", (W, H), (74, 100, 138)), img, glow)
    back = Image.new("L", (W, H), 0)
    ImageDraw.Draw(back).ellipse((120, 90, 1800, 1010), fill=46)
    back = back.filter(ImageFilter.GaussianBlur(120))
    out = Image.composite(Image.new("RGB", (W, H), (128, 152, 196)), out, back)
    return out.convert("RGBA")


@lru_cache(maxsize=1)
def screen_bleed():
    """屏幕往机身上漏的一圈光（只画环，不盖住画面）。"""
    m = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle((SCREEN[0] - 26, SCREEN[1] - 26,
                         SCREEN[0] + SCREEN[2] + 26, SCREEN[1] + SCREEN[3] + 26),
                        radius=36, fill=255)
    d.rounded_rectangle((SCREEN[0] + 3, SCREEN[1] + 3,
                         SCREEN[0] + SCREEN[2] - 3, SCREEN[1] + SCREEN[3] - 3),
                        radius=8, fill=0)
    m = m.filter(ImageFilter.GaussianBlur(14))
    lay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    lay.paste((150, 192, 235, 78), (0, 0), m)
    return lay


@lru_cache(maxsize=64)
def glow_sprite(name, fade):
    cx, cy, w, h, shape = BUTTONS[name]
    size = int(max(w, h) * 2.6)
    sp = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    gm = Image.radial_gradient("L").resize((size, size), Image.LANCZOS)
    gm = gm.point(lambda v: int(255 * (1 - v / 255.0) ** 2.4 * 0.85))
    sp.paste((255, 255, 255, fade), (0, 0), gm)
    sp = sp.filter(ImageFilter.GaussianBlur(size / 10))
    d = ImageDraw.Draw(sp)
    pad = 4
    if shape == "c":
        d.ellipse((size / 2 - w / 2 - pad, size / 2 - h / 2 - pad,
                   size / 2 + w / 2 + pad, size / 2 + h / 2 + pad),
                  outline=(255, 255, 255, fade), width=3)
        d.ellipse((size / 2 - w / 2 + 5, size / 2 - h / 2 + 5,
                   size / 2 + w / 2 - 5, size / 2 + h / 2 - 5), fill=(10, 12, 16, int(fade * 0.55)))
    else:
        r = min(w, h) / 2.2
        d.rounded_rectangle((size / 2 - w / 2 - pad, size / 2 - h / 2 - pad,
                             size / 2 + w / 2 + pad, size / 2 + h / 2 + pad),
                            radius=r + pad, outline=(255, 255, 255, fade), width=3)
        d.rounded_rectangle((size / 2 - w / 2 + 4, size / 2 - h / 2 + 4,
                             size / 2 + w / 2 - 4, size / 2 + h / 2 - 4),
                            radius=r, fill=(10, 12, 16, int(fade * 0.5)))
    return sp, (cx - size // 2, cy - size // 2)


def parse_presses(events, first):
    """返回 [(t_video, button)]：每个按下瞬间（value==1）。"""
    out = []
    for ln in open(events):
        if ln.startswith("#"):
            continue
        p = ln.split()
        ts, typ, code, val = float(p[0]), int(p[-3]), int(p[-2]), int(p[-1])
        if typ != 1 or val != 1:
            continue
        name = CODE.get(code)
        if name:
            out.append((ts - first, name))
    return out


def caption_layer(text, alpha=255):
    if not text:
        return None
    d = ImageDraw.Draw(Image.new("RGBA", (10, 10)))
    f = ImageFont.truetype(CJK, 42, index=2)
    bb = d.textbbox((0, 0), text, font=f)
    cw = bb[2] - bb[0]
    bar_w = min(W - 160, cw + 120)
    lay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    bar = Image.new("RGBA", (bar_w, 92), (0, 0, 0, 0))
    ImageDraw.Draw(bar).rounded_rectangle((0, 0, bar_w - 1, 91), radius=12,
                                          fill=(4, 6, 9, int(175 * alpha / 255)))
    lay.alpha_composite(bar, ((W - bar_w) // 2, H - 112))
    d = ImageDraw.Draw(lay)
    d.text(((W - cw) / 2 - bb[0], H - 112 + 46 - (bb[3] - bb[1]) / 2 - bb[1]), text,
           font=f, fill=(235, 238, 245, alpha))
    return lay


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--take", required=True)
    ap.add_argument("--events", required=True)
    ap.add_argument("--first", type=float, required=True)
    ap.add_argument("--in", dest="tin", type=float, required=True)
    ap.add_argument("--out", dest="tout", type=float, required=True)
    ap.add_argument("--speed", type=float, default=1.0)
    ap.add_argument("--caption", default="")
    ap.add_argument("--outfile", required=True)
    ap.add_argument("--crf", type=int, default=18)
    a = ap.parse_args()

    shell = Image.open(os.path.join(ROOT, "video/assets/shell.png")).convert("RGBA")
    base = bg()
    cap = caption_layer(a.caption)
    presses = [(t, n) for t, n in parse_presses(a.events, a.first)
               if a.tin - 0.5 <= t <= a.tout + 0.5]

    dur = a.tout - a.tin
    dec = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-ss", f"{a.tin}", "-t", f"{dur}", "-i", a.take,
         "-vf", f"setpts=PTS/{a.speed},fps={FPS},scale={SCREEN[2]}:{SCREEN[3]}:flags=lanczos",
         "-pix_fmt", "rgb24", "-f", "rawvideo", "-"],
        stdout=subprocess.PIPE)
    n_out = int(round(dur * FPS))
    enc = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
         "-s", f"{W}x{H}", "-r", str(FPS), "-i", "-",
         "-c:v", "libx264", "-crf", str(a.crf), "-preset", "medium",
         "-pix_fmt", "yuv420p", a.outfile],
        stdin=subprocess.PIPE)

    fsz = SCREEN[2] * SCREEN[3] * 3
    for i in range(n_out):
        buf = dec.stdout.read(fsz)
        if len(buf) < fsz:
            break
        frame = base.copy()
        frame.paste(Image.frombytes("RGB", (SCREEN[2], SCREEN[3]), buf), (SCREEN[0], SCREEN[1]))
        frame.alpha_composite(shell)
        frame.alpha_composite(screen_bleed())
        t_out = a.tin + i * a.speed / FPS
        for (pt, name) in presses:
            dt = (t_out - pt) / a.speed
            if -0.02 <= dt <= GLOW_MS / 1000:
                fade = 255
                if dt > 0.12:
                    fade = int(255 * max(0.0, 1 - (dt - 0.12) / 0.07))
                sp, pos = glow_sprite(name, fade)
                frame.alpha_composite(sp, pos)
        if cap is not None:
            frame.alpha_composite(cap)
        enc.stdin.write(frame.convert("RGB").tobytes())
        if i % 120 == 0:
            print(f"\r{i}/{n_out}", end="", file=sys.stderr)
    enc.stdin.close()
    dec.wait()
    enc.wait()
    print(f"\n{a.outfile}", file=sys.stderr)


if __name__ == "__main__":
    main()
