#include "platform/soft_canvas.h"

#include <algorithm>
#include <cstring>

namespace disp {

void SoftCanvas::resize(int w, int h) {
    w_ = w;
    h_ = h;
    fb_.assign(size_t(w) * h * 4, 0);
}

void SoftCanvas::clear() { std::fill(fb_.begin(), fb_.end(), uint8_t(0)); }

TextureId SoftCanvas::createTexture(const TextureDesc& d) {
    Tex t;
    t.w = d.width;
    t.h = d.height;
    t.fmt = d.format;
    t.px.assign(size_t(d.width) * d.height * bpp(d.format), 0);
    textures_.push_back(std::move(t));
    return TextureId(textures_.size());
}

void SoftCanvas::updateTexture(TextureId id, const uint8_t* pixels, int row_stride) {
    Tex& t = tex(id);
    int b = bpp(t.fmt);
    for (int y = 0; y < t.h; ++y)
        std::memcpy(&t.px[size_t(y) * t.w * b], pixels + size_t(y) * row_stride, size_t(t.w) * b);
}

Rect SoftCanvas::clip(const Rect& r) const {
    Rect o = r;
    if (o.x < 0) { o.w += o.x; o.x = 0; }
    if (o.y < 0) { o.h += o.y; o.y = 0; }
    if (o.right() > w_) o.w = w_ - o.x;
    if (o.bottom() > h_) o.h = h_ - o.y;
    if (o.w < 0) o.w = 0;
    if (o.h < 0) o.h = 0;
    return o;
}

void SoftCanvas::blendPixel(int x, int y, Color c) {
    if (c.a == 0) return;
    uint8_t* p = &fb_[(size_t(y) * w_ + x) * 4];
    if (c.a == 255) {
        p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = 255;
        return;
    }
    int ia = 255 - c.a;
    p[0] = uint8_t((c.r * c.a + p[0] * ia) / 255);
    p[1] = uint8_t((c.g * c.a + p[1] * ia) / 255);
    p[2] = uint8_t((c.b * c.a + p[2] * ia) / 255);
    p[3] = 255;
}

void SoftCanvas::fillRect(const Rect& r0, Color c) {
    Rect r = clip(r0);
    if (r.empty() || c.a == 0) return;
    for (int y = r.y; y < r.bottom(); ++y)
        for (int x = r.x; x < r.right(); ++x) blendPixel(x, y, c);
}

void SoftCanvas::drawTexture(TextureId id, const Rect& src, const Rect& dst0, Color tint) {
    const Tex& t = tex(id);
    Rect dst = clip(dst0);
    if (dst.empty()) return;
    int b = bpp(t.fmt);
    for (int y = dst.y; y < dst.bottom(); ++y) {
        int sy = src.y + (y - dst0.y) * src.h / dst0.h;
        if (sy < 0 || sy >= t.h) continue;
        for (int x = dst.x; x < dst.right(); ++x) {
            int sx = src.x + (x - dst0.x) * src.w / dst0.w;
            if (sx < 0 || sx >= t.w) continue;
            const uint8_t* p = &t.px[(size_t(sy) * t.w + sx) * b];
            // tint 是调制色：纹理 RGB × tint RGB，透明度相乘
            if (t.fmt == PixelFormat::A8) {
                blendPixel(x, y, Color{tint.r, tint.g, tint.b, p[0]});
            } else {
                blendPixel(x, y, Color{uint8_t(p[0] * tint.r / 255), uint8_t(p[1] * tint.g / 255),
                                       uint8_t(p[2] * tint.b / 255),
                                       uint8_t(p[3] * tint.a / 255)});
            }
        }
    }
}

}  // namespace disp
