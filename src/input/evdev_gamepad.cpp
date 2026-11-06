#include "input/evdev_gamepad.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <chrono>
#include <cmath>
#include <cstring>


namespace input {

float PadState::norm(int v, const Range& r) const {
    if (r.maxV == r.minV) return 0.f;
    float f = float(v - r.minV) / float(r.maxV - r.minV);
    return f * 2.f - 1.f;
}

void PadState::setAxisRange(int code, int minV, int maxV) {
    Range r{minV, maxV};
    int mid = (minV + maxV) / 2;
    switch (code) {
        case ABS_X: lx_ = r; lxV_ = mid; break;
        case ABS_Y: ly_ = r; lyV_ = mid; break;
        case ABS_RX: rx_ = r; rxV_ = mid; break;
        case ABS_RY: ry_ = r; ryV_ = mid; break;
        default: break;
    }
}

bool PadState::stickVec(bool right, float& dx, float& dy) const {
    dx = norm(right ? rxV_ : lxV_, right ? rx_ : lx_);
    dy = -norm(right ? ryV_ : lyV_, right ? ry_ : ly_);  // evdev 上 = 最小 → 数学坐标上 = +
    return dx != 0.f || dy != 0.f;
}

void PadState::onEvent(const input_event& ev, const PadHandlers& h) {
    if (ev.type == EV_ABS) {
        switch (ev.code) {
            case ABS_X: lxV_ = ev.value; break;
            case ABS_Y: lyV_ = ev.value; break;
            case ABS_RX: rxV_ = ev.value; break;
            case ABS_RY: ryV_ = ev.value; break;
            // hat 轴 -1/0/1：回中(0) 就是松开。按住 Shift 连按 R/ZR 要靠它判断"还按着"
            case ABS_HAT0X:
                if (ev.value < 0 && h.onButton) h.onButton(kPadLeft);
                if (ev.value > 0 && h.onButton) h.onButton(kPadRight);
                if (ev.value == 0 && h.onRelease) {
                    h.onRelease(kPadLeft);
                    h.onRelease(kPadRight);
                }
                break;
            case ABS_HAT0Y:
                if (ev.value < 0 && h.onButton) h.onButton(kPadUp);
                if (ev.value > 0 && h.onButton) h.onButton(kPadDown);
                if (ev.value == 0 && h.onRelease) {
                    h.onRelease(kPadUp);
                    h.onRelease(kPadDown);
                }
                break;
            default: break;
        }
    } else if (ev.type == EV_KEY && ev.value == 0) {
        int btn = -1;  // 抬起：只报"按住有意义"的那些
        switch (ev.code) {
            case BTN_DPAD_UP: btn = kPadUp; break;
            case BTN_DPAD_DOWN: btn = kPadDown; break;
            case BTN_DPAD_LEFT: btn = kPadLeft; break;
            case BTN_DPAD_RIGHT: btn = kPadRight; break;
            case BTN_TL: btn = kPadLB; break;
            case BTN_TR: btn = kPadRB; break;
            case BTN_TL2: btn = kPadLT; break;
            case BTN_TR2: btn = kPadRT; break;
            default: break;
        }
        if (btn >= 0 && h.onRelease) h.onRelease(btn);
    } else if (ev.type == EV_KEY && ev.value == 1) {
        int btn = -1;
        switch (ev.code) {
            case BTN_TL: btn = kPadLB; break;
            case BTN_TR: btn = kPadRB; break;
            case BTN_TL2: btn = kPadLT; break;
            case BTN_TR2: btn = kPadRT; break;
            case BTN_SOUTH: btn = kPadB; break;  // Joy-Con 物理 B 在下方
            case BTN_EAST: btn = kPadA; break;   // Joy-Con 物理 A 在右方（Xbox 相反）
            case BTN_NORTH: btn = kPadX; break;
            case BTN_WEST: btn = kPadY; break;
            case BTN_DPAD_UP: btn = kPadUp; break;
            case BTN_DPAD_DOWN: btn = kPadDown; break;
            case BTN_DPAD_LEFT: btn = kPadLeft; break;
            case BTN_DPAD_RIGHT: btn = kPadRight; break;
            case BTN_THUMBL: btn = kPadL3; break;  // 摇杆按下（BTN_THUMBR 设备上不存在）
            case BTN_START: btn = kPadPlus; break;  // 右手柄 +：呼出 onboard（KDE 抢手柄时的兜底）
            default: break;
        }
        if (btn >= 0 && h.onButton) h.onButton(btn);
    } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
        // evdev 的 Y：上=最小、下=最大；StickNormalizer 内部再转成数学坐标
        int ls = left_.update({norm(lxV_, lx_), norm(lyV_, ly_)});
        if (ls != lastL_ && h.onLeftStick) h.onLeftStick(ls);
        lastL_ = ls;
        int rs = right_.update({norm(rxV_, rx_), norm(ryV_, ry_)});
        if (rs != lastR_ && h.onRightStick) h.onRightStick(rs);
        lastR_ = rs;
    }
}

int EvdevGamepad::open() {
    close();
    for (int i = 0; i < 64; ++i) {
        char path[64];
        std::snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = ::open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        // 有没有摇杆/手柄按钮：没有就不是手柄，关掉
        unsigned char absbits[(ABS_MAX + 8) / 8] = {0};
        unsigned char keybits[(KEY_MAX + 8) / 8] = {0};
        if (::ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits) < 0 ||
            ::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) < 0) {
            ::close(fd);
            continue;
        }
        auto hasAbs = [&](int c) { return absbits[c / 8] & (1 << (c % 8)); };
        auto hasKey = [&](int c) { return keybits[c / 8] & (1 << (c % 8)); };
        if (!(hasAbs(ABS_X) && hasAbs(ABS_Y)) && !hasKey(BTN_SOUTH)) {
            ::close(fd);
            continue;
        }

        char name[256] = "?";
        ::ioctl(fd, EVIOCGNAME(sizeof name), name);
        // IMU（陀螺仪，量程 ±32767000）/ Serial（序列号）是同一台手柄的辅助节点，
        // 但它们也报 ABS_X/Y，会被当成摇杆读数抢走真正的摇杆
        if (std::strstr(name, "IMU") != nullptr || std::strstr(name, "Serial") != nullptr) {
            ::close(fd);
            continue;
        }
        fds_.push_back(fd);
        names_.push_back(name);
        PadState ps(stickDz_, stickHyst_);
        for (int code2 : {ABS_X, ABS_Y, ABS_RX, ABS_RY}) {
            input_absinfo ai2{};
            if (::ioctl(fd, EVIOCGABS(code2), &ai2) == 0)
                ps.setAxisRange(code2, ai2.minimum, ai2.maximum);
        }
        pads_.push_back(ps);
    }
    return count();
}

void EvdevGamepad::close() {
    for (int fd : fds_) ::close(fd);
    fds_.clear();
    names_.clear();
    pads_.clear();
}

bool EvdevGamepad::stickVec(bool right, float& dx, float& dy) const {
    // 默认轴范围下「回中」算出来是 1.5e-5 而不是 0，所以按噪声水平当零处理。
    // 不这么滤的话，只有左摇杆的那半个 Joy-Con 会用这个噪声值抢掉右摇杆的结果。
    constexpr float kStill = 1e-3f;
    dx = dy = 0.f;
    // 取「推得最狠」的那台，而不是第一台非零的：两台手柄同时连着时，
    // 没被碰的那台回中漂移会永久抢住另一台的推杆
    float bestX = 0.f, bestY = 0.f, best = kStill;
    for (const PadState& p : pads_) {
        float x = 0, y = 0;
        if (!p.stickVec(right, x, y)) continue;
        const float m = std::sqrt(x * x + y * y);
        if (m <= best) continue;
        best = m;
        bestX = x;
        bestY = y;
    }
    dx = bestX;
    dy = bestY;
    return best > kStill;
}

bool EvdevGamepad::pollPad(const PadHandlers& h) {
    bool any = false;
    input_event ev;
    for (size_t i = 0; i < fds_.size(); ++i) {
        while (::read(fds_[i], &ev, sizeof ev) == ssize_t(sizeof ev)) {
            pads_[i].onEvent(ev, h);
            any = true;
        }
    }
    return any;
}

}  // namespace input
