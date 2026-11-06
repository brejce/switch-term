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

// 映射只在这里发生一次：evdev 键码/坐标 → PadButton/扇区（下面），
// 键→动作全部在 config/keymap.lua（cfg::KeyMap），C++ 里不再有第二张键位表。

// 新架构的按键上报：只报「哪个键被按下 + 摇杆在哪个扇区」，映射交给 cfg::KeyMap
enum PadButton {
    kPadLB = 0, kPadLT, kPadRB, kPadRT, kPadA, kPadB, kPadX, kPadY,
    kPadUp, kPadDown, kPadLeft, kPadRight, kPadL3,
    kPadPlus,  // 右手柄 +（BTN_START）：兜底用，键位顺序必须与 cfg::InputKey 一致
    kPadCount
};

struct PadHandlers {
    std::function<void(int btn)> onButton;
    std::function<void(int sector)> onLeftStick;   // -1 = 回中
    std::function<void(int sector)> onRightStick;
    std::function<void(int btn)> onRelease;  // 抬起（DPAD hat 回中 / EV_KEY value==0）
};

class PadState {  // 纯映射：evdev 事件 → PadButton / 扇区（可用合成事件自检）
public:
    PadState() = default;
    PadState(float deadzone, float hysteresisDeg)  // engine.input.deadzone / hysteresis_deg
        : left_(deadzone, hysteresisDeg), right_(deadzone, hysteresisDeg) {}

    void setAxisRange(int code, int minV, int maxV);
    void onEvent(const input_event& ev, const PadHandlers& h);

    // 当前原始归一化向量（-1..1，y 已转成数学坐标：上 = +），不是扇区。
    // 物理场吃这套值（app::physics::step 的 ix/iy）；死区不在这里滤，调用方自己按需要滤。
    // 返回 false = 两个轴都在中心。
    bool stickVec(bool right, float& dx, float& dy) const;

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

    // 第一个报了向量的 pad 的值（合成设备与单侧 Joy-Con 上报一致，取谁都一样）。
    // 返回 false = 没有 pad，或没有 pad 报出非零向量（dx/dy 仍写成 0，调用方不用先清零）。
    bool stickVec(bool right, float& dx, float& dy) const;

    bool pollPad(const PadHandlers& h);    // 只报按键/扇区，动作交给 cfg::KeyMap

private:
    std::vector<int> fds_;
    std::vector<std::string> names_;
    std::vector<PadState> pads_;
    std::string lastError_;
    float stickDz_ = 0.35f, stickHyst_ = 8.f;  // 默认值 = src/input/gamepad.h
};

}  // namespace input
