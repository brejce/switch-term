#include "ui/panels.h"
#include "ui/char_grid.h"
#include "ui/layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace ui::layout;
using render::Renderer;

namespace ui {
namespace {

// 面板里剩下这些按设计尺寸写的固定像素，也要跟着缩放，否则放大后区域重叠
inline int S(int v) { return int(std::lround(float(v) * layout::kScale)); }

void buildDefaultKeys() {
    const char* la[13] = {"e", "i", "h", "a", "o", "n", "u", "g", "z", "m", "t", "l", "r"};
    const char* ra[13] = {"q", "s", "x", "d", "b", "y", "c", "j", "v", "k", "w", "p", "f"};
    const char* ls[13] = {"；", "，", "：", "？", "！", "。", "、", "“", "”",
                          "（", "）", "—", "…"};  // IMPLEMENTATION §14.3
    const char* rn[13] = {"7", "8", "9", "4", "5", "6", "1", "2", "3", "+", "−", "×", "÷"};
    auto fill = [](StickSpec& s, const char** src) {
        for (int i = 0; i < 9; ++i) s.cells[i] = src[i];
        for (int i = 0; i < 4; ++i) s.outers[i] = src[9 + i];
    };
    fill(keys::leftAlpha, la);
    fill(keys::rightAlpha, ra);
    fill(keys::leftSymbol, ls);
    fill(keys::rightNum, rn);
}


struct Pos {
    int row, col;
};
// 5×5 网格：第一层占 2..4 行列的九宫格，第二层占外围四个正方向
constexpr Pos kInner[9] = {{1, 1}, {1, 2}, {1, 3}, {2, 1}, {2, 2}, {2, 3}, {3, 1}, {3, 2}, {3, 3}};
constexpr Pos kOuter[4] = {{0, 2}, {2, 4}, {4, 2}, {2, 0}};

// 肩键块：24px 高、左右各 11px 内边距（原来 18 高太小，点不中也没手感）
int shoulder(Renderer& r, const FontBook& f, int x, int y, const char* label, uint32_t textC,
             uint32_t bgC, std::vector<HitCell>* hits, const char* bytes, int action) {
    int w = r.textWidth(label, f.shoulder) + S(22);
    disp::Rect box{x, y, w, S(24)};
    r.fill(box, c(bgC));
    r.textInBox(box, label, f.shoulder, c(textC), render::Align::Center, 0, 0, 1);
    if (hits) hits->push_back({box, bytes ? std::string(bytes) : std::string(), action});
    return w;
}

void ptitle(Renderer& r, const FontBook& f, int x, int y, const char* text, uint32_t color) {
    r.textInBox({x, y, 0, S(12)}, text, f.ptitle, c(color), render::Align::Left, 0, 0, 1);
}

void drawStick(Renderer& r, const FontBook& f, int x0, int y0, const StickSpec& s,
               uint32_t accent, std::vector<HitCell>* hits) {
    for (int i = 0; i < 9; ++i) {
        bool center = (i == 4);
        disp::Rect box = cellBox(x0, y0, kStickCell, kStickCell, kInner[i].col, kInner[i].row);
        if (center) {  // 中心反色：填功能色 + 底色字
            r.fill(box, c(accent));
            r.textInBox(box, s.cells[i].c_str(), f.stick, c(0x000000), render::Align::Center);
        } else {
            r.textInBox(box, s.cells[i].c_str(), f.stick, c(accent), render::Align::Center);
        }
        if (hits) hits->push_back({box, s.cells[i]});
    }
    for (int i = 0; i < 4; ++i) {  // 第二层默认极暗
        disp::Rect box = cellBox(x0, y0, kStickCell, kStickCell, kOuter[i].col, kOuter[i].row);
        r.textInBox(box, s.outers[i].c_str(), f.stick2, c(col::kFaint), render::Align::Center);
        if (hits) hits->push_back({box, s.outers[i]});
    }
}

void drawDpad(Renderer& r, const FontBook& f, int x0, int y0, int mods,
              std::vector<HitCell>* hits) {
    const int c0 = kDpadCell;
    auto put = [&](int col, int row, const char* s, uint32_t color, int lockBit, int dblAct,
                   int tapAct) {
        disp::Rect box = cellBox(x0, y0, c0, c0, col, row);
        bool on = lockBit && (mods & lockBit);
        if (on) {  // 锁定 = 反色，一眼看得出
            r.fill(box, c(color));
            r.textInBox(box, s, f.dpad, c(col::kBg), render::Align::Center);
        } else {
            r.textInBox(box, s, f.dpad, c(color), render::Align::Center);
        }
        if (hits && (dblAct || tapAct)) hits->push_back({box, std::string(), tapAct, dblAct});
    };
    // Tab 不是修饰键；三个修饰键：点一下 = 只作用于下一个键，双击 = 锁定
    put(1, 0, keys::dpadTab.c_str(), col::kGreen, 0, kActNone, kActNone);
    put(0, 1, keys::dpadCtrl.c_str(), col::kPurple, 1, kActLockCtrl, kActStickyCtrl);
    put(1, 1, "·", col::kFaint, 0, kActNone, kActNone);
    put(2, 1, keys::dpadShift.c_str(), col::kYellow, 2, kActLockShift, kActStickyShift);
    put(1, 2, keys::dpadAlt.c_str(), col::kBlue, 4, kActLockAlt, kActStickyAlt);
}

void drawAbxy(Renderer& r, const FontBook& f, int x0, int y0, std::vector<HitCell>* hits) {
    const int c0 = kAbxyCell;
    auto put = [&](int col, int row, const char* s, uint32_t color, const char* bytes) {
        disp::Rect box = cellBox(x0, y0, c0, c0, col, row);
        r.textInBox(box, s, f.abxy, c(color), render::Align::Center);
        if (hits && bytes) hits->push_back({box, bytes});
    };
    if (hits)  // Y = ESC，也是 UI 动作（点它等于按 Y）
        hits->push_back({cellBox(x0, y0, c0, c0, 1, 0), "\x1b", kActNone});
    r.textInBox(cellBox(x0, y0, c0, c0, 1, 0), "Y", f.abxy, c(col::kYellow),
                render::Align::Center);
    put(0, 1, keys::abxyX.c_str(), col::kBlue, "\x1b[3~");
    put(2, 1, keys::abxyA.c_str(), col::kGreen, "\r");
    put(1, 2, keys::abxyB.c_str(), col::kRed, "\x7f");
}

struct KRow {
    const char* key;
    uint32_t keyColor;
    const char* bold;
    const char* rest;
    int action = kActNone;  // 非 0 = 整行可点（面板上不占额外按钮）
};

// 底部功能说明：4 行，从面板底往上贴
void drawKList(Renderer& r, const FontBook& f, int panelX, const KRow* rows, int n,
               std::vector<HitCell>* hits) {
    const int rowH = S(19), keyH = S(16);
    int y = kContentY + kContentH - kPanelPadBot - (n * keyH + (n - 1) * 3);
    for (int i = 0; i < n; ++i) {
        const KRow& row = rows[i];
        int kw = std::max(S(24), r.textWidth(row.key, f.kk) + S(8));
        disp::Rect kb{panelX + kPanelPadX, y, kw, keyH};
        // .kk 变体各自有底色（ui-mockup.html .kk.{cyan,purple,green}）
        uint32_t bg = row.keyColor == col::kGreen   ? 0x0e1e12
                      : row.keyColor == col::kCyan  ? 0x0e1e26
                      : row.keyColor == col::kPurple ? 0x1a1226
                                                     : col::kKkBg;
        r.fill(kb, c(bg));
        r.textInBox(kb, row.key, f.kk, c(row.keyColor), render::Align::Center, 0, 0, 1);

        int x = kb.right() + S(6);
        int baseline = y + keyH / 2 + (f.kd->ascent - f.kd->descent) / 2;
        r.textBaseline(x, baseline, row.bold, f.kk, c(col::kText));
        x += r.textWidth(row.bold, f.kk);
        r.textBaseline(x, baseline, row.rest, f.kd, c(col::kDim));
        if (hits && row.action)
            hits->push_back({disp::Rect{panelX + kPanelPadX, y, kPanelW - 2 * kPanelPadX, keyH},
                             std::string(), row.action});
        y += rowH;
    }
}

void panelBase(Renderer& r, int panelX, bool left) {
    r.fill({panelX, kContentY, kPanelW, kContentH}, c(col::kPanel));
    r.fill({left ? panelX + kPanelW - 1 : panelX, kContentY, 1, kContentH}, c(col::kLine));
}

// 面板上半部分：肩键 → 标题 → 摇杆 → 标题 → 键块
int panelHead(Renderer& r, const FontBook& f, int panelX, bool left, const StickSpec& spec,
              const StickSpec& spec2, bool layer2, uint32_t accent, const char* t1,
              const char* t2, std::vector<HitCell>* hits) {
    int y = kContentY + kPanelPadTop;
    int xr = panelX + kPanelW - kPanelPadX;
    if (left) {
        int w1 = shoulder(r, f, panelX + kPanelPadX, y, "L", col::kPurple, col::kShL, hits,
                          nullptr, kActLayer);
        shoulder(r, f, panelX + kPanelPadX + w1 + 6, y, "ZL", col::kShZLText, col::kShZL, hits,
                 nullptr, kActCursorLeft);
    } else {
        int wR = r.textWidth("R", f.shoulder) + S(22);    // 宽度随文字算，别写死偏移（会重叠）
        int wZR = r.textWidth("ZR", f.shoulder) + S(22);
        shoulder(r, f, xr - wZR - wR - S(6), y, "R", col::kCyan, col::kShR, hits, " ", kActNone);
        shoulder(r, f, xr - wZR, y, "ZR", col::kShZRText, col::kShZR, hits, nullptr,
                 kActCursorRight);
    }
    // 与 DOM 对齐：肩键 44..62（+2 margin）→ 标题 72 → 摇杆 100 → 标题 296 → 键块 318
    y = S(72);
    ptitle(r, f, panelX + kPanelPadX, y, t1, left ? col::kCyan : col::kRed);
    drawStick(r, f, panelX + (kPanelW - kStickBox) / 2, S(100), layer2 ? spec2 : spec, accent,
              hits);
    ptitle(r, f, panelX + kPanelPadX, S(296), t2, left ? col::kCyan : col::kRed);
    return S(318);
}

}  // namespace

void drawTopBar(Renderer& r, const FontBook& f, const UiState& st) {
    r.fill({0, 0, kScreenW, kTopBarH}, c(col::kBg));
    r.fill({0, kTopBarH - 1, kScreenW, 1}, c(col::kLine));
    r.textInBox({16, 0, 0, kTopBarH}, "SYS.TERM", f.bar, c(col::kCyan), render::Align::Left, 0, 0,
                3);

    char buf[64];
    std::snprintf(buf, sizeof buf, "BAT %d%%", st.battery);
    int bw = r.textWidth(buf, f.bar);
    int tx = kScreenW - 16 - bw;
    r.textInBox({tx, 0, 0, kTopBarH}, buf, f.bar, c(col::kGreen), render::Align::Left);
    tx -= 8 + r.textWidth("·", f.bar) + 8;
    r.textInBox({tx, 0, 0, kTopBarH}, "·", f.bar, c(col::kFaint), render::Align::Left);
    tx -= r.textWidth(st.clock.c_str(), f.bar) + 8;
    r.textInBox({tx, 0, 0, kTopBarH}, st.clock.c_str(), f.bar, c(col::kText), render::Align::Left);
}

void drawBottomBar(Renderer& r, const FontBook& f, const UiState& st) {
    int y = kScreenH - kBotBarH;
    r.fill({0, y, kScreenW, kBotBarH}, c(col::kBg));
    r.fill({0, y, kScreenW, 1}, c(col::kLine));

    int x = 16;
    auto item = [&](const char* label, const char* value, uint32_t valueC) {
        x = putText(r, x, y, kBotBarH, f.footer, label, col::kFaint, 1);  // letter-spacing .5
        x = putText(r, x + 4, y, kBotBarH, f.footer, value, valueC, 1) + 14;
    };
    item("LAYER", st.layer.c_str(), st.layer == "ALPHA" ? col::kCyan : col::kPurple);
    item("LANG", st.lang.c_str(), st.lang == "ZH" ? col::kYellow : col::kGreen);
    item("IME", st.ime.c_str(), col::kDim);
    item("INPUT", st.input.c_str(), col::kDim);

    char right[64];
    std::snprintf(right, sizeof right, "SYS.TERM %s", st.version.c_str());
    int rw = r.textWidth(right, f.footer);
    r.textInBox({kScreenW - 16 - rw, y, 0, kBotBarH}, right, f.footer, c(col::kFaint),
                render::Align::Left);
    int capsX = kScreenW - 16 - rw - 14 - r.textWidth("CAPS —", f.footer);
    r.textInBox({capsX, y, 0, kBotBarH}, st.caps ? "CAPS ON" : "CAPS —", f.footer,
                c(st.caps ? col::kYellow : col::kFaint), render::Align::Left);
}

void drawLeftPanel(Renderer& r, const FontBook& f, int panelX, const UiState& st,
                   std::vector<HitCell>* hits) {
    panelBase(r, panelX, true);
    int y = panelHead(r, f, panelX, true, keys::leftAlpha, keys::leftSymbol, st.layer == "SYMBOL",
                      col::kCyan, "// LEFT_STICK", "// DPAD", hits);
    drawDpad(r, f, panelX + (kPanelW - 3 * kDpadCell) / 2, y, st.locks | st.sticky, hits);

    static const KRow rows[] = {
        {"L", col::kPurple, "切换", " 字母/符号"},
        {"Tab", col::kGreen, "翻页", " 候选"},
        {"ZL", col::kPurple, "光标/选区", " 左移 ·锁Shift"},
        {"L3", col::kDim, "", "·DPAD 双击锁定"},
        {"CP", col::kCyan, "复制", " 可见区", kActCopy},
        {"PS", col::kPurple, "粘贴", " 剪贴板", kActPaste},
    };
    drawKList(r, f, panelX, rows, 6, hits);
}

void drawRightPanel(Renderer& r, const FontBook& f, int panelX, const UiState& st,
                    std::vector<HitCell>* hits) {
    panelBase(r, panelX, false);
    int y = panelHead(r, f, panelX, false, keys::rightAlpha, keys::rightNum, st.layer == "SYMBOL", col::kRed,
                      "// RIGHT_STICK", "// ABXY", hits);
    drawAbxy(r, f, panelX + (kPanelW - 3 * kAbxyCell) / 2, y, hits);

    static const KRow rows[] = {
        {"Y", col::kGreen, "ESC", ""},
        {"R", col::kCyan, "空格", " ·锁Ctrl=中英"},
        {"ZR", col::kCyan, "光标/选区", " 右移 ·锁Shift"},
        {"R3", col::kDim, "", "按下摇杆 · 第二层"},
    };
    drawKList(r, f, panelX, rows, 4, hits);
}

namespace keys {
void resetDefaults() { buildDefaultKeys(); }
}  // namespace keys

}  // namespace ui
