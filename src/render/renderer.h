#pragma once
// 上层唯一绘制入口：只发 fillRect / drawTexture，不碰像素，不含后端分支
#include <string>
#include <vector>

#include "platform/display_backend.h"
#include "render/font_manager.h"
#include "render/glyph_atlas.h"

namespace render {

enum class Align { Left, Center, Right };

class Renderer {
public:
    bool init(disp::DisplayBackend* bd, FontManager* fm);

    void beginFrame();
    void endFrame();

    void fill(disp::Rect r, disp::Color c);
    // 裁剪：后续 fill/text 只在 r 里落地（textInBox/textBaseline 走 drawTexture，逐字形被裁）；
    // clearClip 回到整块画布。调用方成对用，别跨帧漏掉。
    void setClip(disp::Rect r) { bd_->setClip(r); }
    void clearClip() { bd_->clearClip(); }

    int textWidth(const std::string& utf8, const FontFace* f, int charSpacing = 0);
    void textBaseline(int x, int baselineY, const std::string& utf8, const FontFace* f,
                      disp::Color c, int charSpacing = 0);
    // 在 box 内垂直居中（按 ascent/descent 视觉居中）
    void textInBox(disp::Rect box, const std::string& utf8, const FontFace* f, disp::Color c,
                   Align align = Align::Left, int padLeft = 0, int padRight = 0,
                   int charSpacing = 0);

    const FontFace* font(const std::string& query, double px) { return fm_->get(query, px); }

private:
    static std::vector<uint32_t> decodeUtf8(const std::string& s);

    disp::DisplayBackend* bd_ = nullptr;
    FontManager* fm_ = nullptr;
    GlyphAtlas atlas_;
};

}  // namespace render
