"""Turn a real screenshot crop into an ink plate (ink on transparent).

ink coverage = darkness of the source, so a dark UI prints as solid ink with the
bright glyphs knocked out to paper — exactly like a printed screen grab.
"""
import sys
from PIL import Image, ImageFilter
INK = (20, 28, 40)


def plate(src, box, out, width, gamma=0.9):
    im = Image.open(src).convert('RGB')
    if im.width < im.height:                      # portrait fb grab
        im = im.rotate(90, expand=True)
    g = im.crop(box)
    h = round(g.height * width / g.width)
    lum = g.convert('L').resize((width, h), Image.LANCZOS)
    # ink coverage: dark UI prints solid, bright glyphs are knocked out to paper.
    def cov(v):
        t = max(0.0, min(1.0, (v - 40.0) / 110.0))
        return 255.0 * (1.0 - t) ** gamma
    lut = [int(cov(i)) for i in range(256)]
    a = lum.point(lut).filter(ImageFilter.GaussianBlur(0.6))
    img = Image.new('RGBA', (width, h), INK + (0,))
    img.putalpha(a)
    img.save(out)
    print(out, img.size)


if __name__ == '__main__':
    src, box, out, width = sys.argv[1], eval(sys.argv[2]), sys.argv[3], int(sys.argv[4])
    plate(src, box, out, width)
