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

// 圆角矩形：renderer 只有 fillRect，所以用「上下各 rad 行逐行内缩 + 中间一整块」拼。
// 每帧每键约 (2×rad + 1) 次 fill，30 键 ≈ 1000 次，软光栅足够（不是逐像素）。
void fillRound(render::Renderer& r, disp::Rect b, int rad, disp::Color col) {
    if (b.w <= 0 || b.h <= 0) return;
    rad = std::clamp(rad, 0, std::min(b.w, b.h) / 2);
    if (rad <= 0) {
        r.fill(b, col);
        return;
    }
    r.fill({b.x, b.y + rad, b.w, b.h - 2 * rad}, col);
    for (int i = 0; i < rad; ++i) {
        const float dy = float(rad - i) - 0.5f;
        const int dx = rad - int(std::lround(std::sqrt(float(rad * rad) - dy * dy)));
        r.fill({b.x + dx, b.y + i, b.w - 2 * dx, 1}, col);
        r.fill({b.x + dx, b.y + b.h - 1 - i, b.w - 2 * dx, 1}, col);
    }
}

// 径向遮罩：以 (ccx,ccy) 为心，r0 内全透明、r1 外全不透，中间线性插值，
// 用背景色叠加（软件光栅器没有模糊）。逐像素 blend 太贵，按 4px 小格填充。
// tau = 衰减长度（格）：遮盖强度 = 1 - exp(-d/tau)。
// 用指数而不是线性/smoothstep：线性两头都平，中间的键怎么调都不够透；
// 指数是「中心掉得快、外面拖一条长尾」，于是邻键明显半透明、数字行还能隐约看见。
void vignette(render::Renderer& r, disp::Rect area, float ccx, float ccy, float pitchX,
              float pitchY, float tau, disp::Color col) {
    if (!(tau > 0.f)) return;
    const float ix = pitchX > 0.f ? 1.f / pitchX : 0.f;
    const float iy = pitchY > 0.f ? 1.f / pitchY : 0.f;
    // 2px 小格：6px 格子会看出棋盘台阶（边缘"生硬"就是这个）
    constexpr int kCell = 2;
    constexpr int kFar = 10;  // 接近饱和的远场用大块铺，省绘制调用
    auto rampAt = [&](float px, float py) {  // 该点的遮盖强度 0..1
        const float dx = (px - ccx) * ix, dy = (py - ccy) * iy;  // 距离按「格」算
        return 1.f - std::exp(-std::sqrt(dx * dx + dy * dy) / tau);
    };
    for (int y = area.y; y < area.bottom();) {
        const int stepY = (rampAt(ccx, float(y) + kFar * 0.5f) >= 0.97f &&
                           rampAt(ccx, float(y) + kFar - 1.f) >= 0.97f)
                              ? kFar
                              : kCell;
        const int h = std::min(stepY, area.bottom() - y);
        for (int x = area.x; x < area.right();) {
            const int stepX =
                (stepY > kCell &&
                 rampAt(float(x) + kFar * 0.5f, ccy) >= 0.97f)  // 远场：整块接近饱和
                    ? kFar
                    : kCell;
            const float t = rampAt(float(x) + stepX * 0.5f, float(y) + h * 0.5f);
            const int w = std::min(stepX, area.right() - x);
            if (t > 0.f) {
                disp::Color cc = col;
                cc.a = uint8_t(std::lround(float(col.a) * t));
                r.fill({x, y, w, h}, cc);
            }
            x += stepX;
        }
        y += stepY;
    }
}

void ptitle(render::Renderer& r, const FontBook& f, int x, int y, const std::string& text,
            uint32_t color, float s) {
    r.textInBox({x, y, 0, S(12, s)}, text, f.ptitle, c(color), render::Align::Left, 0, 0,
                S(1, s));
}

struct HintRow {
    const char* key;
    const char* what;
};

// 面板底部的「键 | 功能」提示表：两列右/左对齐，顶部一条 1px 分隔线，整块贴着面板下沿。
// 数据在 drawKeyPanels 里，跟着 keymap.lua 的语义走（改键位记得一起改）
void drawHints(render::Renderer& r, const FontBook& f, int panelX, int panelY, int panelW,
               int panelH, const HintRow* rows, int n, float s) {
    if (n <= 0 || f.hints == nullptr) return;
    const int padX = S(10, s), padTop = S(10, s), padBot = S(12, s);
    const int rowH = S(16, s), rowGap = S(4, s);
    int keyW = 0;
    for (int i = 0; i < n; ++i) keyW = std::max(keyW, r.textWidth(rows[i].key, f.hints));
    const int blockH = padTop + n * rowH + (n - 1) * rowGap + padBot;
    const int top = panelY + panelH - blockH;
    r.fill({panelX, top, panelW, 1}, c(col::kLine));
    int y = top + padTop;
    const int keyX = panelX + padX;
    const int valX = keyX + keyW + S(9, s);
    for (int i = 0; i < n; ++i) {
        r.textInBox({keyX, y, keyW, rowH}, rows[i].key, f.hints, c(col::kDim),
                    render::Align::Right);
        r.textInBox({valX, y, 0, rowH}, rows[i].what, f.hints, c(col::kFaint),
                    render::Align::Left);
        y += rowH + rowGap;
    }
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
    // 外壳中性：静止用 dim，锁定才给强调色（按下时 put 会反白）
    put(1, 0, "Tab", col::kDim, st.pressedKey == cfg::kDpadUp);
    put(1, 1, "·", col::kFaint, false);
    put(0, 1, "Shift", st.modShift ? col::kCyan : col::kDim, st.pressedKey == cfg::kDpadLeft);
    put(2, 1, "Alt", st.modAlt ? col::kCyan : col::kDim, st.pressedKey == cfg::kDpadRight);
    put(1, 2, "Ctrl", st.modCtrl ? col::kCyan : col::kDim, st.pressedKey == cfg::kDpadDown);
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
    // 位置按真机（Nintendo 布局）：X 在上、Y 在左、A 在右、B 在下
    put(1, 0, "X", col::kDim, cfg::kX);
    put(0, 1, "Y", col::kDim, cfg::kY);
    put(2, 1, "A", col::kDim, cfg::kA);
    put(1, 2, "B", col::kDim, cfg::kB);
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
    imeRows = std::max(1, uic.imeRows);  // 行数不随像素缩放变化
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
    // tab 条的可画区：右边要给时钟/电量留出来（那段是右对齐画的，画到哪算到哪），
    // 否则开多了 tab 就压到时钟上；每个 tab 的标题再各自裁进自己的框里，
    // 否则真终端的标题（"brejce@host: ~/path"）会溢出 96px 压到隔壁 tab
    const disp::Rect strip{S(180, s), 0, g.screenW - S(180, s) - S(200, s), g.topBarH};
    for (int i = 0; i < int(titles.size()); ++i) {
        disp::Rect box = tabRect(g, uic, i);
        const bool on = i == active;
        if (box.x >= strip.right()) break;  // 后面的整个都在可画区外
        r.fill(box, c(on ? col::kLine : col::kTerm));  // 底色不裁：宁可被后面盖住也不露缝
        const int cx = std::max(box.x, strip.x);
        const int cr = std::min(box.right(), strip.right());
        if (cr <= cx) continue;
        r.setClip({cx, box.y, cr - cx, box.h});
        // 左对齐 + 放不下就截断加省略号：居中的长标题被裁会变成"中间一小段"，
        // 12 个 tab 全长得一样，看不出是哪个
        std::string label = titles[size_t(i)];
        const int avail = box.w - S(12, s);
        if (r.textWidth(label, f.bar, S(1, s)) > avail) {
            while (!label.empty() && r.textWidth(label + "…", f.bar, S(1, s)) > avail) {
                label.pop_back();
                while (!label.empty() && (uint8_t(label.back()) & 0xC0) == 0x80) label.pop_back();
            }
            label += "…";
        }
        r.textInBox(box, label, f.bar, c(on ? col::kCyan : col::kDim), render::Align::Left,
                    S(6, s), S(6, s), S(1, s));
        // 活动 tab 底部一条线：底色用的是 kLine，不加标记分不清哪个是当前的
        if (on) r.fill({box.x, box.bottom() - S(2, s), box.w, S(2, s)}, c(col::kCyan));
        r.clearClip();
    }
    const disp::Rect plus = tabPlusRect(g, uic, int(titles.size()));
    if (plus.x < strip.right()) {
        r.setClip({plus.x, strip.y, std::min(plus.right(), strip.right()) - plus.x, strip.h});
        r.textInBox(plus, "+", f.bar, c(col::kFaint), render::Align::Center, 0, 0, S(1, s));
        r.clearClip();
    }
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

// 顶栏 + 底栏 + tab 条 的内容签名：脏矩形只在这串变了时才重画这两条。
// 往 drawTopBar/drawBottomBar 里加新字段，必须同时加到这里，否则那一格永远不会刷新。
std::string barSignature(const AppState& st, const std::vector<std::string>& titles, int active) {
    std::string sig = st.clock + '|' + std::to_string(st.battery) + (st.charging ? "+" : "") +
                      '|' + st.layerName + '|' + std::to_string(st.layer) + '|' + st.imeName +
                      '|' + st.modsText + '|' + std::to_string(active);
    for (const auto& t : titles) {
        sig += '|';
        sig += t;
    }
    return sig;
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
    item("IME", st.imeName, st.imeName == "拼音" ? col::kYellow : col::kGreen);

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
    // 整体偏移：键区（含下面的按钮簇，它们按窗口底边排）可以整体上/下挪
    const int dy = S(uic.keyAreaDy, s);
    return {panelX + (panelW - w) / 2, panelY + (panelH - h) / 2 + dy, w, h};
}

// 全角/CJK 的首字节：>= E3 起就是 CJK 区（U+3000 以上）。只用来给键帽挑字号系数，
// 不需要精确解码——ASCII 和拉丁扩展都是 1~2 字节，不会误判
static bool firstCodepointIsFullWidth(const std::string& s) {
    return !s.empty() && static_cast<unsigned char>(s[0]) >= 0xE3;
}

void drawKeyHand(render::Renderer& r, const FontBook& f, const cfg::Ui& uic, const Geometry& g,
                 int layer, int first, int panelX, int panelY, int panelW, int panelH,
                 float ptrX, float ptrY, int snap, int hover, float attractRadius,
                 float snapPulse, float pullX, float pullY) {
    // 中心遮罩：只有画面中心的键是清楚的，往外逐渐沉入背景（"走向幕后阴影"）。
    // 软件光栅器没有高斯模糊，所以做成往面板底色淡出 + 淡到一定程度直接不画。
    // 半径是设计像素，和键距同一套：相邻键 70/76，对角 103。
    // 半径按「窗口现在是 3 列 × 4 行」定：相邻行 82、数字行离中心 123，所以内圈要盖住
    // 中心两行（清楚），外圈到对角（≈144）还留一点影子 —— 数字行就是这么"淡但可见"的
    // 半径按「格」计（横向一格 = 纵向一格）：0.4 格内全亮，2.9 格全遮。
    // 于是中心实、邻键半透明（0.4→约 15%）、数字行/底行透出来（2 格 → 约 68%）、
    // 再远就没了。距离用格数而不是像素，横竖邻键的观感才一样。
    // 遮罩衰减长度（单位=格，横向一格和纵向一格等权）：邻键(d=1)遮 49%、数字行(d=2)遮 74%、
    // 对角(d=2.2)遮 77% —— 邻键明显透、数字行隐约可见但不抢眼
    constexpr float kMaskTau = 1.5f;
    // 缩放半径也按格走：0.6 格开始缩，2.3 格缩到最小（相邻键才看得出大小差）
    constexpr float kShrinkIn = 0.6f, kShrinkOut = 2.3f, kShrinkMax = 0.30f;
    constexpr float kSnapGrow = 1.09f;  // 选中键静止时的尺寸（相对键帽 64px）
    constexpr float kPulseAmp = 0.14f;  // 换键瞬间额外再涨 14% 然后回落 = 弹簧感
    const float s = g.scale;
    // 键距（设计像素）：第 1 颗 = 右邻，第 5 颗 = 下邻。遮罩距离按「格」算要用它
    const float pitchX = float(uic.keysV2[1].x - uic.keysV2[0].x);
    const float pitchY = float(uic.keysV2[5].y - uic.keysV2[0].y);
    const float invPitchX = pitchX > 0.f ? 1.f / pitchX : 0.f;
    const float invPitchY = pitchY > 0.f ? 1.f / pitchY : 0.f;
    const int L = ((layer % cfg::kLayerCount) + cfg::kLayerCount) % cfg::kLayerCount;

    const int side = S(uic.keySize, s);          // 键帽边长来自 ui.key_size
    const int b = std::max(1, S(1, s));          // 1px 边框
    // 行高必须跟字号走：keyMain 36px / keySub 18px（FontBook 里已乘 kUiFontBoost）。
    // 之前这里还是 26/13（字号翻倍前的值），于是主字和上档字在键帽里叠在一起。
    const int gap = S(4, s);  // 主字/副字之间的间距（设计像素）
    const disp::Rect win = keyWindowRect(g, uic, panelX, panelY, panelW, panelH);
    const int cx = win.x + win.w / 2, cy = win.y + win.h / 2;  // 面板中心 = 指针位置
    auto px = [s](float v) { return int(std::lround(v * s)); };
    const disp::Color acc = c(first == 0 ? col::kCyan : col::kRed);  // 左手青 / 右手红
    // 准心 = 选中键的键心（没吸附时退回指针）。衰减必须按它算，不能按摄像机：
    // 摄像机被 camHomeOffY 顶高了 41px，用它算会让 d（41px）和 e（41px）距离相等 —— 静止时大小就没区别。
    float retX = ptrX, retY = ptrY;
    if (snap >= 0 && snap < cfg::kKeyV2Count) {
        retX = float(uic.keysV2[snap].x) + float(uic.keySize) * 0.5f;
        retY = float(uic.keysV2[snap].y) + float(uic.keySize) * 0.5f;
    }
    // 窗外不外溢，但外扩「弹性带」（键帽 0.6 倍）：相机过冲时最外圈键会跟着探出窗口，
    // 按窗口硬裁就是"数字行头部被横切一条"。外扩后再交面板本身，防止糊到终端区。
    const int cl = std::max(2, S(int(std::lround(double(uic.keySize) * 0.6)), s));
    const disp::Rect box{win.x - cl, win.y - cl, win.w + 2 * cl, win.h + 2 * cl};
    const disp::Rect panel{panelX, panelY, panelW, panelH};
    r.setClip({std::max(box.x, panel.x), std::max(box.y, panel.y),
               std::min(box.right(), panel.right()) - std::max(box.x, panel.x),
               std::min(box.bottom(), panel.bottom()) - std::max(box.y, panel.y)});
    for (int i = 0; i < cfg::kKeyV2Count / 2; ++i) {
        const int k = first + i;
        const bool isSnap = snap == k;
        // 吸附优先；而且吸附期间物理层不更新 hovered，所以这里也不能信它（会留下过期边框）
        const bool isHover = snap < 0 && hover == k;
        // 吸附动效：亮度 = 物理场的真实受力（同一半径、同一距离），所以往右推时 f 先亮、
        // 再往右 g 才亮；松手回零位时亮度跟着退回 d。吸附键已反色，不再叠加。
        // 当前层这键是空的：整块都不画（物理层也不会吸它）
        if (uic.layers[L][k].empty()) continue;
        const float kcx = float(uic.keysV2[k].x) + float(uic.keySize) * 0.5f;
        const float kcy = float(uic.keysV2[k].y) + float(uic.keySize) * 0.5f;
        // 亮度按「指针离键心多近」算（不是摄像机）：指针被引力收进键心后自然全灭，
        // 移动过程中才点亮，指针静止时不残留莫名其妙的边框
        const float pull =
            isSnap || attractRadius <= 0.f
                ? 0.f
                : std::clamp(1.f - std::hypot(kcx - pullX, kcy - pullY) / attractRadius, 0.f,
                             1.f);
        disp::Color edge = c(isSnap || isHover ? col::kText : col::kLine);
        disp::Color bg = c(isSnap ? col::kText : col::kKkBg);
        disp::Color mainCol = c(isSnap ? col::kBg : col::kText);
        disp::Color subCol = c(isSnap ? col::kBg : col::kDim);
        if (pull > 0.f) {  // 边框先亮 → 底色跟着亮 → 过一半再开始染字：三段手感看得出来
            edge = disp::mix(edge, acc, pull);
            bg = disp::mix(bg, acc, pull * 0.25f);
            mainCol = disp::mix(mainCol, acc, std::max(0.f, pull * 2.f - 1.f));
        }
        // 离准心越远越淡：这里只决定"缩小多少"和"远到看不见就不画"，
        // 真正的变暗交给画完之后铺的径向遮罩（逐键整体降透明度会有整块感，很假）
        const float dmask = std::hypot((kcx - retX) * invPitchX, (kcy - retY) * invPitchY);
        const float fade = std::exp(-dmask / kMaskTau);
        if (fade <= 0.04f) continue;  // 沉到底了（约 4.8 格）：连框带字都不画
        // 偏移 = 面板中心 - 指针（世界坐标）：吸附住的键正好压在中心上
        const int kx = cx + px(kcx - ptrX);
        const int ky = cy + px(kcy - ptrY);
        // 外围未选中的键缩小：用独立半径曲线，连续变化不跳档
        const float st = std::clamp((dmask - kShrinkIn) / (kShrinkOut - kShrinkIn), 0.f, 1.f);
        // 选中的键：静止 1.09，换键那一下再乘 (1 + 0.14×pulse)，然后落回静止尺寸
        const float shrink =
            isSnap ? kSnapGrow * (1.f + kPulseAmp * snapPulse) : (1.f - kShrinkMax * st);
        const int ds = int(std::lround(float(side) * shrink));
        const disp::Rect box{kx - ds / 2, ky - ds / 2, ds, ds};
        // 吸附 = 整键反色（底色 col::kText、字色 col::kBg）；悬停（无吸附时）= 亮色描边
        // 圆角：外圈 rad、内圈 rad-b（门框式描边），半径跟着键块尺寸走，缩小的外围键也是圆角
        // 半径按键块边长取 18%（64 → 11.5）：7px 在 64px 方块上肉眼看就是一圈 45° 倒角，
        // 因为逐行内缩量化成 4-3-2-1 的直线台阶。18% 才有"圆"的观感，且跟缩小同步。
        const int rad = std::clamp(int(std::lround(float(ds) * 0.18f)), 3, ds / 3);
        fillRound(r, box, rad, edge);
        fillRound(r, {box.x + b, box.y + b, box.w - 2 * b, box.h - 2 * b},
                  std::max(1, rad - b), bg);
        // 主字符在上（大/亮），副字符在下（小/淡）：整块字在键帽里竖向居中。
        // 字母在真实键帽上只印大写，所以 ALPHA 层的字母标签也显示大写（实际打出来的仍是小写，
        // 上档/大写在输入路由里处理）；副字符只显示"上档真的有别的字"的键（数字、标点）。
        // 字跟着键块一起缩（tscale = ds/side），所以外围小键里也是小字，不会撑爆。
        const float tscale = side > 0 ? float(ds) / float(side) : 1.f;
        // 行盒高度取「字墨」≈0.72em，不用 lineHeight：lineHeight 含行距（36px 字的行盒是 48），
        // 两行加间距 78 根本进不了 48 的键帽，副字会被顶到键帽外面
        auto inkH = [tscale](const render::FontFace* f) {
            if (f == nullptr) return 0;
            return int(std::lround(f->px * 0.72 * double(tscale)));
        };
        const std::string& subCh = uic.shifts[L][k];  // 没上档就不画副字符
        const bool hasSub = !subCh.empty();
        // 两行（主字 + 副字）必须塞进键帽：keyMain 36px 的行盒 48、keySub 18px 的行盒 26，
        // 加上间距 4 = 78，键帽 48 装不下 → 副字会被顶到键帽外面。按「正好塞进去」统一缩一次，
        // 缩放走 textInBox 的 scale（贴字形位图），不重新栅格化，也不会因为字号缓存跳档
        const int amH = inkH(f.keyMain);
        const int agH = hasSub ? int(std::lround(float(gap) * tscale)) : 0;
        const int asH = hasSub ? inkH(f.keySub) : 0;
        const int total = amH + agH + asH;
        const float fit = total > ds ? float(ds) * 0.96f / float(total) : 1.f;
        const int mH = int(std::lround(float(amH) * fit));
        const int gH = int(std::lround(float(agH) * fit));
        const int sH = int(std::lround(float(asH) * fit));
        const float tsc = tscale * fit;
        const int ty = box.y + (ds - mH - gH - sH) / 2;
        std::string mainCh = uic.layers[L][k];
        if (L == 0 && mainCh.size() == 1 && mainCh[0] >= 'a' && mainCh[0] <= 'z')
            mainCh[0] = char(mainCh[0] - 'a' + 'A');
        // Maple Mono 的 CJK 字形按 em 框满铺（墨高 35px@36px），拉丁只到 cap height（27px）。
        // 同一字号下汉字/全角标点会比字母高一截，NUM 层整片都是标点就显小/显大不齐；
        // 按 27/35 缩一下把墨高对齐拉丁（只是字面缩放，不改字号缓存）
        const float cjkK = firstCodepointIsFullWidth(mainCh) ? 0.78f : 1.f;
        r.textInBox({box.x, ty, box.w, mH}, mainCh, f.keyMain, mainCol, render::Align::Center, 0,
                    0, 0, tsc * cjkK);
        if (hasSub)
            r.textInBox({box.x, ty + mH + gH, box.w, sH}, subCh, f.keySub, subCol,
                        render::Align::Center, 0, 0, 0, tsc);
    }
    // 径向遮罩：圆心 = 准心在屏幕上的位置；铺满整个面板（只铺窗口的话，
    // 窗口边界会留一道硬边——外面是面板色、里面是被压暗的）
    r.clearClip();
    {
        const float ccx = float(cx + px(retX - ptrX));
        const float ccy = float(cy + px(retY - ptrY));
        // 遮罩色 = **背景色**（键淡出就是"透明地沉进背景"），再往黑里压 10% 让外圈更沉
        const disp::Color veil = disp::mix(c(col::kBg), disp::rgb(0x000000), 0.10f);
        vignette(r, disp::Rect{panelX, panelY, panelW, panelH}, ccx, ccy, pitchX, pitchY,
                 kMaskTau, veil);
    }
}

void drawKeyPanels(render::Renderer& r, const FontBook& f, const cfg::Ui& uic, const Geometry& g,
                   const AppState& st) {
    const float s = g.scale;
    const int rightX = g.termX + g.termW;
    r.fill({0, g.contentY, g.panelW, g.contentH}, c(col::kPanel));
    r.fill({rightX, g.contentY, g.panelW, g.contentH}, c(col::kPanel));
    drawKeyHand(r, f, uic, g, st.layer, 0, 0, g.contentY, g.panelW, g.contentH, st.ptrLeftX,
                st.ptrLeftY, st.snapLeft, st.hoverLeft, st.attractRadius, st.snapPulseL,
                st.pullLeftX, st.pullLeftY);
    drawKeyHand(r, f, uic, g, st.layer, 20, rightX, g.contentY, g.panelW, g.contentH,
                st.ptrRightX, st.ptrRightY, st.snapRight, st.hoverRight, st.attractRadius,
                st.snapPulseR, st.pullRightX, st.pullRightY);
    // 竖分隔线画在键之后：滚到窗边的键不该盖住面板边界
    r.fill({g.panelW - 1, g.contentY, 1, g.contentH}, c(col::kLine));
    r.fill({rightX, g.contentY, 1, g.contentH}, c(col::kLine));

    // 肩键提示：面板最上面一行（键名亮、动作暗）
    {
        auto hint = [&](int x, const char* key, const char* act) {
            const int y = g.contentY + S(6, s);
            r.textInBox({x, y, 0, S(16, s)}, key, f.shoulder, c(col::kDim), render::Align::Left);
            const int kx = x + r.textWidth(key, f.shoulder) + S(6, s);
            r.textInBox({kx, y, 0, S(16, s)}, act, f.shoulder, c(col::kFaint),
                        render::Align::Left);
        };
        // 肩键/扳机名按 Switch 的叫法：L / ZL、R / ZR。
        // L 不是"上屏"——它只上屏吸附住的键，跟空格/回车不是一回事，所以叫魔法键。
        // 右肩键在有修饰键（按住或锁定）时是光标的上下，提示跟着变
        const char* rUp = st.modShift ? "上" : "左";
        const char* rDn = st.modShift ? "下" : "右";
        hint(S(10, s), "L", "魔法键");
        hint(S(96, s), "ZL", "空格");
        hint(rightX + S(10, s), "R", rUp);
        hint(rightX + S(96, s), "ZR", rDn);
    }

    // 按键簇摆在键窗下面：窗口在面板里竖向居中，所以从窗口底边往下排
    const disp::Rect win = keyWindowRect(g, uic, 0, g.contentY, g.panelW, g.contentH);
    const int dy = win.bottom() + S(44, s);  // 和键窗拉开一点，不然挤在一起
    // 不画「// DPAD」「// ABXY」标题了（键簇自己看得懂，标题只是噪声）。
    // 两块都按面板宽度居中：左对齐会让 DPAD 的视觉重心偏左
    drawDpad(r, f, (g.panelW - 3 * g.dpadCell) / 2, dy, g.dpadCell, st, s);
    drawAbxy(r, f, rightX + (g.panelW - 3 * g.abxyCell) / 2, dy, g.abxyCell, s, st.pressedKey);

    // 面板底部提示表：左右各一张，贴着面板下沿
    // 肩键（L/ZL、R/ZR）不写在这里：面板顶部那行已经画了，重复只是噪声
    static const HintRow kLeftHints[] = {
        {"L3", "切层"},
        {"DPAD", "修饰"},
        {"双击DPAD", "锁定"},
    };
    static const HintRow kRightHints[] = {
        {"A", "回车"},
        {"B", "退格"},
        {"X", "Del"},
        {"Y", "Esc"},
    };
    drawHints(r, f, 0, g.contentY, g.panelW, g.contentH, kLeftHints,
              int(sizeof(kLeftHints) / sizeof(kLeftHints[0])), s);
    drawHints(r, f, rightX, g.contentY, g.panelW, g.contentH, kRightHints,
              int(sizeof(kRightHints) / sizeof(kRightHints[0])), s);
}

void drawTermBackground(render::Renderer& r, const FontBook& f, const Geometry& g) {
    (void)f;
    r.fill({g.termX, g.contentY, g.termW, g.contentH}, c(col::kTerm));
}

void drawComposeLine(render::Renderer& r, const FontBook& f, const Geometry& g,
                     const AppState& st, int cursorRow, int cursorCol, bool cursorVisible) {
    if (!cursorVisible || (st.compose.empty() && st.cands.empty())) return;

    const float s = g.scale;
    const int cellH = g.termCellH;
    const int totalRows = std::max(g.termRows + g.imeRows, 1);
    const int row = std::clamp(cursorRow, 0, g.termRows - 1);
    int y = g.contentY + g.termPadTop + row * cellH;
    int x0 = g.termX + g.termPadX + std::clamp(cursorCol, 0, g.termCols - 1) * g.termCellW;
    const int right = g.termX + g.termW - g.termPadX;
    r.setClip({g.termX + 1, g.contentY, g.termW - 2, g.contentH});

    // 组字串贴着 PTY 光标；PTY 行数已先扣 imeRows，所以候选在最底输入行下仍放得下。
    int baseline = y + cellH / 2 + (f.term->ascent - f.term->descent) / 2;
    if (!st.compose.empty()) {
        r.fill({x0, y, std::max(0, right - x0), cellH}, c(col::kTerm));
        r.textBaseline(x0, baseline, st.compose, f.term, c(col::kBlue));
        int w = r.textWidth(st.compose, f.term);
        r.fill({x0, y + cellH - S(3, s), w, S(1, s)}, c(col::kBlue));
    }

    // 候选紧邻输入行；预留行只限制 PTY 高度，不把候选钉在屏幕底部。
    y = g.contentY + g.termPadTop + std::min(row + 1, totalRows - 1) * cellH;
    baseline = y + cellH / 2 + (f.term->ascent - f.term->descent) / 2;
    int x = x0;
    r.fill({x, y, std::max(0, right - x), cellH}, c(col::kTerm));
    for (size_t i = 0; i < st.cands.size() && i < 7; ++i) {
        const CandView& cd = st.cands[i];
        std::string num = std::to_string(i + 1);
        int wn = r.textWidth(num, f.term);
        int wt = r.textWidth(cd.text, f.term);
        int wa = cd.annot.empty() ? 0 : S(6, s) + r.textWidth(cd.annot, f.term);
        if (x + wn + S(4, s) + wt + wa > right) break;
        bool active = (i == size_t(st.candSel));
        if (active)
            r.fill({x - S(2, s), y, wn + wt + wa + S(10, s), cellH}, c(col::kBlue));
        r.textBaseline(x, baseline, num, f.term, c(active ? 0x000000 : col::kDim));
        x += wn + S(4, s);
        r.textBaseline(x, baseline, cd.text, f.term, c(active ? 0x000000 : col::kText));
        x += wt;
        if (wa > 0) {
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
        r.textBaseline(right - w, baseline, pg, f.term, c(col::kFaint));
    }
    r.clearClip();
}

}  // namespace ui
