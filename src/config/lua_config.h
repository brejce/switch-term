#pragma once
// Lua 只在启动装配：把 ui.lua / keymap.lua 读成原生结构，运行期主循环纯 C++。
#include <string>

namespace cfg {

// 动作：由 keymap.lua 的字符串名映射到这些常量
enum Action {
    kNone = 0,
    kMagic,      // LB：网格 ↔ 弹窗 ↔ 上屏
    kSpace,      // LT
    kTab,        // 十字上
    kEnter,      // A
    kBackspace,  // B
    kDelete,     // X
    kEscape,     // Y
    kModCtrl,    // 一次性修饰
    kModAlt,
    kModShift,
    kLockCtrl,   // 双击锁定
    kLockAlt,
    kLockShift,
    kMouseLeft,  // RB
    kMouseRight, // RT
};

enum InputKey {
    kLB = 0, kLT, kRB, kRT, kA, kB, kX, kY,
    kDpadUp, kDpadDown, kDpadLeft, kDpadRight,
    kKeyCount
};

struct T9Key {
    std::string num;      // "2"
    std::string letters;  // "ABC"（多击取字母）
};

struct Ui {
    int panelW = 220, topBarH = 32, botBarH = 26;
    int stickBox = 180, dpadCell = 28, abxyCell = 30, shoulderH = 20;
    int termPadX = 14, termPadTop = 10, termPadBot = 8;
    int cellW = 8, cellH = 21, imeRows = 2;
    std::string title = "SYS.TERM";
    T9Key t9Alpha[9], t9Symbol[9];
    std::string t9Title = "// T9_KEYPAD", dpadTitle = "// DPAD (MODS)";
    std::string mouseTitle = "// MOUSE_CONTROL", abxyTitle = "// ABXY";
};

struct KeyMap {
    Action single[kKeyCount] = {};
    Action dbl[kKeyCount] = {};
    bool axisMouseMove = false;
};

bool loadUi(const char* path, Ui& out);
bool loadKeymap(const char* path, KeyMap& out);

const char* actionName(Action a);   // 反查，给日志/自检用
bool actionFromName(const char* s, Action& out);

}  // namespace cfg
