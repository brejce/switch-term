#include "input/evdev_gamepad.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <chrono>
#include <cmath>
#include <cstring>


namespace input {

float GamepadMapper::norm(int v, const Range& r) const {
    if (r.maxV == r.minV) return 0.f;
    float f = float(v - r.minV) / float(r.maxV - r.minV);  // 0..1
    return f * 2.f - 1.f;                                 // -1..1
}

void GamepadMapper::setAxisRange(int code, int minV, int maxV) {
    Range r{minV, maxV};
    int mid = (minV + maxV) / 2;  // 初值必须落在中心，否则开机会先吐一串假字母
    switch (code) {
        case ABS_X: lx_ = r; lxV_ = mid; break;
        case ABS_Y: ly_ = r; lyV_ = mid; break;
        case ABS_RX: rx_ = r; rxV_ = mid; break;
        case ABS_RY: ry_ = r; ryV_ = mid; break;
        default: break;
    }
}

// 十字方向：单按 = 一次性修饰（sticky），400ms 内同方向再按 = 锁定
void GamepadMapper::dpadKey(int dir, int stickyA, int lockA, int /*bit*/, int stickyB, int lockB,
                            const GamepadHandlers& h) {
    long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    bool again = (dir == lastDpadDir_) && (now - lastDpadMs_ < 400);
    int a = (dir == 1 || dir == 4) ? (again ? lockA : stickyA) : (again ? lockB : stickyB);
    if (a) h.action(a);
    lastDpadDir_ = dir;
    lastDpadMs_ = now;
}

void GamepadMapper::onEvent(const input_event& ev, const GamepadHandlers& h) {
    if (ev.type == EV_ABS) {
        switch (ev.code) {
            case ABS_X: lxV_ = ev.value; break;
            case ABS_Y: lyV_ = ev.value; break;
            case ABS_RX: rxV_ = ev.value; break;
            case ABS_RY: ryV_ = ev.value; break;
            case ABS_HAT0X:  // 左/右 = Ctrl / Shift
                if (ev.value == 0 || !h.action) break;
                dpadKey(ev.value < 0 ? 1 : 2, kGpActStickyCtrl, kGpActLockCtrl, 2,
                        kGpActStickyShift, kGpActLockShift, h);
                break;
            case ABS_HAT0Y:  // 上 = Tab，下 = Alt
                if (ev.value < 0 && h.bytes) h.bytes("\t");
                if (ev.value > 0 && h.action)
                    dpadKey(4, kGpActStickyAlt, kGpActLockAlt, 4, 0, 0, h);
                break;
            default: break;
        }
    } else if (ev.type == EV_KEY && ev.value == 1) {  // 只处理按下
        switch (ev.code) {
            case BTN_SOUTH: if (h.bytes) h.bytes("\x7f"); break;         // 物理 B（下）= 退格
            case BTN_EAST: if (h.bytes) h.bytes("\r"); break;           // 物理 A（右）= 回车
            case BTN_NORTH: if (h.bytes) h.bytes("\x1b[3~"); break;     // X = Delete
            case BTN_WEST: if (h.bytes) h.bytes("\x1b"); break;         // Y = ESC
            case BTN_TL: if (h.action) h.action(kGpActLayer); break;    // L = 切层
            case BTN_TR: if (h.bytes) h.bytes(" "); break;              // R = 空格
            case BTN_TL2: if (h.action) h.action(kGpActCursorLeft); break;
            case BTN_TR2: if (h.action) h.action(kGpActCursorRight); break;
            // joycond 合成设备没有 HAT 轴，十字键走 BTN_DPAD_*（AUDIT-x11-kde §1）
            case BTN_DPAD_LEFT:
                if (h.action) dpadKey(1, kGpActStickyCtrl, kGpActLockCtrl, 2,
                                      kGpActStickyShift, kGpActLockShift, h);
                break;
            case BTN_DPAD_RIGHT:
                if (h.action) dpadKey(2, kGpActStickyCtrl, kGpActLockCtrl, 2,
                                      kGpActStickyShift, kGpActLockShift, h);
                break;
            case BTN_DPAD_UP: if (h.bytes) h.bytes("\t"); break;
            case BTN_DPAD_DOWN:
                if (h.action) dpadKey(4, kGpActStickyAlt, kGpActLockAlt, 4, 0, 0, h);
                break;
            default: break;
        }
    } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
        // 摇杆在 SYN 时结算：evdev 的 Y 一般「上 = 最小」，正好和屏幕坐标 y 向下同向
        StickSample l{norm(lxV_, lx_), norm(lyV_, ly_)};
        int s = left_.update(l);
        if (s >= 0 && s != lastL_ && h.bytes) h.bytes(leftLetterFor(s));
        lastL_ = s;

        StickSample r{norm(rxV_, rx_), norm(ryV_, ry_)};
        int rs = right_.update(r);
        if (rs >= 0 && rs != lastR_ && h.bytes) h.bytes(rightLetterFor(rs));
        lastR_ = rs;
    }
}

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
            case ABS_HAT0X:
                if (ev.value < 0 && h.onButton) h.onButton(kPadLeft);
                if (ev.value > 0 && h.onButton) h.onButton(kPadRight);
                break;
            case ABS_HAT0Y:
                if (ev.value < 0 && h.onButton) h.onButton(kPadUp);
                if (ev.value > 0 && h.onButton) h.onButton(kPadDown);
                break;
            default: break;
        }
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
        fds_.push_back(fd);
        names_.push_back(name);
        GamepadMapper m;
        for (int code : {ABS_X, ABS_Y, ABS_RX, ABS_RY}) {
            input_absinfo ai{};
            if (::ioctl(fd, EVIOCGABS(code), &ai) == 0) m.setAxisRange(code, ai.minimum, ai.maximum);
        }
        mappers_.push_back(m);
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
    mappers_.clear();
    pads_.clear();
}

bool EvdevGamepad::stickVec(bool right, float& dx, float& dy) const {
    // 默认轴范围下「回中」算出来是 1.5e-5 而不是 0，所以按噪声水平当零处理。
    // 不这么滤的话，只有左摇杆的那半个 Joy-Con 会用这个噪声值抢掉右摇杆的结果。
    constexpr float kStill = 1e-3f;
    dx = dy = 0.f;
    for (const PadState& p : pads_) {
        float x = 0, y = 0;
        if (!p.stickVec(right, x, y)) continue;
        if (std::fabs(x) < kStill && std::fabs(y) < kStill) continue;
        dx = x;
        dy = y;
        return true;
    }
    return false;
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

bool EvdevGamepad::poll(const GamepadHandlers& h) {
    bool any = false;
    input_event ev;
    for (size_t i = 0; i < fds_.size(); ++i) {
        while (::read(fds_[i], &ev, sizeof ev) == ssize_t(sizeof ev)) {
            mappers_[i].onEvent(ev, h);
            any = true;
        }
    }
    return any;
}

}  // namespace input
