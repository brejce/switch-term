#pragma once
// 新 UI（DESIGN v4.1）：三分区纯字符界面。数据全部来自 cfg::Ui（Lua 装配），
// 这里只管画，不认终端/输入法/手柄的任何实现。
#include <string>
#include <vector>

#include "config/lua_config.h"
#include "render/renderer.h"
#include "ui/theme.h"

namespace ui {

// 候选：正文 + 注音（nǐ hǎo 这种带声调的拼音）
struct CandView {
    std::string text;
    std::string annot;
};

struct AppState {
    std::string layerName = "ALPHA";  // 展示用名字：ALPHA | NUM | SYM
    int layer = 0;                 // 层下标 0=ALPHA 1=NUM 2=SYM（L3 循环，LAYOUT-v2 §2）
    // v2 物理场：每侧一个指针。snap = 吸附的键（全局 0..29，-1 = 无），hover = 无吸附时最近的键
    int snapLeft = -1, snapRight = -1;
    int hoverLeft = -1, hoverRight = -1;
    float ptrLeftX = 0, ptrLeftY = 0, ptrRightX = 0, ptrRightY = 0;  // 设计像素
    float attractRadius = 60;     // engine.physics.attract_radius：键窗的点亮半径跟着物理场走
    float pointerRadius = 0;      // 指针球半径：渲染把球画出来，0 = 不画（原点模型）
    std::string lang = "ZH";      // ZH | EN
    std::string imeName = "PINYIN";
    bool modCtrl = false, modShift = false, modAlt = false;  // 锁定态（底部状态栏显示）
    int pressedKey = -1;          // 刚按下的键位（cfg::InputKey），提示条闪一下
    long long pressedKeyMs = 0;
    std::string compose;          // 组字串
    std::vector<CandView> cands;  // 候选（含注音）
    int candPage = 0, candPageCount = 1;
    int candSel = 0;              // 候选条选中项（页内下标）：空格上屏它
    std::string clock = "--:--";  // 真实本地时间（app.cpp 每 2s 刷）
    int battery = -1;              // -1 = 读不到电池（顶栏显示 BAT --%%）
    bool charging = false;
    std::string modsText = "";    // "CTRL+SHIFT" 之类，空 = 无
};

// 缩放后的几何（由 cfg::Ui 算出，跟布局缩放一起变）
struct Geometry {
    float scale = 1.0f;
    int screenW = 1280, screenH = 720;
    int panelW = 220, topBarH = 32, botBarH = 26;
    int stickBox = 180, keyCell = 60;
    int dpadCell = 28, abxyCell = 30, shoulderH = 20;
    int termX = 220, termW = 840, contentY = 32, contentH = 662;
    int termPadX = 14, termPadTop = 10, termPadBot = 8;
    int termCellW = 8, termCellH = 21, imeRows = 2;
    int termCols = 101, termRows = 28;
    void fromConfig(const cfg::Ui& uic, float scale);
};

// 顶栏里的 tab 条。格宽固定、起点是常量（不量标题宽度），所以命中测试不用带 renderer；
// drawTabBar 和 tabAt 共用 tabRect，绘制和命中只算一份几何。
// idx = 0..n-1 是 tab，idx == n 是末尾的「+」格（那格的宽是方形的条带高）
disp::Rect tabRect(const Geometry& g, const cfg::Ui& uic, int idx);
// 返回 0..count-1，count = 点中「+」，-1 = 没点中
int tabAt(const Geometry& g, const cfg::Ui& uic, int count, int x, int y);
void drawTabBar(render::Renderer& r, const FontBook& f, const Geometry& g, const cfg::Ui& uic,
                const std::vector<std::string>& titles, int active);

void drawTopBar(render::Renderer& r, const FontBook& f, const cfg::Ui& uic, const Geometry& g,
                const AppState& st);
void drawBottomBar(render::Renderer& r, const FontBook& f, const Geometry& g, const AppState& st);
// 中间终端区只画边框底色 + 组字串/候选两行；网格由调用方按 TerminalParser 填
void drawTermBackground(render::Renderer& r, const FontBook& f, const Geometry& g);
void drawComposeLine(render::Renderer& r, const FontBook& f, const Geometry& g,
                     const AppState& st);

// v2 分体键盘的滑窗键区：世界比面板大（5×3 键），只露出 ui.key_window.cols×rows 那一窗，
// 指针固定在窗口中心，摇杆滚动世界（不画准星：吸附键的整键反色 + 引力点亮就是方向指示）。
// layer 越界按 3 取模收进 ALPHA/NUM/SYM。
disp::Rect keyWindowRect(const Geometry& g, const cfg::Ui& uic, int panelX, int panelY,
                         int panelW, int panelH);

// 一只手的面板：panelX/Y/W/H = 可视矩形（也当裁剪矩形）；ptrX/ptrY = 该手指针（世界坐标）；
// first = 这手在 ui.keys 里的起点（左 0 / 右 15）；snap/hover = 高亮键下标（全局 0..29，-1 = 无）。
// attractRadius = 引力半径：键按「离指针多远」点亮（强度 = 物理场真实受力），0 = 不画动效。
// 每键屏位 = 面板中心 + (键中心 - 指针)，所以吸附住的键正好落在窗口中心，窗外的部分被裁掉。
void drawKeyHand(render::Renderer& r, const FontBook& f, const cfg::Ui& uic, const Geometry& g,
                 int layer, int first, int panelX, int panelY, int panelW, int panelH,
                 float ptrX, float ptrY, int snap, int hover, float attractRadius,
                 float pointerRadius);
// v2 左右键区整块：面板底 + 两手滑窗键区（含高亮/引力点亮）+ 左 DPAD 簇 / 右 ABXY 簇。
// 文字只来自 ui.lua 的标题（dpad_title / abxy_title），这里不写键字符。
void drawKeyPanels(render::Renderer& r, const FontBook& f, const cfg::Ui& uic, const Geometry& g,
                   const AppState& st);
// 物理按键簇（v2：画在字母区中缝里作提示）
void drawDpad(render::Renderer& r, const FontBook& f, int x0, int y0, int cell, const AppState& st,
              float s);
void drawAbxy(render::Renderer& r, const FontBook& f, int x0, int y0, int cell, float s,
              int pressedKey = -1);

}  // namespace ui
