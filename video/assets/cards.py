#!/usr/bin/env python3
"""生成 30s 宣传片的标题卡 / 数字卡 / 结尾卡（1920x1080 PNG）。"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 1920, 1080
OUT = Path(__file__).resolve().parent.parent / "work"

MAPLE = "/home/brejce/.local/share/fonts/MapleMono-NF-CN-{}.ttf"
NOTO = "/usr/share/fonts/noto-cjk/NotoSansCJK-{}.ttc"
NOTO_INDEX = 2  # 中文在 ttc 里的字面索引

CYAN = (51, 212, 236)
WHITE = (236, 241, 248)
GRAY = (138, 150, 168)
DIM = (96, 108, 126)


def font(kind, size, weight="Regular"):
    if kind == "mono":
        return ImageFont.truetype(MAPLE.format(weight), size)
    return ImageFont.truetype(NOTO.format(weight), size, index=NOTO_INDEX)


def background():
    """竖向渐变 + 中心柔光 + 四周压暗，和录屏合成用的是同一套底。"""
    img = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(img)
    for y in range(H):
        t = y / H
        d.line([(0, y), (W, y)], fill=(int(30 - 24 * t), int(37 - 29 * t), int(52 - 40 * t)))
    halo = Image.new("L", (W, H), 0)
    ImageDraw.Draw(halo).ellipse((260, 60, W - 260, H - 60), fill=255)
    halo = halo.filter(ImageFilter.GaussianBlur(260))
    img = Image.composite(Image.new("RGB", (W, H), (76, 102, 140)), img, halo)
    vig = Image.new("L", (W, H), 0)
    ImageDraw.Draw(vig).rectangle((0, 0, W, H), outline=0, width=0)
    vig = Image.new("L", (W, H), 255)
    ImageDraw.Draw(vig).ellipse((-420, -320, W + 420, H + 320), fill=0)
    vig = vig.filter(ImageFilter.GaussianBlur(320))
    img = Image.composite(Image.new("RGB", (W, H), (4, 6, 10)), img, vig)
    return img.convert("RGBA")


def tracked(draw, xy, text, fnt, fill, tracking=0):
    """带字距的绘制，返回结束 x。"""
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=fnt, fill=fill)
        x += draw.textlength(ch, font=fnt) + tracking
    return x


def title_card():
    img = background()
    d = ImageDraw.Draw(img)
    y = 300

    # 细竖线做版式锚点
    d.rectangle((180, y + 6, 186, y + 190), fill=(*CYAN, 255))

    tracked(d, (232, y), "sit", font("mono", 176, "Bold"), WHITE)
    d.text((232 + 300, y + 62), "/0.1", CYAN, font=font("mono", 84, "Bold"))

    d.text((236, y + 232), "用手柄打字的 Linux 终端", WHITE, font=font("cjk", 82, "Medium"))
    d.text((240, y + 356), "Nintendo Switch ·  无桌面 ·  纯 fbdev ·  1280×720 直出",
           GRAY, font=font("cjk", 36))

    # 右下角留一个很小的 "实机录制" 标记
    d.text((240, H - 190), "实机录制：Switch fb0 直采 60fps", DIM, font=font("cjk", 30))
    return img


def numbers_card():
    img = background()
    d = ImageDraw.Draw(img)

    d.text((180, 250), "一台 2017 年的掌机，", WHITE, font=font("cjk", 66, "Medium"))
    d.text((180, 334), "一套不依赖桌面的终端。", WHITE, font=font("cjk", 66, "Medium"))

    stats = [
        ("330", "MB", "常驻内存", "整个终端 + 渲染器"),
        ("13", "s", "开机到 multi-user", "内核 6.7s + 用户态 6.4s"),
        ("0", "", "桌面组件 / 内核补丁", "Xorg、Wayland、EGL 都没有"),
    ]
    x = 180
    for num, unit, label, sub in stats:
        d.rectangle((x, 520, x + 78, 524), fill=(*CYAN, 255))
        d.text((x, 552), num, WHITE, font=font("mono", 118, "Bold"))
        nw = d.textlength(num, font=font("mono", 118, "Bold"))
        d.text((x + nw + 10, 600), unit, CYAN, font=font("mono", 54, "Bold"))
        d.text((x, 700), label, WHITE, font=font("cjk", 46, "Medium"))
        d.text((x, 766), sub, GRAY, font=font("cjk", 30))
        x += 580
    return img


def outro_card():
    img = background()
    d = ImageDraw.Draw(img)

    w = d.textlength("sit", font=font("mono", 150, "Bold"))
    d.text((180, 300), "sit", WHITE, font=font("mono", 150, "Bold"))
    d.text((180 + w + 12, 356), "/0.1", CYAN, font=font("mono", 84, "Bold"))

    d.text((184, 500), "switch-ime-term", WHITE, font=font("mono", 64, "Bold"))
    d.text((184, 596), "摇杆选键 · 内置拼音 · 全屏终端 · 配置热重载",
           GRAY, font=font("cjk", 42))

    d.rectangle((184, 720, 262, 724), fill=(*CYAN, 255))
    d.text((184, 752), "Made on a Nintendo Switch", WHITE, font=font("mono", 46, "Bold"))
    d.text((184, 828), "30 秒片：全部画面来自实机，外壳为后期合成", DIM, font=font("cjk", 30))
    return img


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, fn in [("title", title_card), ("numbers", numbers_card), ("outro", outro_card)]:
        p = OUT / f"{name}.png"
        fn().convert("RGB").save(p)
        print(p)


if __name__ == "__main__":
    main()
