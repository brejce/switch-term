#pragma once
// 字形图集：1024x1024 A8，简易 shelf 打包，永久驻留（ASCII 常驻 + CJK 按需）
#include <cstdint>
#include <map>
#include <vector>

#include "platform/display_backend.h"
#include "render/font_manager.h"

namespace render {

struct Glyph {
    bool valid = false;
    int texX = 0, texY = 0;
    int w = 0, h = 0;
    int left = 0;   // 相对绘制原点的水平偏移
    int top = 0;    // 基线以上到 bitmap 顶部
    int advance = 0;
};

class GlyphAtlas {
public:
    bool init(disp::DisplayBackend* bd, int size = 1024);
    disp::TextureId texture() const { return tex_; }
    const Glyph& glyph(const FontFace* f, uint32_t codepoint);
    // 按 fallback 链选面：主面缺字则用 fallback
    const Glyph& glyphResolved(const FontFace* f, uint32_t codepoint);
    static bool hasGlyph(const FontFace* f, uint32_t codepoint);

private:
    struct Key {
        const FontFace* f;
        uint32_t cp;
        bool operator<(const Key& o) const {
            if (f != o.f) return f < o.f;
            return cp < o.cp;
        }
    };

    disp::DisplayBackend* bd_ = nullptr;
    disp::TextureId tex_ = 0;
    int size_ = 1024;
    int penX_ = 1, penY_ = 1, rowH_ = 0;
    std::vector<uint8_t> pixels_;
    std::map<Key, Glyph> glyphs_;
};

}  // namespace render
