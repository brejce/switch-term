#pragma once
// 字体加载（fontconfig + FreeType）
#include <ft2build.h>
#include FT_FREETYPE_H

#include <map>
#include <memory>
#include <string>

namespace render {

struct FontFace {
    FT_Face face = nullptr;
    double px = 0;
    int ascent = 0;   // 像素，基线以上
    int descent = 0;  // 像素，正值
    int lineHeight = 0;
    std::string family;
    std::string style;
    // 缺字时回退（Maple Mono CN subset 缺 ⇄ ＋ → 回退 Noto Sans CJK）
    const FontFace* fallback = nullptr;
    // 请求了 bold 但字体家族没有 Bold 字重时，用 FreeType 合成加粗
    bool syntheticBold = false;
};

class FontManager {
public:
    ~FontManager();
    bool init();

    // query 为 fontconfig 模式串，例如 "Noto Sans Mono CJK SC:weight=bold"
    const FontFace* get(const std::string& query, double px);

private:
    FT_Library lib_ = nullptr;
    std::map<std::string, std::unique_ptr<FontFace>> cache_;
};

}  // namespace render
