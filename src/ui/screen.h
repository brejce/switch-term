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
    std::string layer = "ALPHA";  // ALPHA | SYMBOL
    std::string lang = "ZH";      // ZH | EN
    std::string imeName = "T9";
    bool modCtrl = false, modShift = false, modAlt = false;  // 锁定态（底部状态栏显示）
    int focusIdx = 4;             // T9 焦点格 0..8
    bool popup = false;           // 魔法键弹窗状态
    int popupFocus = -1;          // 当前弹窗锁定的格子 0..8（-1 = 没弹窗）
    int selCell = -1;             // 弹窗内选中的字符键 cell（-1 = 无选中：鼠标在虚无）
    int hoverCell = -1;           // 鼠标悬停的九宫格 cell（弹窗里只有字符键算实体）
    int pressedCell = -1;         // 刚触发的字符键 cell
    long long pressedMs = 0;      // 触发时间戳（steady_clock 毫秒），150ms 后失效
    std::string compose;          // 组字串
    std::vector<CandView> cands;  // 候选（含注音）
    int candPage = 0, candPageCount = 1;
    std::string clock = "20:20";
    int battery = 100;
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

// 九宫格 cell 0..8 的矩形。drawT9Panel 和鼠标命中测试共用这一份几何，别各算一遍
disp::Rect t9CellRect(const Geometry& g, int idx);

void drawTopBar(render::Renderer& r, const FontBook& f, const cfg::Ui& uic, const Geometry& g,
                const AppState& st);
void drawBottomBar(render::Renderer& r, const FontBook& f, const Geometry& g, const AppState& st);
void drawT9Panel(render::Renderer& r, const FontBook& f, const cfg::Ui& uic, const Geometry& g,
                 const AppState& st);
void drawMousePanel(render::Renderer& r, const FontBook& f, const cfg::Ui& uic, const Geometry& g,
                    const AppState& st);
// 中间终端区只画边框底色 + 组字串/候选两行；网格由调用方按 TerminalParser 填
void drawTermBackground(render::Renderer& r, const FontBook& f, const Geometry& g);
void drawComposeLine(render::Renderer& r, const FontBook& f, const Geometry& g,
                     const AppState& st);

}  // namespace ui
