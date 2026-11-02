#pragma once
#include <string>
#include <vector>

#include "config/lua_config.h"
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

// 面板动作：鼠标/触摸点击、键盘、以后的手柄按钮走的都是这一个分发。
// 编号唯一来源是 cfg::Action；kActXxx 是旧名（--legacy 路径在用），值全部指过去。
using ActionId = cfg::Action;
constexpr ActionId kActNone = cfg::kNone;
constexpr ActionId kActIme = cfg::kIme;              // 中英切换
constexpr ActionId kActCopy = cfg::kCopy;            // 复制可见区
constexpr ActionId kActPaste = cfg::kPaste;          // 粘贴剪贴板
constexpr ActionId kActSelLeft = cfg::kSelLeft;      // 选区左移一格（手柄 = Shift+ZL）
constexpr ActionId kActSelRight = cfg::kSelRight;    // 选区右移一格（手柄 = Shift+ZR）
constexpr ActionId kActLayer = cfg::kLayer;          // 字母层 / 符号·数字层（L）
constexpr ActionId kActLockCtrl = cfg::kLockCtrl;    // 双击锁定 Ctrl
constexpr ActionId kActLockShift = cfg::kLockShift;  // 双击锁定 Shift（锁后移动光标 = 扩选）
constexpr ActionId kActLockAlt = cfg::kLockAlt;      // 双击锁定 Alt
constexpr ActionId kActCursorLeft = cfg::kCursorLeft;    // 光标左移；锁了 Shift 就变成扩选
constexpr ActionId kActCursorRight = cfg::kCursorRight;
// 一次性修饰：物理上就是 keymap.lua 的 mod_* 动作
constexpr ActionId kActStickyCtrl = cfg::kModCtrl;  // 只对下一个键生效
constexpr ActionId kActStickyShift = cfg::kModShift;
constexpr ActionId kActStickyAlt = cfg::kModAlt;

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
