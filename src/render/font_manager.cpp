#include "render/font_manager.h"

#include <fontconfig/fontconfig.h>

#include <cstdio>
#include <cstring>

namespace render {

FontManager::~FontManager() {
    for (auto& kv : cache_)
        if (kv.second->face) FT_Done_Face(kv.second->face);
    if (lib_) FT_Done_FreeType(lib_);
}

bool FontManager::init() {
    if (FT_Init_FreeType(&lib_)) {
        std::fprintf(stderr, "FT_Init_FreeType failed\n");
        return false;
    }
    if (!FcInit()) {
        std::fprintf(stderr, "FcInit failed\n");
        return false;
    }
    return true;
}

const FontFace* FontManager::get(const std::string& query, double px) {
    char key[256];
    std::snprintf(key, sizeof(key), "%s@%.2f", query.c_str(), px);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second.get();

    FcPattern* pat = FcNameParse((const FcChar8*)query.c_str());
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res = FcResultNoMatch;
    FcPattern* m = FcFontMatch(nullptr, pat, &res);
    FcPatternDestroy(pat);
    if (!m) {
        std::fprintf(stderr, "fontconfig: no match for '%s'\n", query.c_str());
        return nullptr;
    }

    FcChar8* file = nullptr;
    FcChar8* family = nullptr;
    FcChar8* style = nullptr;
    int index = 0;
    FcPatternGetString(m, FC_FILE, 0, &file);
    FcPatternGetString(m, FC_FAMILY, 0, &family);
    FcPatternGetString(m, FC_STYLE, 0, &style);
    FcPatternGetInteger(m, FC_INDEX, 0, &index);
    if (!file) {
        FcPatternDestroy(m);
        return nullptr;
    }

    std::string fileStr = (const char*)file;
    auto f = std::make_unique<FontFace>();
    if (FT_New_Face(lib_, (const char*)file, index, &f->face)) {
        std::fprintf(stderr, "FT_New_Face failed: %s (index %d)\n", (const char*)file, index);
        FcPatternDestroy(m);
        return nullptr;
    }
    FT_Select_Charmap(f->face, FT_ENCODING_UNICODE);
    // 注意：FT_Set_Char_Size 的第 3/4 参数是 DPI。DPI 必须是 72，
    // 否则 1pt≠1px（96 DPI 下字号会被放大 4/3，整屏文字都变大）。
    FT_Set_Char_Size(f->face, 0, (FT_F26Dot6)(px * 64.0), 72, 72);

    f->px = px;
    f->ascent = int(f->face->size->metrics.ascender >> 6);
    f->descent = int(-(f->face->size->metrics.descender) >> 6);
    f->lineHeight = int((f->face->size->metrics.height) >> 6);
    f->family = family ? (const char*)family : "";
    f->style = style ? (const char*)style : "";
    f->syntheticBold = query.find("bold") != std::string::npos &&
                       f->style.find("Bold") == std::string::npos;
    FcPatternDestroy(m);

    std::fprintf(stderr, "[font] %-26s %-9s %5.1fpx -> %s:%d\n", f->family.c_str(),
                 f->style.c_str(), px, fileStr.c_str(), index);

    auto* raw = f.get();
    cache_[key] = std::move(f);
    return raw;
}

}  // namespace render
