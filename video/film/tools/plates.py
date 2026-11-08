#!/usr/bin/env python3
"""Halftone / print plates for the SIT-001 film."""
from __future__ import annotations

import math
import random
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent                      # video/film
OUT = ROOT / "assets" / "plates"
SRC = Path("/home/brejce/Project/switch-ime-term")

PAPER = (238, 228, 206)
INK = (20, 28, 40)
OVER = (198, 66, 45)
HI = (198, 216, 78)


def paper(w: int, h: int, seed: int = 7) -> Image.Image:
    im = Image.new("RGB", (w, h), PAPER)
    rnd = random.Random(seed)
    small = Image.new("L", (w // 26 + 2, h // 26 + 2))
    px = small.load()
    for y in range(small.height):
        for x in range(small.width):
            px[x, y] = 128 + rnd.randint(-24, 20)
    mottle = small.resize((w, h), Image.BICUBIC).filter(ImageFilter.GaussianBlur(8))
    tint = Image.composite(Image.new("RGB", (w, h), (231, 217, 190)),
                           Image.new("RGB", (w, h), PAPER), mottle)
    im = Image.blend(im, tint, 0.6)
    vig = Image.new("L", (w, h), 0)
    ImageDraw.Draw(vig).ellipse((-w * .2, -h * .24, w * 1.2, h * 1.24), fill=255)
    vig = vig.filter(ImageFilter.GaussianBlur(w // 10))
    return Image.composite(im, Image.new("RGB", (w, h), (204, 189, 162)), vig)


def ink_print(src: Path, w: int = 2048, lo: int = 45, span: int = 85, gamma: float = .8,
              bleed: bool = True, out: Path | None = None) -> Image.Image:
    img = Image.open(src).convert("RGB")
    h = round(img.height * w / img.width)
    lum = img.convert("L").resize((w, h), Image.LANCZOS)

    def f(v):
        t = max(0.0, min(1.0, (v - lo) / span))
        return int((t ** gamma) * 255)

    a = lum.point(f)
    if bleed:
        a = a.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.GaussianBlur(.5))
    plate = Image.composite(Image.new("RGB", (w, h), INK), paper(w, h), a)
    tex = paper(w, h, seed=11).convert("L").point(lambda v: 240 + int((v - 240) * .22))
    plate = ImageChops.multiply(plate, Image.merge("RGB", (tex, tex, tex)))
    if out:
        plate.save(out)
    return plate


def dots(w, h, step, angle, field, colour, base):
    d = ImageDraw.Draw(base)
    a = math.radians(angle)
    ca, sa = math.cos(a), math.sin(a)
    cx, cy = w / 2, h / 2
    for j in range(int(h / step) + 3):
        for i in range(int(w / step) + 3):
            u = (i - int(w / step) / 2 - 1) * step
            v = (j - int(h / step) / 2 - 1) * step
            x = cx + u * ca - v * sa
            y = cy + u * sa + v * ca
            if not (0 <= x < w and 0 <= y < h):
                continue
            r = step * .72 * field(x, y)
            if r < .5:
                continue
            d.ellipse((x - r, y - r, x + r, y + r), fill=colour)
    return base


def device_plate(dev_path: Path, screen_plate: Image.Image, out: Path, w: int = 1720) -> Image.Image:
    dev = Image.open(dev_path).convert("RGBA")
    h = round(dev.height * w / dev.width)
    dev = dev.resize((w, h), Image.LANCZOS)
    ring = dev.split()[3]
    box = ring.getbbox()
    pad = 70
    W, H = w + pad * 2, h + pad * 2
    plate = paper(W, H)

    # 1) screen content sits under the device silhouette
    hole = ring.point(lambda v: 255 - v)          # solid inside the screen cut-out
    scr = screen_plate.resize((w, h), Image.LANCZOS)
    plate.paste(scr, (pad, pad), hole.filter(ImageFilter.MaxFilter(5)))

    # 2) flat highlight fill for the body
    body = Image.new("RGB", (w, h), HI)
    plate.paste(body, (pad, pad), ring)

    # 3) halftone shading where the body is dark (buttons, rails, shadow side)
    lum = dev.convert("L")
    shade = Image.new("RGB", (w, h), PAPER)
    dots(w, h, 9, 20, lambda x, y: max(0.0, 0.55 * (1 - lum.getpixel((min(w - 1, int(x)), min(h - 1, int(y)))) / 255) ** 1.2), INK, shade)
    plate.paste(shade, (pad, pad), ring.point(lambda v: int(v * 0.85)))

    # 4) ink outline
    inner = ring.filter(ImageFilter.MinFilter(7))
    edge = ImageChops.subtract(ring, inner)
    edge = edge.filter(ImageFilter.GaussianBlur(.7)).point(lambda v: min(255, v * 3))
    plate.paste(Image.new("RGB", (w, h), INK), (pad, pad), edge)

    plate.save(out)
    return plate


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    scr = ink_print(SRC / "build/shot/headless2.png", out=OUT / "screen.png")
    dev = device_plate(ROOT / "assets" / "device.png", scr, OUT / "device.png")
    print("plates:", [p.name for p in OUT.iterdir()], dev.size)
