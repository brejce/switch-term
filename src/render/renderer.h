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
    // 脏矩形版：只重画 base clip 内的像素，endFrame 只把这些块搬到 fb
    void beginFrame(const disp::DamageList& d) { bd_->beginFrame(d); }
    void endFrame(const disp::DamageList& d) { bd_->endFrame(d); }
    void setBaseClip(const disp::Rect& r) { bd_->setBaseClip(r); }

    void fill(disp::Rect r, disp::Color c);
    // 裁剪：后续 fill/text 只在 r 里落地（textInBox/textBaseline 走 drawTexture，逐字形被裁）；
    // clearClip 回到整块画布。调用方成对用，别跨帧漏掉。
    void setClip(disp::Rect r) { bd_->setClip(r); }
    void clearClip() { bd_->clearClip(); }

    int textWidth(const std::string& utf8, const FontFace* f, int charSpacing = 0);
    // scale < 1 = 直接把字形位图缩着贴（最近邻）。用于"远一点的键整体变小"这类效果，
    // 比按字号重新栅格化省事，也不会因为字号缓存产生跳档。
    void textBaseline(int x, int baselineY, const std::string& utf8, const FontFace* f,
                      disp::Color c, int charSpacing = 0, float scale = 1.f);
    // 在 box 内垂直居中（按 ascent/descent 视觉居中）
    void textInBox(disp::Rect box, const std::string& utf8, const FontFace* f, disp::Color c,
                   Align align = Align::Left, int padLeft = 0, int padRight = 0,
                   int charSpacing = 0, float scale = 1.f);

    const FontFace* font(const std::string& query, double px) { return fm_->get(query, px); }

private:
    static std::vector<uint32_t> decodeUtf8(const std::string& s);

    disp::DisplayBackend* bd_ = nullptr;
    FontManager* fm_ = nullptr;
    GlyphAtlas atlas_;
};

}  // namespace render
