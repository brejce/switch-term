#pragma once
// 布局：设计基准 1280×720，实际按 kScale 放大到窗口分辨率。
// 所有 UI 尺寸都是 base × kScale 算出来的，字体也用同一个系数重栅格化
// （否则就是把 13px 位图硬放大，字会糊）。
#include "platform/display_backend.h"

namespace ui {
namespace layout {

constexpr int kDesignW = 1280;  // Switch 掌机模式的设计尺寸
constexpr int kDesignH = 720;

// layout.lua 里写的是「设计尺寸下的像素」，存这里
struct Base {
    int topBarH = 32, botBarH = 26;
    int panelW = 220, panelPadTop = 12, panelPadX = 10, panelPadBot = 8, panelGap = 8;
    int stickBox = 180, dpadCell = 26, abxyCell = 28;
    int termPadX = 14, termPadTop = 10, termPadBot = 8;
    int cellW = 8, cellH = 21, imeRows = 2;
};
inline Base base;

inline float kScale = 1.0f;

// 缩放后的实际值（UI 只用这些）
inline int kScreenW = kDesignW, kScreenH = kDesignH;
inline int kTopBarH = 32, kBotBarH = 26, kPanelW = 220;
inline int kPanelPadTop = 12, kPanelPadX = 10, kPanelPadBot = 8, kPanelGap = 8;
inline int kStickBox = 180, kStickCell = 36, kDpadCell = 26, kAbxyCell = 28;
inline int kTermPadX = 14, kTermPadTop = 10, kTermPadBot = 8;
inline int kCellW = 8, kCellH = 21, kImeRows = 2;
inline int kTermX = 220, kTermW = 840, kContentY = 32, kContentH = 662;
inline int kTermCols = 101, kTermRows = 28;

void setScale(float s);  // 改缩放并重算全部派生量
void recompute();

// 读 layout.lua（只填 base；缩放由调用方决定）
bool loadFile(const char* path);

}  // namespace layout
}  // namespace ui
