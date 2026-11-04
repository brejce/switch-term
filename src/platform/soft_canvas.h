#pragma once
// 软件光栅画布：fillRect / drawTexture 的合成都走这里，headless 和 sdl 共用一份实现
#include <cstdint>
#include <vector>

#include "platform/display_backend.h"

namespace disp {

class SoftCanvas {
public:
    void resize(int w, int h);
    int width() const { return w_; }
    int height() const { return h_; }
    const std::vector<uint8_t>& pixels() const { return fb_; }

    void clear();
    // 裁剪矩形：fillRect/drawTexture（含 textInBox 的字形）只在它和画布的交集里落地
    void setClip(const Rect& r);
    void clearClip();
    TextureId createTexture(const TextureDesc&);
    void updateTexture(TextureId, const uint8_t* pixels, int row_stride);
    void fillRect(const Rect&, Color);
    void drawTexture(TextureId, const Rect& src, const Rect& dst, Color tint);

private:
    struct Tex {
        int w = 0, h = 0;
        PixelFormat fmt = PixelFormat::RGBA8888;
        std::vector<uint8_t> px;
    };

    static int bpp(PixelFormat f) { return f == PixelFormat::A8 ? 1 : 4; }
    Tex& tex(TextureId id) { return textures_[id - 1]; }
    Rect clip(const Rect& r) const;
    Rect intersect(const Rect& a, const Rect& b) const;
    void blendPixel(int x, int y, Color c);

    int w_ = 0, h_ = 0;
    Rect clipRect_;  // setClip 设的裁剪矩形；clear()/resize() 回到整块画布
    std::vector<uint8_t> fb_;
    std::vector<Tex> textures_;
};

}  // namespace disp
