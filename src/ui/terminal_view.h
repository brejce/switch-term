#pragma once
#include <string>
#include <vector>

#include "ime/pinyin_index.h"
#include "render/renderer.h"
#include "term/vterm_bridge.h"
#include "ui/theme.h"

namespace ui {

struct TerminalView {
    const term::VTermBridge* grid = nullptr;
    int cols = 0, rows = 0;
    int cellW = 8, cellH = 22;
    std::string preedit;            // 组字串（拼音），画在光标处
    std::vector<ime::Cand> cands;  // 候选（含注音）
    int candPage = 0, candPageSize = 7, candPageCount = 0;
    int scroll = 0;  // 回看行数（0 = 跟随最新输出）
    int selAnchor = -1, selCaret = -1;  // 选区：可见区内的线性格号，-1 = 没选
};

void drawTerminal(render::Renderer& r, const FontBook& f, const TerminalView& v);

// 虚拟行（含滚动历史）取一格；visibleText 把当前可见区导成文本（Ctrl+Shift+C 用）
term::CellView virtualCell(const TerminalView& v, int row, int col);
std::string visibleText(const TerminalView& v);
// 选区（Shift+ZL/ZR 或 Shift+方向键扩展）；selectedText 只导出选中的部分
void selStep(TerminalView& v, int n);   // n 可正可负
void selClear(TerminalView& v);
bool selActive(const TerminalView& v);
std::string selectedText(const TerminalView& v);

}  // namespace ui
