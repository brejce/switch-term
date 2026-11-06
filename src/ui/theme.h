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
// 这台机器必须自己装（~/.local/share/fonts + fc-cache -f）：fontconfig 对没装的族名是静默
// 降级成 Noto Sans（比例字体），i/l/j 落进 8px 格子就会散开，字体又“看起来没问题”。
// 崩法见 docs/DEVICE.md 「7.x 终端字体必须字节真实等宽」；FontManager::get 会在降级时报警告。
inline const char* kMono = "Maple Mono NF CN";
inline const char* kMonoBold = "Maple Mono NF CN:weight=bold";
inline const char* kCjk = "Noto Sans CJK SC";
}  // namespace fam

// 界面字号倍率（相对终端内容）：键帽、顶栏、底栏、tab 标签、面板标题都乘它，
// 终端内容不乘（等宽网格 8×21 会被撑破）。以后加设置页时改成可调。
inline constexpr float kUiFontBoost = 2.0f;

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
    const render::FontFace* keyMain = nullptr;   // 18 键帽主字符（大字）
    const render::FontFace* keySub = nullptr;    // 9 键帽副字符（小淡）
    const render::FontFace* hints = nullptr;     // 7 面板底部提示表（键|功能 两列）

    bool init(render::FontManager& fm) {
        // 两套字号，分开算：
        //   term  = 终端内容（命令行区域），只跟布局缩放走；字号以后要能在运行期调，
        //           所以这里不吃 kUiFontBoost——放大字体会撑破 8×21 的等宽网格
        //   其余  = 界面（键帽/顶栏/底栏/tab 标签/面板标题），额外乘 kUiFontBoost
        const double T = double(layout::kScale);
        const double U = T * kUiFontBoost;
        auto get = [&fm](const char* q, double px, const char* fbq) {
            const render::FontFace* f = fm.get(q, px);
            const render::FontFace* fb = fm.get(fbq, px);
            if (f) const_cast<render::FontFace*>(f)->fallback = fb;
            return f;
        };
        auto ui = [&](const char* q, double px, const char* fbq) {
            return get(q, px * U, fbq);
        };
        term = get(fam::kMono, 13 * T, fam::kCjk);
        stick = ui(fam::kMonoBold, 16, fam::kCjk);
        stick2 = ui(fam::kMono, 14, fam::kCjk);
        dpad = ui(fam::kMono, 6, fam::kCjk);  // 12px：Shift/Alt 才塞得进 28px 的格子
        abxy = ui(fam::kMonoBold, 12, fam::kCjk);
        ptitle = ui(fam::kMono, 10, fam::kCjk);
        shoulder = ui(fam::kMonoBold, 9, fam::kCjk);
        kk = ui(fam::kMonoBold, 9, fam::kCjk);
        kd = ui(fam::kMono, 9, fam::kCjk);
        bar = ui(fam::kMono, 11, fam::kCjk);
        footer = ui(fam::kMono, 10, fam::kCjk);
        keyMain = ui(fam::kMonoBold, 18, fam::kCjk);
        keySub = ui(fam::kMono, 9, fam::kCjk);
        hints = ui(fam::kMono, 7, fam::kCjk);
        return term && stick && kk && kd;
    }
};

}  // namespace ui
