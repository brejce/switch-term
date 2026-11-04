#include "ui/screen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>


namespace ui {
namespace {

int S(int v, float s) { return int(std::lround(float(v) * s)); }

// 「+」格是正方形：tabRect 只按下标算，不知道末位是「+」，宽在这里改成条带高
disp::Rect tabPlusRect(const Geometry& g, const cfg::Ui& uic, int count) {
    disp::Rect box = tabRect(g, uic, count);
    box.w = box.h;
    return box;
}

void ptitle(render::Renderer& r, const FontBook& f, int x, int y, const std::string& text,
            uint32_t color, float s) {
    r.textInBox({x, y, 0, S(12, s)}, text, f.ptitle, c(color), render::Align::Left, 0, 0,
                S(1, s));
}

}  // namespace

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
    put(1, 0, "Tab", col::kGreen, st.pressedKey == cfg::kDpadUp);
    put(1, 1, "·", col::kFaint, false);
    put(0, 1, "Ctrl", col::kPurple, st.modCtrl || st.pressedKey == cfg::kDpadLeft);
    put(2, 1, "Shift", col::kYellow, st.modShift || st.pressedKey == cfg::kDpadRight);
    put(1, 2, "Alt", col::kBlue, st.modAlt || st.pressedKey == cfg::kDpadDown);
}

void drawAbxy(render::Renderer& r, const FontBook& f, int x0, int y0, int cell, float s,
              int pressedKey) {
    auto put = [&](int col, int row, const char* label, uint32_t color, int key) {
        disp::Rect box{x0 + col * cell, y0 + row * cell, cell, cell};
        if (key == pressedKey) {
            r.fill(box, c(col::kText));
            r.textInBox(box, label, f.stick, c(col::kBg), render::Align::Center);
            return;
        }
        r.textInBox(box, label, f.stick, c(color), render::Align::Center);
    };
    put(1, 0, "Y", col::kYellow, cfg::kY);
    put(0, 1, "X", col::kBlue, cfg::kX);
    put(2, 1, "A", col::kGreen, cfg::kA);
    put(1, 2, "B", col::kRed, cfg::kB);
}

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


disp::Rect tabRect(const Geometry& g, const cfg::Ui& uic, int idx) {
    (void)uic;  // 几何全是固定常量 + 缩放，标题宽度不参与（见下面的 shortcut）
    const float s = g.scale;
    const int h = std::max(1, g.topBarH - S(8, s));
    // shortcut: 起始 x 是常量 180，标题变长到压住 tab 时改成量 textWidth
    return {S(180, s) + idx * (S(96, s) + S(4, s)), S(4, s), S(96, s), h};
}

int tabAt(const Geometry& g, const cfg::Ui& uic, int count, int x, int y) {
    if (count <= 0 || x < 0 || y < 0) return -1;
    for (int i = 0; i < count; ++i) {
        disp::Rect box = tabRect(g, uic, i);
        if (x >= box.x && x < box.right() && y >= box.y && y < box.bottom()) return i;
    }
    disp::Rect plus = tabPlusRect(g, uic, count);
    if (x >= plus.x && x < plus.right() && y >= plus.y && y < plus.bottom()) return count;
    return -1;
}

void drawTabBar(render::Renderer& r, const FontBook& f, const Geometry& g, const cfg::Ui& uic,
                const std::vector<std::string>& titles, int active) {
    const float s = g.scale;
    for (int i = 0; i < int(titles.size()); ++i) {
        disp::Rect box = tabRect(g, uic, i);
        const bool on = i == active;
        r.fill(box, c(on ? col::kLine : col::kTerm));
        r.textInBox(box, titles[size_t(i)], f.bar, c(on ? col::kCyan : col::kDim),
                    render::Align::Center, 0, 0, S(1, s));
        // 活动 tab 底部一条线：底色用的是 kLine，不加标记分不清哪个是当前的
        if (on) r.fill({box.x, box.bottom() - S(2, s), box.w, S(2, s)}, c(col::kCyan));
    }
    r.textInBox(tabPlusRect(g, uic, int(titles.size())), "+", f.bar, c(col::kFaint),
                render::Align::Center, 0, 0, S(1, s));
}

void drawTopBar(render::Renderer& r, const FontBook& f, const cfg::Ui& uic, const Geometry& g,
                const AppState& st) {
    const float s = g.scale;
    r.fill({0, 0, g.screenW, g.topBarH}, c(col::kBg));
    r.fill({0, g.topBarH - 1, g.screenW, 1}, c(col::kLine));
    r.textInBox({S(16, s), 0, 0, g.topBarH}, uic.title, f.bar, c(col::kCyan), render::Align::Left, 0,
                0, S(1, s));

    char right[64];
    if (st.battery < 0) std::snprintf(right, sizeof right, "BAT --%%");
    else std::snprintf(right, sizeof right, "BAT %d%%%s", st.battery, st.charging ? "+" : "");
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
    item("LAYER", st.layerName, st.layer == 0 ? col::kCyan : col::kPurple);
    item("LANG", st.lang, st.lang == "ZH" ? col::kYellow : col::kGreen);
    item("IME", st.imeName, col::kDim);

    if (!st.modsText.empty()) {
        std::string t = "MODS [" + st.modsText + "]";
        int w = r.textWidth(t, f.footer);
        r.textInBox({g.screenW - S(16, s) - w, barY, 0, g.botBarH}, t, f.footer, c(col::kYellow),
                    render::Align::Left);
    }
}

disp::Rect keyWindowRect(const Geometry& g, const cfg::Ui& uic, int panelX, int panelY,
                         int panelW, int panelH) {
    const float s = g.scale;
    // 键距只在 ui.keys 里（同一只手内等距）：第 1 个键 = 右邻，第 5 个 = 下邻
    const int pitchX = S(uic.keysV2[1].x - uic.keysV2[0].x, s);
    const int pitchY = S(uic.keysV2[5].y - uic.keysV2[0].y, s);
    const int w = (uic.keyWindowCols - 1) * pitchX + S(uic.keySize, s);
    const int h = (uic.keyWindowRows - 1) * pitchY + S(uic.keySize, s);
    return {panelX + (panelW - w) / 2, panelY + (panelH - h) / 2, w, h};
}

void drawKeyHand(render::Renderer& r, const FontBook& f, const cfg::Ui& uic, const Geometry& g,
                 int layer, int first, int panelX, int panelY, int panelW, int panelH,
                 float ptrX, float ptrY, int snap, int hover, float attractRadius) {
    const float s = g.scale;
    const int L = ((layer % cfg::kLayerCount) + cfg::kLayerCount) % cfg::kLayerCount;
    const int sub = (L + 1) % cfg::kLayerCount;  // 副字符 = 下一层同位置（LAYOUT-v2 §3）
    const int side = S(uic.keySize, s);          // 键帽边长来自 ui.key_size
    const int b = std::max(1, S(1, s));          // 1px 边框
    const int mainH = S(26, s), subH = S(13, s), gap = S(4, s);
    const disp::Rect win = keyWindowRect(g, uic, panelX, panelY, panelW, panelH);
    const int cx = win.x + win.w / 2, cy = win.y + win.h / 2;  // 面板中心 = 指针位置
    auto px = [s](float v) { return int(std::lround(v * s)); };
    const disp::Color acc = c(first == 0 ? col::kCyan : col::kRed);  // 左手青 / 右手红
    r.setClip(win);  // 窗外的键画不出去，终端区不会被脏到
    for (int i = 0; i < cfg::kKeyV2Count / 2; ++i) {
        const int k = first + i;
        const bool isSnap = snap == k;
        const bool isHover = !isSnap && hover == k;  // 吸附优先：吸附键不再描边
        // 吸附动效：亮度 = 物理场的真实受力（同一半径、同一距离），所以往右推时 f 先亮、
        // 再往右 g 才亮；松手回零位时亮度跟着退回 d。吸附键已反色，不再叠加。
        const float kcx = float(uic.keysV2[k].x) + float(uic.keySize) * 0.5f;
        const float kcy = float(uic.keysV2[k].y) + float(uic.keySize) * 0.5f;
        const float pull =
            isSnap || attractRadius <= 0.f
                ? 0.f
                : std::clamp(1.f - std::hypot(kcx - ptrX, kcy - ptrY) / attractRadius, 0.f, 1.f);
        disp::Color edge = c(isSnap || isHover ? col::kText : col::kLine);
        disp::Color bg = c(isSnap ? col::kText : col::kKkBg);
        disp::Color mainCol = c(isSnap ? col::kBg : col::kText);
        disp::Color subCol = c(isSnap ? col::kBg : col::kDim);
        if (pull > 0.f) {  // 边框先亮 → 底色跟着亮 → 过一半再开始染字：三段手感看得出来
            edge = disp::mix(edge, acc, pull);
            bg = disp::mix(bg, acc, pull * 0.25f);
            mainCol = disp::mix(mainCol, acc, std::max(0.f, pull * 2.f - 1.f));
        }
        // 偏移 = 面板中心 - 指针（世界坐标）：吸附住的键正好压在中心上
        const int kx = cx + px(kcx - ptrX);
        const int ky = cy + px(kcy - ptrY);
        const disp::Rect box{kx - side / 2, ky - side / 2, side, side};
        // 吸附 = 整键反色（底色 col::kText、字色 col::kBg）；悬停（无吸附时）= 亮色描边
        r.fill(box, edge);
        r.fill({box.x + b, box.y + b, box.w - 2 * b, box.h - 2 * b}, bg);
        // 主字符在上（大/亮），副字符在下（小/淡）：整块字在键帽里竖向居中
        const int ty = box.y + (side - mainH - gap - subH) / 2;
        r.textInBox({box.x, ty, box.w, mainH}, uic.layers[L][k], f.keyMain, mainCol,
                    render::Align::Center);
        r.textInBox({box.x, ty + mainH + gap, box.w, subH}, uic.layers[sub][k], f.keySub, subCol,
                    render::Align::Center);
    }
    r.clearClip();
}

void drawKeyPanels(render::Renderer& r, const FontBook& f, const cfg::Ui& uic, const Geometry& g,
                   const AppState& st) {
    const float s = g.scale;
    const int rightX = g.termX + g.termW;
    r.fill({0, g.contentY, g.panelW, g.contentH}, c(col::kPanel));
    r.fill({rightX, g.contentY, g.panelW, g.contentH}, c(col::kPanel));
    drawKeyHand(r, f, uic, g, st.layer, 0, 0, g.contentY, g.panelW, g.contentH, st.ptrLeftX,
                st.ptrLeftY, st.snapLeft, st.hoverLeft, st.attractRadius);
    drawKeyHand(r, f, uic, g, st.layer, 15, rightX, g.contentY, g.panelW, g.contentH,
                st.ptrRightX, st.ptrRightY, st.snapRight, st.hoverRight, st.attractRadius);
    // 竖分隔线画在键之后：滚到窗边的键不该盖住面板边界
    r.fill({g.panelW - 1, g.contentY, 1, g.contentH}, c(col::kLine));
    r.fill({rightX, g.contentY, 1, g.contentH}, c(col::kLine));

    // 按键簇摆在键窗下面：窗口在面板里竖向居中，所以从窗口底边往下排
    const disp::Rect win = keyWindowRect(g, uic, 0, g.contentY, g.panelW, g.contentH);
    const int dy = win.bottom() + S(20, s);
    const int padX = S(10, s);
    ptitle(r, f, padX, dy, uic.dpadTitle, col::kCyan, s);
    drawDpad(r, f, (g.panelW - 3 * g.dpadCell) / 2, dy + S(16, s), g.dpadCell, st, s);
    ptitle(r, f, rightX + padX, dy, uic.abxyTitle, col::kRed, s);
    drawAbxy(r, f, rightX + (g.panelW - 3 * g.abxyCell) / 2, dy + S(16, s), g.abxyCell, s,
             st.pressedKey);
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
        bool active = (i == size_t(st.candSel));
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
