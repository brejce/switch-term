#include "input/evdev_keys.h"

#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace input {

int sysKeyFromCode(int evdevCode) {
    switch (evdevCode) {
        case KEY_VOLUMEDOWN: return kSysVolDown;
        case KEY_VOLUMEUP: return kSysVolUp;
        case KEY_SLEEP: return kSysPower;  // 电源键实测上报 KEY_SLEEP（不是 KEY_POWER）
        default: return -1;
    }
}

int EvdevSysKeys::open() {
    close();
    for (int i = 0; i < 64; ++i) {
        char path[64];
        std::snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = ::open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        unsigned char absbits[(ABS_MAX + 8) / 8] = {0};
        unsigned char keybits[(KEY_MAX + 8) / 8] = {0};
        if (::ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits) < 0 ||
            ::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits) < 0) {
            ::close(fd);
            continue;
        }
        auto hasAbs = [&](int c) { return absbits[c / 8] & (1 << (c % 8)); };
        auto hasKey = [&](int c) { return keybits[c / 8] & (1 << (c % 8)); };
        // 手柄自己就报 ABS_X/Y 或 BTN_SOUTH：那台交给 EvdevGamepad，这里跳过免得同一次按键报两遍
        if ((hasAbs(ABS_X) && hasAbs(ABS_Y)) || hasKey(BTN_SOUTH)) {
            ::close(fd);
            continue;
        }
        if (!hasKey(KEY_VOLUMEDOWN) && !hasKey(KEY_VOLUMEUP) && !hasKey(KEY_SLEEP)) {
            ::close(fd);
            continue;
        }

        char name[256] = "?";
        ::ioctl(fd, EVIOCGNAME(sizeof name), name);
        // IMU（陀螺仪）/ Serial（序列号）是手柄的辅助节点；能力位图里没有按键，按名字再挡一次。
        // CEC 是 HDMI 遥控透传，它也带 114/115（电视遥控的音量），不挡会变成幽灵音量键
        if (std::strstr(name, "IMU") != nullptr || std::strstr(name, "Serial") != nullptr ||
            std::strstr(name, "cec") != nullptr || std::strstr(name, "CEC") != nullptr) {
            ::close(fd);
            continue;
        }
        fds_.push_back(fd);
        names_.push_back(name);
    }
    return count();
}

void EvdevSysKeys::close() {
    lost_ = false;
    for (int fd : fds_) ::close(fd);
    fds_.clear();
    names_.clear();
}

void EvdevSysKeys::poll(const std::function<void(int key, bool pressed)>& h) {
    input_event ev;
    for (int fd : fds_) {
        while (::read(fd, &ev, sizeof ev) == ssize_t(sizeof ev)) {
            if (ev.type != EV_KEY || ev.value == 2) continue;  // 2 = 自动重复，忽略
            int key = sysKeyFromCode(ev.code);
            if (key >= 0 && h) h(key, ev.value == 1);
        }
        // ENODEV = 设备没了；非阻塞读的 EAGAIN 不算
        if (errno == ENODEV) lost_ = true;
    }
}

}  // namespace input
