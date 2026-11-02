#include "ui/screen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui/t9_popup.h"

namespace ui {
namespace {

int S(int v, float s) { return int(std::lround(float(v) * s)); }

// 面板里的一行说明（键块 + 说明文字）
struct KeyRow {
    const char* key;
    uint32_t color;
    const char* bold;
    const char* rest;
};

void drawKeyList(render::Renderer& r, const FontBook& f, int panelX, int panelW, int botY,
                 const KeyRow* rows, int n, float s) {
    const int rowH = S(19, s), keyH = S(18, s);
    int y = botY - (n * keyH + (n - 1) * S(3, s));
    for (int i = 0; i < n; ++i) {
        const KeyRow& row = rows[i];
        int kw = std::max(S(28, s), r.textWidth(row.key, f.kk) + S(8, s));
        disp::Rect kb{panelX, y, kw, keyH};
        r.fill(kb, c(col::kKkBg));
        r.textInBox(kb, row.key, f.kk, c(row.color), render::Align::Center, 0, 0, S(1, s));
        int x = kb.right() + S(6, s);
        int baseline = y + keyH / 2 + (f.kd->ascent - f.kd->descent) / 2;
        r.textBaseline(x, baseline, row.bold, f.kk, c(col::kText));
        x += r.textWidth(row.bold, f.kk);
        r.textBaseline(x, baseline, row.rest, f.kd, c(col::kDim));
        y += rowH;
    }
}

int shoulder(render::Renderer& r, const FontBook& f, int x, int y, const char* label,
             uint32_t textC, uint32_t bgC, float s) {
    int w = r.textWidth(label, f.shoulder) + S(16, s);
    disp::Rect box{x, y, w, S(20, s)};
    r.fill(box, c(bgC));
    r.textInBox(box, label, f.shoulder, c(textC), render::Align::Center, 0, 0, S(1, s));
    return w;
}

void ptitle(render::Renderer& r, const FontBook& f, int x, int y, const std::string& text,
            uint32_t color, float s) {
    r.textInBox({x, y, 0, S(12, s)}, text, f.ptitle, c(color), render::Align::Left, 0, 0,
                S(1, s));
}

// 9 张字符格：row-major 九宫格。弹窗时字符键按 t9_popup.h 的占位表铺开，
// 锁定格自己承载的那个字符就是默认选中项，没被字符键占的格子变暗。
void drawT9Grid(render::Renderer& r, const FontBook& f, const cfg::T9Key* keys, const Geometry& g,
                const AppState& st, uint32_t accent, float s) {
    const int lock = st.popup ? (st.popupFocus >= 0 ? st.popupFocus : st.focusIdx) : -1;
    // 弹窗时每格上的字符（0 = 这格没有字符键）；符号层字符数少于占位数时按 letters 截断
    char letterAt[9] = {};
    if (lock >= 0) {
        const std::string& letters = keys[lock].letters;
        int n = std::min(popupCellCount(lock), int(letters.size()));
        for (int k = 0; k < n; ++k) letterAt[popupCellFor(lock, k)] = letters[size_t(k)];
    }
    const int sel = st.selCell >= 0 ? st.selCell : lock;  // 默认选中锁定格上的字符
    for (int i = 0; i < 9; ++i) {
        disp::Rect box = t9CellRect(g, i);
        const int x = box.x, y = box.y, cell = box.w;
        const bool isChar = letterAt[i] != 0;
        uint32_t bg = 0;
        if (st.popup) {
            if (isChar && i == sel) bg = col::kBlue;             // 悬停/选中：反色底
            if (isChar && i == st.pressedCell) bg = col::kText;  // 刚触发：闪一下
        } else if (i == st.focusIdx) {
            bg = accent;  // 网格态焦点格
        }
        uint32_t fg = (st.popup && !isChar) ? col::kFaint : accent;
        if (bg) {
            r.fill({x + S(2, s), y + S(2, s), cell - S(4, s), cell - S(4, s)}, c(bg));
            fg = col::kBg;  // 底色是反色，字用底色
        }
        // 数字（左上，小字）
        r.textInBox({x + S(4, s), y + S(2, s), 0, S(12, s)}, keys[i].num, f.dpad,
                    c(bg ? (st.popup ? 0x001018 : 0x101018) : col::kDim), render::Align::Left);
        // 字母（居中，大字）；弹窗时字符键一格一个字母
        std::string letters = isChar ? std::string(1, letterAt[i]) : keys[i].letters;
        r.textInBox({x, y + S(10, s), cell, S(30, s)}, letters, f.stick2, c(fg),
                    render::Align::Center, 0, 0, S(1, s));
    }
}

void drawDpad(render::Renderer& r, const FontBook& f, int x0, int y0, int cell,
              const AppState& st, float s) {
    auto put = [&](int col, int row, const char* label, uint32_t color, bool on) {
        disp::Rect box{x0 + col * cell, y0 + row * cell, cell, cell};
        if (on) {
            r.fill(box, c(col::kText));
            r.textInBox(box, label, f.dpad, c(col::kBg), render::Align::Center);
        } else {
            r.textInBox(box, label, f.dpad, c(color), render::Align::Center);
        }
    };
    put(1, 0, "Tab", col::kGreen, false);
    put(1, 1, "·", col::kFaint, false);
    put(0, 1, "Ctrl", col::kPurple, st.modCtrl);
    put(2, 1, "Shift", col::kYellow, st.modShift);
    put(1, 2, "Alt", col::kBlue, st.modAlt);
}

void drawAbxy(render::Renderer& r, const FontBook& f, int x0, int y0, int cell, float s) {
    auto put = [&](int col, int row, const char* label, uint32_t color) {
        r.textInBox({x0 + col * cell, y0 + row * cell, cell, cell}, label, f.stick, c(color),
                    render::Align::Center);
    };
    put(1, 0, "Y", col::kYellow);
    put(0, 1, "X", col::kBlue);
    put(2, 1, "A", col::kGreen);
    put(1, 2, "B", col::kRed);
}

}  // namespace

void Geometry::fromConfig(const cfg::Ui& uic, float s) {
    scale = s;
    screenW = S(1280, s);
    screenH = S(720, s);
    panelW = S(uic.panelW, s);
    topBarH = S(uic.topBarH, s);
    botBarH = S(uic.botBarH, s);
    stickBox = S(uic.stickBox, s);
    keyCell = std::max(1, stickBox / 3);
    dpadCell = S(uic.dpadCell, s);
    abxyCell = S(uic.abxyCell, s);
    shoulderH = S(uic.shoulderH, s);
    termPadX = S(uic.termPadX, s);
    termPadTop = S(uic.termPadTop, s);
    termPadBot = S(uic.termPadBot, s);
    contentY = topBarH;
    contentH = screenH - topBarH - botBarH;
    termX = panelW;
    termW = screenW - 2 * panelW;
    termCellW = std::max(1, S(uic.cellW, s));
    termCellH = std::max(1, S(uic.cellH, s));
    imeRows = std::max(1, S(uic.imeRows, s));
    termCols = std::max(1, (termW - 2 * termPadX) / termCellW);
    // 底部固定留 imeRows 行给组字串/候选
    termRows = std::max(1, (contentH - termPadTop - termPadBot) / termCellH - imeRows);
}

disp::Rect t9CellRect(const Geometry& g, int idx) {
    const int gx = (g.panelW - g.stickBox) / 2;
    const int gy = g.contentY + S(64, g.scale);  // 标题那一行下面
    const int col = idx % 3, row = idx / 3;
    return {gx + col * g.keyCell, gy + row * g.keyCell, g.keyCell, g.keyCell};
}

void drawTopBar(render::Renderer& r, const FontBook& f, const cfg::Ui& uic, const Geometry& g,
                const AppState& st) {
    const float s = g.scale;
    r.fill({0, 0, g.screenW, g.topBarH}, c(col::kBg));
    r.fill({0, g.topBarH - 1, g.screenW, 1}, c(col::kLine));
    r.textInBox({S(16, s), 0, 0, g.topBarH}, uic.title, f.bar, c(col::kCyan), render::Align::Left, 0,
                0, S(1, s));

    char right[64];
    std::snprintf(right, sizeof right, "BAT %d%%", st.battery);
    int w = r.textWidth(right, f.bar);
    int x = g.screenW - S(16, s) - w;
    r.textInBox({x, 0, 0, g.topBarH}, right, f.bar, c(col::kGreen), render::Align::Left);
    x -= S(10, s) + r.textWidth("·", f.bar);
    r.textInBox({x, 0, 0, g.topBarH}, "·", f.bar, c(col::kFaint), render::Align::Left);
    x -= r.textWidth(st.clock, f.bar) + S(10, s);
    r.textInBox({x, 0, 0, g.topBarH}, st.clock, f.bar, c(col::kText), render::Align::Left);
}

void drawBottomBar(render::Renderer& r, const FontBook& f, const Geometry& g,
                   const AppState& st) {
    const float s = g.scale;
    int barY = g.contentY + g.contentH;
    r.fill({0, barY, g.screenW, g.botBarH}, c(col::kBg));
    r.fill({0, barY, g.screenW, 1}, c(col::kLine));

    int x = S(16, s);
    auto item = [&](const char* label, const std::string& value, uint32_t vc) {
        r.textInBox({x, barY, 0, g.botBarH}, label, f.footer, c(col::kFaint), render::Align::Left,
                    0, 0, S(1, s));
        x += r.textWidth(label, f.footer) + S(4, s);
        r.textInBox({x, barY, 0, g.botBarH}, value, f.footer, c(vc), render::Align::Left, 0, 0,
                    S(1, s));
        x += r.textWidth(value, f.footer) + S(14, s);
    };
    item("LAYER", st.layer, st.layer == "ALPHA" ? col::kCyan : col::kPurple);
    item("LANG", st.lang, st.lang == "ZH" ? col::kYellow : col::kGreen);
    item("IME", st.imeName, col::kDim);

    if (!st.modsText.empty()) {
        std::string t = "MODS [" + st.modsText + "]";
        int w = r.textWidth(t, f.footer);
        r.textInBox({g.screenW - S(16, s) - w, barY, 0, g.botBarH}, t, f.footer, c(col::kYellow),
                    render::Align::Left);
    }
}

void drawT9Panel(render::Renderer& r, const FontBook& f, const cfg::Ui& uic, const Geometry& g,
                 const AppState& st) {
    const float s = g.scale;
    int padX = S(10, s);
    r.fill({0, g.contentY, g.panelW, g.contentH}, c(col::kPanel));
    r.fill({g.panelW - 1, g.contentY, 1, g.contentH}, c(col::kLine));

    // 肩键：LB（魔法键）/ LT（空格）
    int sy = g.contentY + S(12, s);
    int wLB = shoulder(r, f, padX, sy, "LB (MAGIC)", col::kPurple,
                       st.popup ? col::kPurple : col::kShZL, s) + S(6, s);
    shoulder(r, f, padX + wLB, sy, "LT (SPACE)", col::kCyan, col::kShR, s);

    // T9 九键
    ptitle(r, f, padX, g.contentY + S(46, s), uic.t9Title, col::kCyan, s);
    int gy = t9CellRect(g, 0).y;
    const cfg::T9Key* keys = (st.layer == "SYMBOL") ? uic.t9Symbol : uic.t9Alpha;
    drawT9Grid(r, f, keys, g, st, col::kCyan, s);

    // DPAD
    int dy = gy + g.stickBox + S(16, s);
    ptitle(r, f, padX, dy, uic.dpadTitle, col::kCyan, s);
    int dx = (g.panelW - 3 * g.dpadCell) / 2;
    drawDpad(r, f, dx, dy + S(16, s), g.dpadCell, st, s);

    // 底部说明
    static const KeyRow rows[] = {
        {"LB", col::kPurple, "魔法键", " 弹出/连击"},
        {"LT", col::kCyan, "空格", ""},
        {"DPAD", col::kDim, "双击", " 锁定 Ctrl/Shift/Alt"},
    };
    drawKeyList(r, f, padX, g.panelW - 2 * padX, g.contentY + g.contentH - S(10, s), rows, 3, s);
}

void drawMousePanel(render::Renderer& r, const FontBook& f, const cfg::Ui& uic, const Geometry& g,
                    const AppState& st) {
    (void)st;
    const float s = g.scale;
    int padX = g.termX + g.termW + S(10, s);
    int panelX = g.termX + g.termW;
    r.fill({panelX, g.contentY, g.panelW, g.contentH}, c(col::kPanel));
    r.fill({panelX, g.contentY, 1, g.contentH}, c(col::kLine));

    int sy = g.contentY + S(12, s);
    int wRB = r.textWidth("RB (L-CLICK)", f.shoulder) + S(16, s);
    int wRT = r.textWidth("RT (R-CLICK)", f.shoulder) + S(16, s);
    int xr = panelX + g.panelW - S(10, s);
    shoulder(r, f, xr - wRT - wRB - S(6, s), sy, "RB (L-CLICK)", col::kGreen, col::kShR, s);
    shoulder(r, f, xr - wRT, sy, "RT (R-CLICK)", col::kRed, col::kShZL, s);

    ptitle(r, f, padX, g.contentY + S(46, s), uic.mouseTitle, col::kRed, s);
    // 鼠标准星（摇杆区中央）
    int gx = panelX + (g.panelW - g.stickBox) / 2;
    int gy = g.contentY + S(64, s);
    r.textInBox({gx, gy, g.stickBox, g.stickBox}, "╬", f.stick, c(col::kRed),
                render::Align::Center);

    int dy = gy + g.stickBox + S(16, s);
    ptitle(r, f, padX, dy, uic.abxyTitle, col::kRed, s);
    int ax = panelX + (g.panelW - 3 * g.abxyCell) / 2;
    drawAbxy(r, f, ax, dy + S(16, s), g.abxyCell, s);

    static const KeyRow rows[] = {
        {"A", col::kGreen, "回车", ""},
        {"B", col::kRed, "退格", ""},
        {"X", col::kBlue, "Del", ""},
        {"Y", col::kYellow, "Esc", ""},
        {"RB", col::kGreen, "鼠标左键", ""},
        {"RT", col::kRed, "鼠标右键", ""},
    };
    drawKeyList(r, f, padX, g.panelW - 2 * S(10, s), g.contentY + g.contentH - S(10, s), rows, 6,
                s);
}

void drawTermBackground(render::Renderer& r, const FontBook& f, const Geometry& g) {
    (void)f;
    r.fill({g.termX, g.contentY, g.termW, g.contentH}, c(col::kTerm));
}

void drawComposeLine(render::Renderer& r, const FontBook& f, const Geometry& g,
                     const AppState& st) {
    const float s = g.scale;
    int cellH = g.termCellH;
    int y = g.contentY + g.contentH - g.imeRows * cellH;  // 底部留给定行数：组字串 + 候选
    r.fill({g.termX + 1, y, g.termW - 2, cellH * g.imeRows}, c(col::kTerm));
    r.fill({g.termX + 1, y, g.termW - 2, 1}, c(col::kLine));

    int baseline = y + cellH / 2 + (f.term->ascent - f.term->descent) / 2;
    int x = g.termX + g.termPadX;
    if (!st.compose.empty()) {  // 组字串：蓝字 + 下划线
        r.textBaseline(x, baseline, st.compose, f.term, c(col::kBlue));
        int w = r.textWidth(st.compose, f.term);
        r.fill({x, y + cellH - S(3, s), w, S(1, s)}, c(col::kBlue));
        x += w + S(8, s);
    }
    // 候选
    y += cellH;
    baseline = y + cellH / 2 + (f.term->ascent - f.term->descent) / 2;
    x = g.termX + g.termPadX;
    for (size_t i = 0; i < st.cands.size() && i < 7; ++i) {
        const CandView& cd = st.cands[i];
        std::string num = std::to_string(i + 1);
        int wn = r.textWidth(num, f.term);
        int wt = r.textWidth(cd.text, f.term);
        int wa = cd.annot.empty() ? 0 : S(6, s) + r.textWidth(cd.annot, f.term);
        if (x + wn + S(4, s) + wt + wa > g.termX + g.termW - g.termPadX) break;
        bool active = (i == 0);
        if (active)
            r.fill({x - S(2, s), y, wn + wt + wa + S(10, s), cellH}, c(col::kBlue));
        r.textBaseline(x, baseline, num, f.term, c(active ? 0x000000 : col::kDim));
        x += wn + S(4, s);
        r.textBaseline(x, baseline, cd.text, f.term, c(active ? 0x000000 : col::kText));
        x += wt;
        if (wa > 0) {  // 注音：带声调的拼音，暗色
            x += S(6, s);
            r.textBaseline(x, baseline, cd.annot, f.term,
                           c(active ? 0x103040 : col::kDim));
            x += r.textWidth(cd.annot, f.term);
        }
        x += S(12, s);
    }
    if (st.candPageCount > 0) {
        char pg[32];
        std::snprintf(pg, sizeof pg, "%d/%d", st.candPage + 1, st.candPageCount);
        int w = r.textWidth(pg, f.term);
        r.textBaseline(g.termX + g.termW - g.termPadX - w, baseline, pg, f.term, c(col::kFaint));
    }
}

}  // namespace ui
