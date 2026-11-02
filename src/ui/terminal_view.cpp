#include "ui/terminal_view.h"

#include <algorithm>
#include "ui/layout.h"

using namespace ui::layout;
using render::Renderer;

namespace ui {
namespace {

std::string utf8(char32_t cp) {
    std::string s;
    if (cp < 0x80) return std::string(1, char(cp));
    if (cp < 0x800) {
        s += char(0xC0 | (cp >> 6));
        s += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += char(0xE0 | (cp >> 12));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    } else {
        s += char(0xF0 | (cp >> 18));
        s += char(0x80 | ((cp >> 12) & 0x3F));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    }
    return s;
}

}  // namespace

term::CellView virtualCell(const TerminalView& v, int row, int col) {
    if (!v.grid) return term::CellView{};
    const int hist = v.grid->historySize();
    int i = hist - std::clamp(v.scroll, 0, hist) + row;
    if (i < 0) return term::CellView{};
    if (i < hist) return v.grid->historyAt(i, col);
    return v.grid->cellAt(i - hist, col);
}

std::string visibleText(const TerminalView& v) {
    std::string out;
    for (int row = 0; row < v.rows; ++row) {
        std::string line;
        for (int col = 0; col < v.cols;) {
            term::CellView cell = virtualCell(v, row, col);
            line += utf8(cell.cp);
            col += cell.wide ? 2 : 1;
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        out += '\n';
    }
    return out;
}

namespace {
bool inSel(const TerminalView& v, int row, int col) {
    if (v.selAnchor < 0 || v.selCaret < 0) return false;
    int i = row * v.cols + col;
    int a = std::min(v.selAnchor, v.selCaret), b = std::max(v.selAnchor, v.selCaret);
    return i >= a && i <= b;
}
}  // namespace

bool selActive(const TerminalView& v) { return v.selAnchor >= 0 && v.selCaret >= 0; }

void selClear(TerminalView& v) {
    v.selAnchor = v.selCaret = -1;
}

void selStep(TerminalView& v, int n) {
    if (v.selAnchor < 0) return;  // 起点由调用方用光标位置设定
    int last = v.rows * v.cols - 1;
    v.selCaret = std::clamp(v.selCaret + n, 0, last);
}

std::string selectedText(const TerminalView& v) {
    if (!selActive(v)) return {};
    int a = std::min(v.selAnchor, v.selCaret), b = std::max(v.selAnchor, v.selCaret);
    std::string out;
    for (int i = a; i <= b; ++i) {
        int row = i / v.cols, col = i % v.cols;
        term::CellView cell = virtualCell(v, row, col);
        out += utf8(cell.cp);
        if (cell.wide) ++i;  // 宽字符占两格，延续格不再取
    }
    return out;
}

void drawTerminal(Renderer& r, const FontBook& f, const TerminalView& v) {
    r.fill({kTermX, kContentY, kTermW, kContentH}, c(col::kTerm));
    r.fill({kTermX, kContentY, 1, kContentH}, c(col::kLine));
    r.fill({kTermX + kTermW - 1, kContentY, 1, kContentH}, c(col::kLine));
    if (!v.grid) return;

    int baselineOff = (v.cellH + f.term->ascent - f.term->descent) / 2;
    // 虚拟行号 → 数据来源：先滚动历史，再 vterm 屏幕
    const int hist = v.grid->historySize();
    const int sc = std::clamp(v.scroll, 0, hist);
    const auto cellOf = [&v](int vr, int col) {
        term::CellView cell = virtualCell(v, vr, col);
        if (inSel(v, vr, col)) {  // 选区反色
            uint32_t t = cell.fg;
            cell.fg = cell.bg;
            cell.bg = t;
        }
        return cell;
    };
    for (int row = 0; row < v.rows; ++row) {
        int y = kContentY + kTermPadTop + row * v.cellH;
        // ① 先铺这一行所有底色。宽字符占两格，必须两格一起铺：
        //    否则后一格的底色会把前一个字右半边盖掉（fzf 选中行就是这毛病）。
        for (int col = 0; col < v.cols;) {
            term::CellView cell = cellOf(row, col);
            int w = cell.wide ? 2 : 1;
            if (cell.bg != col::kTerm) {
                int x = kTermX + kTermPadX + col * v.cellW;
                r.fill({x, y, v.cellW * w, v.cellH}, c(cell.bg));
            }
            col += w;
        }
        // ② 再画字。底色已经铺完，谁也盖不掉谁。
        for (int col = 0; col < v.cols;) {
            term::CellView cell = cellOf(row, col);
            int x = kTermX + kTermPadX + col * v.cellW;
            int w = cell.wide ? 2 : 1;
            if (cell.cp != ' ' && cell.cp != 0)
                r.textBaseline(x, y + baselineOff, utf8(cell.cp), f.term, c(cell.fg));
            if (cell.underline) r.fill({x, y + v.cellH - 3, v.cellW * w, 1}, c(cell.fg));
            col += w;
        }
    }

    int cr = 0, cc = 0;
    v.grid->cursor(cr, cc);
    int curX = kTermX + kTermPadX + cc * v.cellW;
    int curY = kContentY + kTermPadTop + cr * v.cellH;

    // 组字串（系统 IME 的 preedit）画在光标处：蓝字 + 下划线 + 2px 光标
    if (!v.preedit.empty() && sc == 0 && cr >= 0 && cr < v.rows) {
        int x = curX;
        size_t i = 0;
        while (i < v.preedit.size()) {
            unsigned char b = v.preedit[i];
            int n = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
            std::string ch = v.preedit.substr(i, size_t(n));
            i += size_t(n);
            int w = (b < 0x80) ? 1 : 2;  // 中文占两格
            if (x + w * v.cellW > kTermX + kTermW) break;
            r.fill({x, curY, w * v.cellW, v.cellH}, c(col::kTerm));  // 盖掉底下的字符
            r.textBaseline(x, curY + baselineOff, ch, f.term, c(col::kBlue));
            r.fill({x, curY + v.cellH - 3, w * v.cellW, 1}, c(col::kBlue));
            x += w * v.cellW;
        }
        r.fill({x, curY + 2, 2, v.cellH - 4}, c(col::kBlue));
    }

    if (v.preedit.empty() && sc == 0 && v.grid->cursorVisible() && cr >= 0 && cr < v.rows &&
        cc >= 0 && cc < v.cols) {
        term::CellView cell = v.grid->cellAt(cr, cc);
        r.fill({curX, curY + v.cellH - 2, v.cellW, 2}, c(cell.fg));
    }

    if (sc > 0) {  // 回看状态：占掉底部提示行，告诉用户现在在哪
        int sy = kContentY + kTermPadTop + v.rows * v.cellH;
        r.fill({kTermX + kTermPadX, sy, kTermW - 2 * kTermPadX, v.cellH}, c(col::kTerm));
        std::string t = "SCROLL " + std::to_string(sc) + "/" + std::to_string(hist) +
                        "  Shift+PgDn 回到底部";
        r.textBaseline(kTermX + kTermPadX, sy + baselineOff, t, f.term, c(col::kDim));
    }
    if (v.preedit.empty()) return;

    // IME 区固定在终端区底部两行（PTY 只占上面 28 行），永远在输入行下方
    int imeY = kContentY + kTermPadTop + kTermRows * v.cellH;
    r.fill({kTermX + kTermPadX, imeY, kTermW - 2 * kTermPadX, v.cellH * kImeRows}, c(col::kTerm));

    int x = kTermX + kTermPadX;
    int first = v.candPage * v.candPageSize;
    int last = std::min(first + v.candPageSize, int(v.cands.size()));
    if (v.cands.empty()) {  // 没候选也要让提示行在，别整块消失
        r.textBaseline(x, imeY + baselineOff, "（无候选）", f.term, c(col::kFaint));
    }
    for (int i = first; i < last; ++i) {
        const ime::Cand& cd = v.cands[size_t(i)];
        bool active = (i == first);
        std::string num = std::to_string(i - first + 1);
        int wNum = r.textWidth(num, f.term);
        int wTxt = r.textWidth(cd.text, f.term);
        int wAnn = cd.annot.empty() ? 0 : 6 + r.textWidth(cd.annot, f.term);
        if (active)  // 选中项反色
            r.fill({x - 2, imeY, wNum + 4 + wTxt + wAnn + 2, v.cellH}, c(col::kBlue));
        int by = imeY + baselineOff;
        r.textBaseline(x, by, num, f.term, c(active ? 0x000000 : col::kDim));
        x += wNum + 4;
        r.textBaseline(x, by, cd.text, f.term, c(active ? 0x000000 : col::kText));
        x += wTxt;
        if (!cd.annot.empty()) {  // 注音（带声调）
            x += 6;
            r.textBaseline(x, by, cd.annot, f.term, c(active ? 0x103040 : col::kDim));
            x += r.textWidth(cd.annot, f.term);
        }
        x += 12;
    }

    int my = imeY + v.cellH;
    int mx = kTermX + kTermPadX;
    auto hint = [&](const char* t, uint32_t cc) {
        r.textBaseline(mx, my + baselineOff, t, f.term, c(cc));
        mx += r.textWidth(t, f.term);
    };
    hint("Tab 翻页 · Space 上屏 · 1-7 选字 · Esc 取消", col::kFaint);
    if (v.candPageCount > 0) {
        std::string pg = "  " + std::to_string(v.candPage + 1) + "/" + std::to_string(v.candPageCount);
        hint(pg.c_str(), col::kDim);
    }
}

}  // namespace ui
