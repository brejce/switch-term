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
    // 基础裁剪：一帧的绘制范围（脏区），setClip 只能再往里收，clearClip 回到它
    void setBaseClip(const Rect& r) { baseClip_ = r; clipRect_ = baseClip_; }
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
    Rect baseClip_;  // 本帧的重画范围（默认整块画布）；setClip 的裁剪框再跟它求交
    Rect clipRect_;  // 当前生效的裁剪矩形；clear()/clearClip() 回到 baseClip_
    std::vector<uint8_t> fb_;
    std::vector<Tex> textures_;
};

}  // namespace disp
