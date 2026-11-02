#pragma once
#include <string>
#include <vector>

#include "render/renderer.h"
#include "ui/theme.h"

namespace ui {

// 摇杆两层的字符表：九宫格（row-major）+ 外圈四方向
struct StickSpec {
    std::string cells[9];
    std::string outers[4];
};

// 键位/标题：都可以被 layout.lua 覆盖
namespace keys {
inline StickSpec leftAlpha, leftSymbol, rightAlpha, rightNum;
inline std::string dpadTab = "Tab", dpadCtrl = "Ctrl", dpadShift = "Shift",
                    dpadAlt = "Alt";
inline std::string abxyX = "X", abxyA = "A", abxyB = "B";
void resetDefaults();
}  // namespace keys

struct UiState {
    std::string layer = "ALPHA";  // ALPHA | SYMBOL
    std::string lang = "ZH";      // ZH | EN
    std::string ime = "BUILTIN";
    std::string input = "GAMEPAD";
    bool caps = false;
    int locks = 0;   // 1=Ctrl 2=Shift 4=Alt（双击锁定，一直生效）
    int sticky = 0;  // 同上位图，但只作用于下一个键
    std::string clock = "20:20";
    int battery = 100;
    std::string version = "v0.1.0";
};

// 面板动作：鼠标/触摸点击、键盘、以后的手柄按钮走的都是这一个分发
enum ActionId {
    kActNone = 0,
    kActIme = 1,    // 中英切换
    kActCopy = 2,   // 复制可见区
    kActPaste = 3,  // 粘贴剪贴板
    kActSelLeft = 4,   // 选区左移一格（手柄 = Shift+ZL）
    kActSelRight = 5,  // 选区右移一格（手柄 = Shift+ZR）
    kActLayer = 6,     // 切换 字母层 / 符号·数字层（L）
    kActLockCtrl = 7,  // 双击锁定 Ctrl
    kActLockShift = 8, // 双击锁定 Shift（锁定后移动光标 = 扩选）
    kActLockAlt = 9,   // 双击锁定 Alt
    kActCursorLeft = 10,   // 光标左移；锁了 Shift 就变成扩选
    kActCursorRight = 11,
    kActStickyCtrl = 12,   // 按一下 = 只对下一个键生效（实体键盘的"先按 Ctrl 再按键"）
    kActStickyShift = 13,
    kActStickyAlt = 14,
};

// 可点击的字符格：action 为 0 时把 bytes 写入 PTY，否则执行动作
struct HitCell {
    disp::Rect rect;
    std::string bytes;
    int action = kActNone;     // 单击
    int dblAction = kActNone;  // 双击（clicks>=2）
};

void drawTopBar(render::Renderer& r, const FontBook& f, const UiState& st);
void drawBottomBar(render::Renderer& r, const FontBook& f, const UiState& st);
void drawLeftPanel(render::Renderer& r, const FontBook& f, int panelX, const UiState& st,
                   std::vector<HitCell>* hits = nullptr);
void drawRightPanel(render::Renderer& r, const FontBook& f, int panelX, const UiState& st,
                    std::vector<HitCell>* hits = nullptr);

}  // namespace ui
