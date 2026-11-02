#include "render/renderer.h"

namespace render {

bool Renderer::init(disp::DisplayBackend* bd, FontManager* fm) {
    bd_ = bd;
    fm_ = fm;
    return atlas_.init(bd);
}

void Renderer::beginFrame() { bd_->beginFrame(); }
void Renderer::endFrame() { bd_->endFrame(); }

void Renderer::fill(disp::Rect r, disp::Color c) { bd_->fillRect(r, c); }

std::vector<uint32_t> Renderer::decodeUtf8(const std::string& s) {
    std::vector<uint32_t> out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        uint32_t cp = 0;
        int n = 0;
        if (c < 0x80) { cp = c; n = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
        else { ++i; continue; }
        ++i;
        for (int k = 0; k < n && i < s.size(); ++k, ++i) cp = (cp << 6) | (s[i] & 0x3F);
        out.push_back(cp);
    }
    return out;
}

int Renderer::textWidth(const std::string& utf8, const FontFace* f, int charSpacing) {
    if (!f) return 0;
    int w = 0;
    for (uint32_t cp : decodeUtf8(utf8)) w += atlas_.glyphResolved(f, cp).advance + charSpacing;
    return w;
}

void Renderer::textBaseline(int x, int baselineY, const std::string& utf8, const FontFace* f,
                            disp::Color c, int charSpacing) {
    if (!f || c.a == 0) return;
    int pen = x;
    for (uint32_t cp : decodeUtf8(utf8)) {
        const Glyph& g = atlas_.glyphResolved(f, cp);
        if (g.w > 0 && g.h > 0 && cp != ' ') {
            bd_->drawTexture(atlas_.texture(), disp::Rect{g.texX, g.texY, g.w, g.h},
                             disp::Rect{pen + g.left, baselineY - g.top, g.w, g.h}, c);
        }
        pen += g.advance + charSpacing;
    }
}

void Renderer::textInBox(disp::Rect box, const std::string& utf8, const FontFace* f,
                         disp::Color c, Align align, int padLeft, int padRight,
                         int charSpacing) {
    if (!f) return;
    int tw = textWidth(utf8, f, charSpacing);
    int innerX = box.x + padLeft;
    int innerW = box.w - padLeft - padRight;
    int x = innerX;
    if (align == Align::Center) x = innerX + (innerW - tw) / 2;
    else if (align == Align::Right) x = innerX + innerW - tw;
    int baseline = box.y + box.h / 2 + (f->ascent - f->descent) / 2;
    textBaseline(x, baseline, utf8, f, c, charSpacing);
}

}  // namespace render
