#!/usr/bin/env python3
"""照片版合成：把实机 fb0 录屏投进那张「完整 Switch 渲染图」的屏幕里。

- stage-src.png：用户给的整机渲染图（800x480，白底）
- 设备抠出来放到 1920x1080 深色底上，屏幕四角 = QUAD（画布坐标）
- 按键高亮按 BUTTONS（真实按键位置）打光
"""
import json
import subprocess
import sys
from functools import lru_cache
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
W, H = 1920, 1080

SRC = HERE / "stage-src.png"
DEV_BOX = (117, 119, 679, 359)        # 设备外接框（源图坐标）
SCR_BOX = (237, 148, 557, 328)        # 屏幕外接框（源图坐标）
OVERSCAN = 5                          # 屏幕四边多铺 5px：盖住渲染图玻璃的反光边
DEV_W = 1720                          # 设备在画布上的宽度
DEV_POS = (100, 196)                  # 设备左上角
S = DEV_W / (DEV_BOX[2] - DEV_BOX[0])

def _px(v, axis):
    return DEV_POS[axis] + (v - DEV_BOX[axis]) * S


_sx0, _sy0 = _px(SCR_BOX[0] - OVERSCAN, 0), _px(SCR_BOX[1] - OVERSCAN, 1)
_sx1, _sy1 = _px(SCR_BOX[2] + OVERSCAN, 0), _px(SCR_BOX[3] + OVERSCAN, 1)
SCREEN = (_sx0, _sy0, _sx1 - _sx0, _sy1 - _sy0)
QUAD = [(SCREEN[0], SCREEN[1]), (SCREEN[0] + SCREEN[2], SCREEN[1]),
        (SCREEN[0] + SCREEN[2], SCREEN[1] + SCREEN[3]), (SCREEN[0], SCREEN[1] + SCREEN[3])]

def _btn(x, y, r):
    return (_px(x, 0), _px(y, 1), max(8, r * S))

BUTTONS = {
    "A": _btn(657, 183, 12), "B": _btn(639, 199, 12),
    "X": _btn(640, 163, 12), "Y": _btn(620, 182, 12),
    "UP": _btn(161, 225, 11), "DOWN": _btn(161, 259, 11),
    "LEFT": _btn(140, 244, 11), "RIGHT": _btn(179, 244, 11),
    "L3": _btn(164, 180, 22), "R3": _btn(639, 244, 22),
    # 肩键在正面看不见 → 用设备上沿的柔光表示
    "L": (165 * S + DEV_POS[0], DEV_POS[1] + 2, 30), "R": (630 * S + DEV_POS[0], DEV_POS[1] + 2, 30),
    "ZL": (150 * S + DEV_POS[0], DEV_POS[1] - 4, 26), "ZR": (645 * S + DEV_POS[0], DEV_POS[1] - 4, 26),
}
CODE = {305: "A", 304: "B", 307: "X", 308: "Y", 310: "L", 311: "R", 312: "ZL", 313: "ZR",
        317: "L3", 544: "UP", 545: "DOWN", 546: "LEFT", 547: "RIGHT"}


def _solve(mat, vec):
    n = len(vec)
    a = [row[:] + [vec[i]] for i, row in enumerate(mat)]
    for i in range(n):
        p = max(range(i, n), key=lambda r: abs(a[r][i]))
        a[i], a[p] = a[p], a[i]
        for r in range(n):
            if r != i:
                f = a[r][i] / a[i][i]
                for c in range(i, n + 1):
                    a[r][c] -= f * a[i][c]
    return [a[i][n] / a[i][i] for i in range(n)]


def perspective_coeffs(dst_quad, sw=1280, sh=720):
    src = [(0, 0), (sw, 0), (sw, sh), (0, sh)]
    mat, vec = [], []
    for (X, Y), (x, y) in zip(dst_quad, src):
        mat.append([X, Y, 1, 0, 0, 0, -x * X, -x * Y]); vec.append(x)
        mat.append([0, 0, 0, X, Y, 1, -y * X, -y * Y]); vec.append(y)
    return _solve(mat, vec)


COEFFS = perspective_coeffs(QUAD)


def gradient_bg():
    img = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(img)
    for y in range(H):
        t = y / H
        d.line([(0, y), (W, y)], fill=(int(30 - 24 * t), int(37 - 29 * t), int(52 - 40 * t)))
    return img


@lru_cache(maxsize=1)
def device():
    """抠图 v3：先放大再抠（边缘按亚像素走，不出现锯齿/阶梯），颜色只从内部扩散，alpha 用白底混色公式反解。"""
    from collections import deque
    x0, y0, x1, y1 = DEV_BOX[0] - 2, DEV_BOX[1] - 2, DEV_BOX[2] + 2, DEV_BOX[3] + 2
    im = Image.open(SRC).convert("RGB").crop((x0, y0, x1, y1))
    nw = DEV_W
    nh = int(round(im.size[1] * S))
    im = im.resize((nw, nh), Image.LANCZOS)
    w, h = nw, nh
    ip = im.load()

    def pale(r, g, b):
        m, M = min(r, g, b), max(r, g, b)
        return m > 198 and M - m < 46          # 白底 + 它放大后的过渡带

    bg = bytearray(w * h)
    dq = deque()
    for x in range(w):
        for y in (0, h - 1):
            if pale(*ip[x, y]) and not bg[y * w + x]:
                bg[y * w + x] = 1; dq.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            if pale(*ip[x, y]) and not bg[y * w + x]:
                bg[y * w + x] = 1; dq.append((x, y))
    while dq:
        x, y = dq.popleft()
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and not bg[ny * w + nx] and pale(*ip[nx, ny]):
                bg[ny * w + nx] = 1; dq.append((nx, ny))

    obj = Image.new("L", (w, h), 255)
    op = obj.load()
    for y in range(h):
        for x in range(w):
            if bg[y * w + x]:
                op[x, y] = 0
    core = obj.filter(ImageFilter.MinFilter(9))       # 内缩 4px：取色只用内部色
    cp_ = core.load()

    dist = [-1] * (w * h)
    col = [(0, 0, 0)] * (w * h)
    dq = deque()
    for y in range(h):
        for x in range(w):
            if cp_[x, y] > 127:
                dist[y * w + x] = 0
                col[y * w + x] = ip[x, y]
                dq.append((x, y))
    MAXD = 8
    while dq:
        x, y = dq.popleft()
        d = dist[y * w + x]
        if d >= MAXD:
            continue
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and dist[ny * w + nx] < 0:
                dist[ny * w + nx] = d + 1
                col[ny * w + nx] = col[y * w + x]
                dq.append((nx, ny))

    a = Image.new("L", (w, h), 0)
    ap = a.load()
    fg = Image.new("RGB", (w, h), (0, 0, 0))
    fp = fg.load()
    for y in range(h):
        for x in range(w):
            d = dist[y * w + x]
            if d < 0:
                continue
            r, g, b = ip[x, y]
            if d == 0:
                ap[x, y] = 255
                fp[x, y] = (r, g, b)
                continue
            fr, fgg, fb = col[y * w + x]
            den = 255 - min(fr, fgg, fb)
            al = 0.0 if den <= 6 else (255 - min(r, g, b)) / den
            al = max(0.0, min(1.0, al))
            if al < 0.30:
                al = 0.0
            ap[x, y] = int(al * 255)
            fp[x, y] = (fr, fgg, fb)

    a = a.filter(ImageFilter.MinFilter(7)).filter(ImageFilter.GaussianBlur(0.9))
    # 只对不透明内部锐化，避免在边缘拉出黑边
    sharp = fg.filter(ImageFilter.UnsharpMask(radius=1.5, percent=60, threshold=2))
    inner = a.point(lambda v: 255 if v > 238 else 0)
    fg = Image.composite(sharp, fg, inner)
    out = fg.convert("RGBA")
    out.putalpha(a)
    return out


@lru_cache(maxsize=1)
def base():
    bg = gradient_bg().convert("RGBA")
    # 地面阴影
    sh = Image.new("L", (W, H), 0)
    ImageDraw.Draw(sh).ellipse((DEV_POS[0] + 60, DEV_POS[1] + device().size[1] - 70,
                                DEV_POS[0] + DEV_W - 60, DEV_POS[1] + device().size[1] + 46), fill=150)
    sh = sh.filter(ImageFilter.GaussianBlur(46))
    bg = Image.composite(Image.new("RGBA", (W, H), (0, 0, 0, 255)), bg,
                         sh.point(lambda v: v // 2))
    # 设备背后的一圈环境光
    halo = Image.new("L", (W, H), 0)
    ImageDraw.Draw(halo).ellipse((DEV_POS[0] - 40, DEV_POS[1] - 60,
                                  DEV_POS[0] + DEV_W + 40, DEV_POS[1] + device().size[1] + 40), fill=70)
    halo = halo.filter(ImageFilter.GaussianBlur(140))
    bg = Image.composite(Image.new("RGBA", (W, H), (120, 150, 200, 255)), bg, halo)
    dev = device()
    bg.alpha_composite(dev, (DEV_POS[0] - 2, DEV_POS[1] - 2))
    return bg


@lru_cache(maxsize=1)
def screen_glow():
    m = Image.new("L", (W, H), 0)
    ImageDraw.Draw(m).polygon(QUAD, outline=255, width=8)
    m = m.filter(ImageFilter.GaussianBlur(22))
    lay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    lay.paste((170, 210, 245, 80), (0, 0), m)
    return lay


@lru_cache(maxsize=1)
def screen_mask():
    m = Image.new("L", (W, H), 0)
    ImageDraw.Draw(m).polygon(QUAD, fill=255)
    return m.filter(ImageFilter.GaussianBlur(0.8))


@lru_cache(maxsize=1)
def vignette():
    v = Image.new("L", (W, H), 255)
    ImageDraw.Draw(v).ellipse((-380, -320, W + 380, H + 320), fill=0)
    v = v.filter(ImageFilter.GaussianBlur(240))
    return Image.composite(Image.new("RGBA", (W, H), (0, 0, 0, 255)),
                           Image.new("RGBA", (W, H), (0, 0, 0, 0)), v)


SHOULDER = {"L": "L", "R": "R", "ZL": "ZL", "ZR": "ZR"}
# 肩键在机身上沿的徽标锚点（画布坐标，徽标中心）
SHOULDER_ANCHOR = {"L": (232, 150), "ZL": (232, 150), "R": (1688, 150), "ZR": (1688, 150)}


@lru_cache(maxsize=64)
def shoulder_badge(key, fade):
    """肩键在正面看不到 → 在机身上沿打一个发光的键名胶囊。"""
    label = SHOULDER[key]
    f = ImageFont.truetype("/home/brejce/.local/share/fonts/MapleMono-NF-CN-Bold.ttf", 30)
    tw = f.getbbox(label)[2]
    bw, bh = tw + 34, 46
    lay = Image.new("RGBA", (bw + 40, bh + 40), (0, 0, 0, 0))
    d = ImageDraw.Draw(lay)
    d.rounded_rectangle((20, 20, 20 + bw, 20 + bh), radius=12, fill=(250, 252, 255, 235))
    d.text((20 + 17, 20 + 7), label, font=f, fill=(18, 22, 30, 255))
    glow = lay.filter(ImageFilter.GaussianBlur(9))
    k = fade / 255
    out = Image.new("RGBA", lay.size, (0, 0, 0, 0))
    g2 = glow.copy(); g2.putalpha(g2.getchannel("A").point(lambda v: int(v * 0.9 * k)))
    out.alpha_composite(g2)
    lay2 = lay.copy(); lay2.putalpha(lay2.getchannel("A").point(lambda v: int(v * k)))
    out.alpha_composite(lay2)
    x, y = SHOULDER_ANCHOR[key]
    return out, (int(x) - out.size[0] // 2, int(y) - out.size[1] // 2)


@lru_cache(maxsize=128)
def glow_sprite(key, fade):
    from PIL import ImageChops
    x, y, r = BUTTONS[key]
    R = int(r * 2.4) + 16
    n = 2 * R
    ring = Image.new("L", (n, n), 0)
    ImageDraw.Draw(ring).ellipse((R - r - 3, R - r - 3, R + r + 3, R + r + 3),
                                 outline=255, width=max(4, int(r * 0.22)))
    ring = ring.filter(ImageFilter.GaussianBlur(max(1.2, r * 0.05)))
    core = Image.new("L", (n, n), 0)
    ImageDraw.Draw(core).ellipse((R - r * 0.9, R - r * 0.9, R + r * 0.9, R + r * 0.9), fill=70)
    core = core.filter(ImageFilter.GaussianBlur(r * 0.9))
    m = ImageChops.add(ring, core).point(lambda v: int(v * fade / 255))
    sprite = Image.merge("RGBA", (Image.new("L", (n, n), 255), Image.new("L", (n, n), 252),
                                  Image.new("L", (n, n), 240), m))
    return sprite, (int(x) - R, int(y) - R)


def caption(frame, text):
    if not text:
        return frame
    f = ImageFont.truetype("/usr/share/fonts/noto-cjk/NotoSansCJK-Medium.ttc", 44, index=2)
    d = ImageDraw.Draw(frame)
    tw = d.textlength(text, font=f)
    bw, bh = int(tw) + 120, 92
    x, y = (W - bw) // 2, H - 128
    bar = Image.new("RGBA", (bw, bh), (0, 0, 0, 0))
    ImageDraw.Draw(bar).rounded_rectangle((0, 0, bw - 1, bh - 1), radius=14, fill=(10, 12, 16, 190))
    frame.alpha_composite(bar, (x, y))
    d.text((x + (bw - tw) / 2, y + 20), text, font=f, fill=(240, 244, 250))
    return frame


def compose(footage, caption_text=None, key=None):
    frame = base().copy()
    warped = footage.transform((W, H), Image.PERSPECTIVE, COEFFS, Image.BICUBIC)
    frame.paste(warped, (0, 0), screen_mask())
    frame.alpha_composite(screen_glow())
    frame.alpha_composite(vignette())
    if key:
        spr, pos = (shoulder_badge(key, 235) if key in SHOULDER else glow_sprite(key, 235))
        frame.alpha_composite(spr, pos)
    return caption(frame, caption_text)


def still(take, seconds, caption_text=None, key=None):
    f = ROOT / "raw" / f"{take}.mp4"
    raw = subprocess.run(["ffmpeg", "-v", "error", "-ss", str(seconds), "-i", str(f),
                          "-frames:v", "1", "-pix_fmt", "rgb24", "-f", "rawvideo", "-"],
                         capture_output=True).stdout
    img = Image.frombytes("RGB", (1280, 720), raw[:1280 * 720 * 3])
    return compose(img, caption_text, key)


if __name__ == "__main__":
    out = ROOT / "work"
    shots = [
        ("still-en.png", "t13-en", 78.4, "L 魔法键：按下白块选中的键", "L"),
        ("still-ime.png", "t21-zh", 39.0, "输入 nihao  →  候选条  →  ZL 空格确认", "ZL"),
        ("still-out.png", "t21-zh", 41.6, "输出「你好」", None),
    ]
    for name, take, t, cap, key in shots:
        p = out / name
        still(take, t, cap, key).convert("RGB").save(p)
        print(p)
