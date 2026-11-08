#!/usr/bin/env python3
"""把 720p 真机录屏/截图合成到 Switch 外壳里，可选高亮一颗按键。

用法:
  python3 video/assets/mockup.py <footage.png> <out.png> [BUTTON] ["字幕"]

BUTTON: A B X Y L ZL R ZR UP DOWN LEFT RIGHT L3 PLUS MINUS
"""
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 1920, 1080
SCREEN = (360, 190, 1200, 675)          # x, y, w, h（与 shell.svg 挖孔一致）
MONO = "/home/brejce/.local/share/fonts/MapleMono-NF-CN-Regular.ttf"
MONO_B = "/home/brejce/.local/share/fonts/MapleMono-NF-CN-Bold.ttf"
CJK = "/usr/share/fonts/noto-cjk/NotoSansCJK-Medium.ttc"   # index 2 = SC

# name: (cx, cy, w, h, label, shape("c" 圆 / "r" 圆角矩形))
BUTTONS = {
    "A": (1664, 350, 60, 60, "A", "c"),
    "B": (1614, 400, 60, 60, "B", "c"),
    "X": (1614, 300, 60, 60, "X", "c"),
    "Y": (1564, 350, 60, 60, "Y", "c"),
    "L": (167, 141, 130, 46, "", "r"),
    "ZL": (168, 115, 120, 42, "", "r"),
    "R": (1753, 141, 130, 46, "", "r"),
    "ZR": (1754, 115, 120, 42, "", "r"),
    "UP": (206, 546, 44, 44, "", "r"),
    "DOWN": (206, 634, 44, 44, "", "r"),
    "LEFT": (162, 590, 44, 44, "", "r"),
    "RIGHT": (250, 590, 44, 44, "", "r"),
    "L3": (206, 350, 100, 100, "", "c"),
    "PLUS": (1615, 233, 30, 30, "", "r"),
    "MINUS": (299, 226, 30, 30, "", "r"),
}


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
    return Image.composite(Image.new("RGB", (W, H), (74, 100, 138)), img, glow)


def press(img, name):
    cx, cy, w, h, label, shape = BUTTONS[name]
    size = int(max(w, h) * 2.6)
    glow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    gm = Image.radial_gradient("L").resize((size, size), Image.LANCZOS)
    gm = gm.point(lambda v: int(255 * (1 - v / 255.0) ** 2.4 * 0.85))
    glow.paste((255, 255, 255, 255), (0, 0), gm)
    glow = glow.filter(ImageFilter.GaussianBlur(size / 10))
    img.alpha_composite(glow, (cx - size // 2, cy - size // 2))

    d = ImageDraw.Draw(img)
    pad = 4
    if shape == "c":
        box = (cx - w / 2 - pad, cy - h / 2 - pad, cx + w / 2 + pad, cy + h / 2 + pad)
        d.ellipse(box, outline=(255, 255, 255, 240), width=3)
        d.ellipse((cx - w / 2 + 5, cy - h / 2 + 5, cx + w / 2 - 5, cy + h / 2 - 5),
                  fill=(10, 12, 16, 140))
    else:
        r = min(w, h) / 2.2
        box = (cx - w / 2 - pad, cy - h / 2 - pad, cx + w / 2 + pad, cy + h / 2 + pad)
        d.rounded_rectangle(box, radius=r + pad, outline=(255, 255, 255, 240), width=3)
        d.rounded_rectangle((cx - w / 2 + 4, cy - h / 2 + 4, cx + w / 2 - 4, cy + h / 2 - 4),
                            radius=r, fill=(10, 12, 16, 120))
    if label:
        f = ImageFont.truetype(MONO_B, 24)
        bb = d.textbbox((0, 0), label, font=f)
        d.text((cx - (bb[2] - bb[0]) / 2 - bb[0], cy - (bb[3] - bb[1]) / 2 - bb[1]),
               label, font=f, fill=(255, 255, 255, 255))


def main():
    footage_path, out = sys.argv[1], sys.argv[2]
    pressed = sys.argv[3] if len(sys.argv) > 3 else None
    caption = sys.argv[4] if len(sys.argv) > 4 else ""

    img = bg().convert("RGBA")

    x, y, w, h = SCREEN
    shot = Image.open(footage_path).convert("RGB").resize((w, h), Image.LANCZOS)
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, w - 1, h - 1), radius=6, fill=255)
    img.paste(shot, (x, y), mask)

    glare = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glare)
    for i in range(h):
        a = int(18 * max(0.0, 1 - i / (h * 0.6)))
        gd.line([(0, i), (w, i)], fill=(255, 255, 255, a))
    img.alpha_composite(glare, (x, y))

    shell = Image.open("video/assets/shell.png").convert("RGBA")
    img.alpha_composite(shell, (0, 0))

    if pressed in BUTTONS:
        press(img, pressed)

    d = ImageDraw.Draw(img)
    if caption:
        f_cap = ImageFont.truetype(CJK, 42, index=2)
        bb = d.textbbox((0, 0), caption, font=f_cap)
        cw = bb[2] - bb[0]
        bar_w = min(W - 160, cw + 120)
        bar = Image.new("RGBA", (bar_w, 92), (0, 0, 0, 0))
        ImageDraw.Draw(bar).rounded_rectangle((0, 0, bar_w - 1, 91), radius=12, fill=(4, 6, 9, 175))
        img.alpha_composite(bar, ((W - bar_w) // 2, H - 112))
        d = ImageDraw.Draw(img)
        d.text(((W - cw) / 2 - bb[0], H - 112 + 46 - (bb[3] - bb[1]) / 2 - bb[1]),
               caption, font=f_cap, fill=(235, 238, 245, 255))

    f_tag = ImageFont.truetype(MONO, 24)
    d.text((W - 96 - d.textlength("sit/0.1", font=f_tag), 60), "sit/0.1",
           font=f_tag, fill=(110, 120, 138, 255))

    img.convert("RGB").save(out, quality=95)
    print(out)


if __name__ == "__main__":
    main()
