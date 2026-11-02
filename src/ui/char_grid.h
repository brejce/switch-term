#pragma once
// 字符网格绘制助手：UI 里唯一的"块"表现手段是反色（UI_DESIGN.md §二）
#include <string>

#include "render/renderer.h"
#include "ui/theme.h"

namespace ui {

inline disp::Rect cellBox(int x0, int y0, int cw, int ch, int col, int row) {
    return disp::Rect{x0 + col * cw, y0 + row * ch, cw, ch};
}

// 单字符格；reverse = 反色高亮（底填充 + 底色字）
inline void putCell(render::Renderer& r, disp::Rect box, const render::FontFace* f,
                    const std::string& s, uint32_t fg, bool reverse = false) {
    if (reverse) {
        r.fill(box, c(fg));
        r.textInBox(box, s, f, c(col::kBg), render::Align::Center);
    } else {
        r.textInBox(box, s, f, c(fg), render::Align::Center);
    }
}

// 一行等宽文本，从像素 x 起；返回画完后的 x
inline int putText(render::Renderer& r, int x, int y, int h, const render::FontFace* f,
                   const std::string& s, uint32_t fg, int sp = 0) {
    r.textInBox({x, y, 0, h}, s, f, c(fg), render::Align::Left, 0, 0, sp);
    return x + r.textWidth(s, f, sp);
}

}  // namespace ui
