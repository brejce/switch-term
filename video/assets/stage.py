#!/usr/bin/env python3
"""把实机录屏透视贴进真实拍的 Switch 照片里（照片版合成）。

stage 图 = 你拍的 Switch（屏幕是黑的），用 QUAD 标出屏幕四角，把 1280x720
的 fb0 录屏透视投进去；再按 BUTTONS 坐标在真实按键上打高亮。

用法：
  python3 stage.py --take video/raw/t13-en.mp4 --events ... --first ... \
      --in 67.6 --out 68.6 --speed 1.0 --caption "..." --outfile video/work/x.mp4
"""
import argparse
import math
import subprocess
import sys
from functools import lru_cache
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent                       # video/
W, H = 1920, 1080

STAGE = HERE / "stage.jpg"               # 实拍照片
QUAD = [(612, 262), (1329, 264), (1308, 730), (589, 731)]   # 屏幕四角（1920x1080 坐标）

# 真机按键中心（1920x1080 坐标）——用 detect_buttons.py 标定
BUTTONS = {
    "A": (1546, 350, 34), "B": (1498, 386, 34), "X": (1498, 292, 34), "Y": (1450, 350, 34),
    "PLUS": (1448, 262, 26), "R": (1520, 232, 40), "ZR": (1540, 200, 44),
    "L3": (438, 336, 58), "L": (300, 250, 40), "ZL": (280, 218, 44),
    "UP": (404, 470, 26), "DOWN": (404, 512, 26), "LEFT": (386, 490, 26), "RIGHT": (422, 490, 26),
}
CODE = {305: "A", 304: "B", 307: "X", 308: "Y", 310: "LB", 311: "RB", 312: "ZL", 313: "ZR",
        317: "L3", 544: "UP", 545: "DOWN", 546: "LEFT", 547: "RIGHT", 314: "MINUS", 315: "PLUS"}
ALIAS = {"LB": "L", "RB": "R"}
GLOW_MS = 190


def _solve(mat, vec):
    n = len(vec)
    a = [row[:] + [vec[i]] for i, row in enumerate(mat)]
    for i in range(n):
        p = max(range(i, n), key=lambda r: abs(a[r][i]))
        a[i], a[p] = a[p], a[i]
        if abs(a[i][i]) < 1e-12:
            raise ValueError("singular")
        for r in range(n):
            if r != i:
                f = a[r][i] / a[i][i]
                for c in range(i, n + 1):
                    a[r][c] -= f * a[i][c]
    return [a[i][n] / a[i][i] for i in range(n)]


def perspective_coeffs(dst_quad, src_rect=(0, 0, 1280, 720)):
    """输出图上的四个点 → 源图矩形四角，返回 PIL PERSPECTIVE 的 8 个系数。"""
    sw, sh = src_rect[2], src_rect[3]
    src = [(0, 0), (sw, 0), (sw, sh), (0, sh)]
    mat, vec = [], []
    for (X, Y), (x, y) in zip(dst_quad, src):
        mat.append([X, Y, 1, 0, 0, 0, -x * X, -x * Y]); vec.append(x)
        mat.append([0, 0, 0, X, Y, 1, -y * X, -y * Y]); vec.append(y)
    return _solve(mat, vec)


@lru_cache(maxsize=1)
def stage():
    im = Image.open(STAGE).convert("RGB")
    if im.size != (W, H):
        im = im.resize((W, H), Image.LANCZOS)
    return im


@lru_cache(maxsize=1)
def quad_mask():
    m = Image.new("L", (W, H), 0)
    ImageDraw.Draw(m).polygon(QUAD, fill=255)
    return m.filter(ImageFilter.GaussianBlur(1.2))


@lru_cache(maxsize=1)
def screen_bloom():
    """屏幕往机身上漏的光。"""
    m = Image.new("L", (W, H), 0)
    ImageDraw.Draw(m).polygon(QUAD, outline=255, width=10)
    m = m.filter(ImageFilter.GaussianBlur(26))
    lay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    lay.paste((150, 195, 240, 90), (0, 0), m)
    return lay


@lru_cache(maxsize=1)
def vignette():
    v = Image.new("L", (W, H), 0)
    ImageDraw.Draw(v).ellipse((-360, -300, W + 360, H + 300), fill=110)
    v = v.filter(ImageFilter.GaussianBlur(260))
    return Image.composite(Image.new("RGBA", (W, H), (0, 0, 0, 0)),
                           Image.new("RGBA", (W, H), (0, 0, 0, 255)), v)


@lru_cache(maxsize=128)
def glow_sprite(key, fade):
    x, y, r = BUTTONS[key]
    R = int(r * 2.6)
    lay = Image.new("RGBA", (R * 2, R * 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(lay)
    for i in range(12):
        a = i / 11
        rr = R - a * (R - r * 0.7)
        d.ellipse((R - rr, R - rr, R + rr, R + rr), fill=(255, 255, 255, int(6 + 10 * a)))
    d.ellipse((R - r - 3, R - r - 3, R + r + 3, R + r + 3), outline=(255, 255, 255, 220), width=3)
    d.ellipse((R - r, R - r, R + r, R + r), fill=(255, 255, 255, 40))
    lay = lay.filter(ImageFilter.GaussianBlur(9))
    a = int(255 * fade)
    lay.putalpha(lay.getchannel("A").point(lambda v: v * a // 255))
    return lay, (x - R, y - R)


def events(path, first):
    out = []
    for ln in Path(path).read_text().splitlines():
        if ln.startswith("#"):
            continue
        p = ln.split()
        out.append((float(p[0]) - first, int(p[-3]), int(p[-2]), int(p[-1])))
    return out


def decode(take, t0, t1, speed):
    r = subprocess.run(["ffmpeg", "-v", "error", "-ss", f"{t0}", "-t", f"{t1 - t0}",
                        "-i", str(take), "-vf",
                        f"setpts=PTS/{speed},fps=60", "-pix_fmt", "rgb24",
                        "-f", "rawvideo", "-"], capture_output=True)
    if r.returncode:
        sys.exit(r.stderr.decode()[-1500:])
    return r.stdout


def draw_caption(frame, text):
    if not text:
        return frame
    f = ImageFont.truetype("/usr/share/fonts/noto-cjk/NotoSansCJK-Medium.ttc", 44, index=2)
    d = ImageDraw.Draw(frame)
    tw = d.textlength(text, font=f)
    bw, bh = int(tw) + 120, 92
    x, y = (W - bw) // 2, H - 128
    bar = Image.new("RGBA", (bw, bh), (0, 0, 0, 0))
    ImageDraw.Draw(bar).rounded_rectangle((0, 0, bw - 1, bh - 1), radius=14, fill=(10, 12, 16, 180))
    frame.alpha_composite(bar, (x, y))
    d.text((x + (bw - tw) / 2, y + 20), text, font=f, fill=(240, 244, 250))
    return frame


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--take", required=True)
    ap.add_argument("--events")
    ap.add_argument("--first", type=float, default=0.0)
    ap.add_argument("--in", dest="t0", type=float, required=True)
    ap.add_argument("--out", dest="t1", type=float, required=True)
    ap.add_argument("--speed", type=float, default=1.0)
    ap.add_argument("--caption", default="")
    ap.add_argument("--crf", default="18")
    ap.add_argument("--outfile", required=True)
    a = ap.parse_args()

    base = stage().copy()
    coeffs = perspective_coeffs(QUAD)
    mask = quad_mask()
    ev = events(a.events, a.first) if a.events else []
    presses = [(t, CODE.get(c)) for t, ty, c, v in ev if ty == 1 and v == 1]
    presses = [(t, ALIAS.get(k, k)) for t, k in presses if k and ALIAS.get(k, k) in BUTTONS]

    dur = a.t1 - a.t0
    raw = decode(a.take, a.t0, a.t1, a.speed)
    fsz = W * H * 3
    n = len(raw) // fsz

    enc = subprocess.Popen(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
                            "-s", f"{W}x{H}", "-r", "60", "-i", "-", "-c:v", "libx264",
                            "-crf", a.crf, "-preset", "medium", "-pix_fmt", "yuv420p",
                            "-r", "60", str(a.outfile)], stdin=subprocess.PIPE)

    for i in range(n):
        t = i / 60.0
        src = Image.frombytes("RGB", (1280, 720), raw[i * fsz:(i + 1) * fsz])
        warped = src.transform((W, H), Image.PERSPECTIVE, coeffs, Image.BICUBIC)
        frame = base.copy()
        frame.paste(warped, (0, 0), mask)
        frame.alpha_composite(screen_bloom())
        for pt, key in presses:
            dt = abs(t - pt / a.speed)
            if dt < GLOW_MS / 1000 * 2:
                fade = max(0.0, 1 - dt / (GLOW_MS / 1000 * 2))
                spr, pos = glow_sprite(key, round(fade, 2))
                frame.alpha_composite(spr, pos)
        frame.alpha_composite(vignette())
        frame = draw_caption(frame, a.caption)
        enc.stdin.write(frame.convert("RGB").tobytes())
        if i % 60 == 0:
            print(f"  {i}/{n}", file=sys.stderr, end="\r")
    enc.stdin.close()
    enc.wait()
    print(f"\n{a.outfile}  {n} 帧 / {dur:.2f}s（{dur / a.speed:.2f}s 成片）", file=sys.stderr)


if __name__ == "__main__":
    main()
