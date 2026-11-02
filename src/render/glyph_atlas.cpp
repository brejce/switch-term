#include "render/glyph_atlas.h"
#include FT_SYNTHESIS_H

#include <cstdio>

namespace render {

bool GlyphAtlas::init(disp::DisplayBackend* bd, int size) {
    bd_ = bd;
    size_ = size;
    pixels_.assign(size_t(size_) * size_, 0);
    disp::TextureDesc d;
    d.width = size_;
    d.height = size_;
    d.format = disp::PixelFormat::A8;
    tex_ = bd_->createTexture(d);
    return tex_ != 0;
}

bool GlyphAtlas::hasGlyph(const FontFace* f, uint32_t cp) {
    return f && f->face && FT_Get_Char_Index(f->face, cp) != 0;
}

const Glyph& GlyphAtlas::glyphResolved(const FontFace* f, uint32_t cp) {
    if (hasGlyph(f, cp)) return glyph(f, cp);
    const Glyph& g = glyph(f, cp);  // 主面 .notdef（advance 可能为 0）
    for (const FontFace* fb = f ? f->fallback : nullptr; fb; fb = fb->fallback) {
        if (hasGlyph(fb, cp)) return glyph(fb, cp);
    }
    return g;
}

const Glyph& GlyphAtlas::glyph(const FontFace* f, uint32_t cp) {
    Key k{f, cp};
    auto it = glyphs_.find(k);
    if (it != glyphs_.end()) return it->second;

    Glyph g;
    // 小字号用 light hinting：横笔画不做整数对齐，笔画粗细才均匀（16px 以上用 normal）
    const int flags = FT_LOAD_RENDER | (f->px < 16 ? FT_LOAD_TARGET_LIGHT : FT_LOAD_TARGET_NORMAL);
    if (!FT_Load_Char(f->face, cp, flags)) {
        FT_GlyphSlot s = f->face->glyph;
        if (f->syntheticBold) FT_GlyphSlot_Embolden(s);
        g.valid = true;
        g.w = int(s->bitmap.width);
        g.h = int(s->bitmap.rows);
        g.left = s->bitmap_left;
        g.top = s->bitmap_top;
        g.advance = int((s->advance.x + 32) >> 6);  // 四舍五入，别用截断

        if (g.w > 0 && g.h > 0) {
            if (penX_ + g.w + 1 > size_) {  // 换行
                penX_ = 1;
                penY_ += rowH_ + 1;
                rowH_ = 0;
            }
            if (penY_ + g.h + 1 > size_) {
                std::fprintf(stderr, "atlas full (%d/%d)\n", cp, size_);
                g.w = g.h = 0;
            } else {
                g.texX = penX_;
                g.texY = penY_;
                for (int y = 0; y < g.h; ++y) {
                    const uint8_t* src = s->bitmap.buffer + y * s->bitmap.pitch;
                    std::copy(src, src + g.w, &pixels_[size_t(g.texY + y) * size_ + g.texX]);
                }
                penX_ += g.w + 1;
                if (g.h > rowH_) rowH_ = g.h;
            }
        }
    }

    auto res = glyphs_.emplace(k, g);
    if (g.w > 0 && g.h > 0) bd_->updateTexture(tex_, pixels_.data(), size_);
    return res.first->second;
}

}  // namespace render
