#pragma once
// 视觉常量与字体：数值全部取自 ui-mockup.html（纯字符网格，无圆角/描边）
#include <cstdint>

#include "platform/display_backend.h"
#include "render/font_manager.h"
#include "theme/palette.h"
#include "ui/layout.h"

namespace ui {

inline disp::Color c(uint32_t hex, float a = 1.0f) {
    return disp::rgb(hex, uint8_t(a * 255.0f + 0.5f));
}

namespace col {
// 颜色全部来自主题（src/theme/palette.cpp），这里只做名字绑定：
// 调用点仍是 c(col::kText)，换主题不用改任何一处。
inline uint32_t &kBg = pal::bg, &kPanel = pal::panel, &kTerm = pal::term;
inline uint32_t &kLine = pal::line;  // 所有 1px 分隔线
inline uint32_t &kText = pal::text, &kDim = pal::dim, &kFaint = pal::faint;

inline uint32_t &kCyan = pal::cyan, &kRed = pal::red, &kGreen = pal::green;
inline uint32_t &kPurple = pal::purple, &kYellow = pal::yellow, &kBlue = pal::blue;

// 肩键底色
inline uint32_t &kShL = pal::shL, &kShZL = pal::shZL, &kShR = pal::shR, &kShZR = pal::shZR;
inline uint32_t &kShZLText = pal::shZLText, &kShZRText = pal::shZRText;
inline uint32_t &kRightKey = pal::rightKey;  // 右摇杆第一层字色
inline uint32_t &kKkBg = pal::kkBg;
}  // namespace col

namespace fam {
// Maple Mono NF CN：ASCII 0.6em、汉字恰好 2×，1:2 网格合格；CJK 回退 Noto
inline const char* kMono = "Maple Mono NF CN";
inline const char* kMonoBold = "Maple Mono NF CN:weight=bold";
inline const char* kCjk = "Noto Sans CJK SC";
}  // namespace fam

struct FontBook {
    const render::FontFace* term = nullptr;      // 12.5
    const render::FontFace* stick = nullptr;     // 16 bold 第一层
    const render::FontFace* stick2 = nullptr;    // 14 第二层
    const render::FontFace* dpad = nullptr;      // 8
    const render::FontFace* abxy = nullptr;      // 12 bold
    const render::FontFace* ptitle = nullptr;    // 10
    const render::FontFace* shoulder = nullptr;  // 9 bold
    const render::FontFace* kk = nullptr;        // 9 bold
    const render::FontFace* kd = nullptr;        // 8.5
    const render::FontFace* bar = nullptr;       // 11 顶栏
    const render::FontFace* footer = nullptr;    // 10 底栏

    bool init(render::FontManager& fm) {
        const double S = double(layout::kScale);  // 字号跟着布局缩放，位图才不会糊
        auto mono = [&fm, S](const char* q, double px, const char* fbq) {
            const render::FontFace* f = fm.get(q, px * S);
            const render::FontFace* fb = fm.get(fbq, px * S);
            if (f) const_cast<render::FontFace*>(f)->fallback = fb;
            return f;
        };
        term = mono(fam::kMono, 13, fam::kCjk);
        stick = mono(fam::kMonoBold, 16, fam::kCjk);
        stick2 = mono(fam::kMono, 14, fam::kCjk);
        dpad = mono(fam::kMono, 8, fam::kCjk);
        abxy = mono(fam::kMonoBold, 12, fam::kCjk);
        ptitle = mono(fam::kMono, 10, fam::kCjk);
        shoulder = mono(fam::kMonoBold, 9, fam::kCjk);
        kk = mono(fam::kMonoBold, 9, fam::kCjk);
        kd = mono(fam::kMono, 9, fam::kCjk);
        bar = mono(fam::kMono, 11, fam::kCjk);
        footer = mono(fam::kMono, 10, fam::kCjk);
        return term && stick && kk && kd;
    }
};

}  // namespace ui
