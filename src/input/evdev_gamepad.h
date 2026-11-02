#pragma once
// evdev 手柄：Mapper 只做「事件 → 输出」的纯映射（可用合成事件自检），
// EvdevGamepad 负责开设备/读事件。Switch 上 Joy-Con 就是标准 evdev 手柄。
#include <linux/input.h>

#include <functional>
#include <string>
#include <vector>

#include "config/lua_config.h"
#include "input/gamepad.h"

namespace input {

// 输出口：字节进输入流，动作走面板那套 ActionId（切层/切中英/光标/锁修饰…）
struct GamepadHandlers {
    std::function<void(const std::string&)> bytes;
    std::function<void(int)> action;
};

// 面板动作编号：唯一来源 = cfg::Action（与 ui::ActionId 同一套值）。
// kGpAct* 是旧名，保留是为了调用方不改；语义一一对应，没有 input 独有的动作。
constexpr cfg::Action kGpActIme = cfg::kIme;
constexpr cfg::Action kGpActCursorLeft = cfg::kCursorLeft;
constexpr cfg::Action kGpActCursorRight = cfg::kCursorRight;
constexpr cfg::Action kGpActLayer = cfg::kLayer;
constexpr cfg::Action kGpActLockCtrl = cfg::kLockCtrl;
constexpr cfg::Action kGpActStickyCtrl = cfg::kModCtrl;
constexpr cfg::Action kGpActStickyShift = cfg::kModShift;
constexpr cfg::Action kGpActStickyAlt = cfg::kModAlt;
constexpr cfg::Action kGpActLockShift = cfg::kLockShift;
constexpr cfg::Action kGpActLockAlt = cfg::kLockAlt;

class GamepadMapper {
public:
    // 轴的原始范围（EVIOCGABS 读出来；Switch 的摇杆常见 0..255）
    void setAxisRange(int code, int minV, int maxV);
    void onEvent(const input_event& ev, const GamepadHandlers& h);

    int leftSector() const { return left_.sector(); }
    int rightSector() const { return right_.sector(); }

private:
    struct Range {
        int minV = -32768, maxV = 32767;
    } lx_, ly_, rx_, ry_;
    int lxV_ = 0, lyV_ = 0, rxV_ = 0, ryV_ = 0;
    StickNormalizer left_, right_;
    int lastL_ = -1, lastR_ = -1;
    long long lastDpadMs_ = 0;  // 同方向连按两下 = 锁定
    int lastDpadDir_ = 0;  // 只在扇区变化时输出，否则每次 SYN 都会吐字母
    float norm(int v, const Range& r) const;
    void dpadKey(int dir, int stickyA, int lockA, int bit, int stickyB, int lockB,
                 const GamepadHandlers& h);
};

// 新架构的按键上报：只报「哪个键被按下 + 摇杆在哪个扇区」，映射交给 cfg::KeyMap
enum PadButton {
    kPadLB = 0, kPadLT, kPadRB, kPadRT, kPadA, kPadB, kPadX, kPadY,
    kPadUp, kPadDown, kPadLeft, kPadRight, kPadCount
};

struct PadHandlers {
    std::function<void(int btn)> onButton;
    std::function<void(int sector)> onLeftStick;   // -1 = 回中
    std::function<void(int sector)> onRightStick;
};

class PadState {  // 纯映射：evdev 事件 → PadButton / 扇区（可用合成事件自检）
public:
    PadState() = default;
    PadState(float deadzone, float hysteresisDeg)  // engine.input.deadzone / hysteresis_deg
        : left_(deadzone, hysteresisDeg), right_(deadzone, hysteresisDeg) {}

    void setAxisRange(int code, int minV, int maxV);
    void onEvent(const input_event& ev, const PadHandlers& h);

private:
    struct Range {
        int minV = -32768, maxV = 32767;
    } lx_, ly_, rx_, ry_;
    int lxV_ = 0, lyV_ = 0, rxV_ = 0, ryV_ = 0;
    StickNormalizer left_, right_;
    int lastL_ = -1, lastR_ = -1;
    float norm(int v, const Range& r) const;
};

class EvdevGamepad {
public:
    ~EvdevGamepad() { close(); }

    // 打开所有「像手柄」的 /dev/input/event*（有 ABS_X/ABS_Y 或 BTN_SOUTH）
    int open();
    // 摇杆参数要在这里设：open() 时才会用它们建 PadState（engine.input.*）
    void setStickParams(float deadzone, float hysteresisDeg) {
        stickDz_ = deadzone;
        stickHyst_ = hysteresisDeg;
    }
    void close();
    int count() const { return int(fds_.size()); }
    const std::vector<std::string>& names() const { return names_; }

    bool poll(const GamepadHandlers& h);   // 旧字节路径
    bool pollPad(const PadHandlers& h);    // 新：报按键/扇区

private:
    std::vector<int> fds_;
    std::vector<std::string> names_;
    std::vector<GamepadMapper> mappers_;
    std::vector<PadState> pads_;
    std::string lastError_;
    float stickDz_ = 0.35f, stickHyst_ = 8.f;  // 默认值 = src/input/gamepad.h
};

}  // namespace input
