#pragma once
// Lua 只在启动/热重载时装配：把 ui.lua / keymap.lua / engine.lua 读成原生结构，
// 运行期主循环纯 C++。
#include <string>

#include "app/physics.h"

namespace cfg {

// 动作：全局唯一的一套编号。前半段是 keymap.lua 能绑的手柄动作，
// 后半段只有面板/触摸的命中格会产出（没有对应动作名，绑不进 keymap）。
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
    // 面板独有：只出现在 --legacy UI 的 HitCell / evdev 旧映射里（v2 起 R / ZR 也绑光标）
    kIme,        // 中英切换
    kCopy,       // 复制可见区/选区
    kPaste,      // 粘贴剪贴板
    kSelLeft,    // 选区左移一格
    kSelRight,   // 选区右移一格
    kLayer,      // 字母层 / 符号·数字层
    kCursorLeft, // 光标左移（锁 Shift 时 = 扩选）
    kCursorRight,
    // 多 tab：切/建/关都不产出字节，只动 app 里的 term 容器
    kNextTab,
    kPrevTab,
    kNewTab,
    kCloseTab,
    kLayerNext,  // L3：v2 键区层循环 ALPHA→NUM→SYM（不锁层）
    kOnboard,    // START(+): 拉起 onboard 屏幕键盘（自己打字坏掉时的兜底）
};

enum InputKey {
    kLB = 0, kLT, kRB, kRT, kA, kB, kX, kY,
    kDpadUp, kDpadDown, kDpadLeft, kDpadRight,
    kL3,
    kStart,  // 右手柄 +：顺序必须和 input::PadButton 一致
    kKeyCount
};


// v2 分体键盘的一个键位。坐标是设计像素（1280×720 基准），键帽 52×52 由渲染方按
// LAYOUT-v2 §1 取，这里只放左上角
struct KeyV2 {
    int x = 0, y = 0;
};

constexpr int kKeyV2Count = 30;
constexpr int kLayerCount = 3;  // 0=alpha 1=num 2=sym，层序循环见 LAYOUT-v2 §2

struct Ui {
    int panelW = 220, topBarH = 32, botBarH = 26;
    int stickBox = 180, dpadCell = 28, abxyCell = 30, shoulderH = 20;
    int termPadX = 14, termPadTop = 10, termPadBot = 8;
    int cellW = 8, cellH = 21, imeRows = 2;
    std::string title = "SYS.TERM";
    // v2：键位顺序 = 左场 row-major(3×5) 再接右场 row-major(3×5)；缺键/缺层保持空
    KeyV2 keysV2[kKeyV2Count];
    int keySize = 64;  // 键帽边长（设计像素），来自 ui.key_size
    // 面板一屏露几列几行（滑窗可见范围），来自 ui.key_window；左右手各是一个 5×3 的世界
    int keyWindowCols = 3, keyWindowRows = 3;
    std::string layers[kLayerCount][kKeyV2Count];  // 每键一个 UTF-8 字符（ASCII 是 1 字节）
    std::string dpadTitle = "// DPAD (MODS)", abxyTitle = "// ABXY";
};

struct KeyMap {
    Action single[kKeyCount] = {};
    Action dbl[kKeyCount] = {};
    bool axisMouseMove = false;
};

// engine.lua：「看不见」的那一半。默认值 = 接线前散落在各处的硬编码值，改这里等于改默认。
struct Engine {
    int scrollback = 2000;          // src/term/vterm_bridge.h 的滚动历史上限
    int candPerPage = 7;            // 底部候选条一页几个
    bool ngram = true;              // 候选是否按 bigram 上下文重排
    bool imeEnabled = true;         // false = 内置输入法不介入，字符原样透传（桌面模式与 fcitx 共存）
    float deadzone = 0.35f;         // src/input/gamepad.h 摇杆死区
    float hysteresisDeg = 8.f;      // 同上：扇区滞回角
    int doubleTapMs = 400;          // src/app/app.cpp 双击判定窗口
    std::string logLevel = "warn";  // debug | info | warn | error
    std::string logPath;            // 空 = 只写 stderr
    std::string sink = "pty";     // 输出口：pty = 写伪终端；uinput = 打给系统虚拟键盘
    int rotate = 90;                // 屏幕软件旋转：0/90/180/270（fbdev 用；Switch 面板是 720x1280 竖装）
    app::physics::Params physics;   // engine.physics：引力/吸附/回落的旋钮（docs/PHYSICS.md §4）

    // engine.rumble：Joy-Con 震动（evdev FF）。两个用法分开配：移动底噪 / 吸附"咚"
    struct RumbleCfg {
        bool enabled = true;
        float moveMax = 0.30f;      // 移动底噪最大强度（0..1）
        float moveSpeedRef = 500.f; // 指针速度到这么多 px/s 就算"推满"，映射到 moveMax
        float snapStrong = 0.85f;   // 吸附那一下：强马达
        float snapWeak = 0.55f;     //             弱马达
        int snapMs = 90;            // 持续时长（ms）
        int rateHz = 25;            // 底噪刷新率上限
        float moveSmoothMs = 60.f;  // 底噪强度的一阶低通时间常数（越大越绵）
    } rumble;
};

// 一次热重载要拿到的全部配置
struct Profile {
    Ui ui;
    KeyMap keymap;
    Engine engine;
};

bool loadUi(const char* path, Ui& out);
bool loadKeymap(const char* path, KeyMap& out);
// 缺文件 / 缺键 / 类型不符 = 保留 out 里的默认值，返回 false 但不报错
bool loadEngine(const char* path, Engine& out);
// dir 下依次读 ui.lua / keymap.lua / engine.lua；三个都读不到才返回 false
bool loadProfile(const char* dir, Profile& out);

const char* actionName(Action a);   // 反查，给日志/自检用
bool actionFromName(const char* s, Action& out);

}  // namespace cfg
