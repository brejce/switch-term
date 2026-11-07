#include "input/evdev_touch.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace input {

bool EvdevTouch::bitSet(const unsigned long* bits, int bit) {
    constexpr int kWordBits = int(sizeof(unsigned long) * 8);
    return (bits[bit / kWordBits] & (1UL << (bit % kWordBits))) != 0;
}

int EvdevTouch::open() {
    close();
    constexpr int kWords =
        (ABS_MAX + int(sizeof(unsigned long) * 8)) / int(sizeof(unsigned long) * 8);
    for (int i = 0; i < 64; ++i) {
        char path[64];
        std::snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = ::open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned long absBits[kWords]{};
        // 能力位图判设备：内核里触屏就叫 touchscreen，只按名字筛会漏
        if (::ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absBits), absBits) < 0 ||
            !bitSet(absBits, ABS_MT_POSITION_X) || !bitSet(absBits, ABS_MT_POSITION_Y) ||
            ::ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &xInfo_) != 0 ||
            ::ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &yInfo_) != 0) {
            ::close(fd);
            continue;
        }
        char nm[256] = "?";
        ::ioctl(fd, EVIOCGNAME(sizeof nm), nm);
        // IMU（陀螺仪）/ Serial（序列号）是手柄的辅助节点；能力位图已经挡过一次，按名字再挡一次
        if (std::strstr(nm, "IMU") != nullptr || std::strstr(nm, "Serial") != nullptr) {
            ::close(fd);
            continue;
        }
        fd_ = fd;
        name_ = nm;
        return 1;
    }
    return 0;
}

void EvdevTouch::close() {
    lost_ = false;
    active_ = ending_ = havePoint_ = moved_ = false;
    remainder_ = 0;
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    name_.clear();
}

// 触摸 abs 量程直接当面板原生分辨率：真机上它就是屏的 xres/yres（fbdev 原来读的是 fb 的）。
// 走两步归一化（量程 → 原生 → 画布），跟原来那套公式逐字相同。
disp::Point EvdevTouch::point(int cw, int ch) const {
    const int xres = std::max(1, xInfo_.maximum - xInfo_.minimum);
    const int yres = std::max(1, yInfo_.maximum - yInfo_.minimum);
    const int px = (x_ - xInfo_.minimum) * std::max(1, xres - 1) / xres;
    const int py = (y_ - yInfo_.minimum) * std::max(1, yres - 1) / yres;
    switch (rot_) {
        case 90: return {py * cw / yres, ch - 1 - px * ch / xres};
        case 180: return {cw - 1 - px * cw / xres, ch - 1 - py * ch / yres};
        case 270: return {cw - 1 - py * cw / yres, px * ch / xres};
        default: return {px * cw / xres, py * ch / yres};
    }
}

void EvdevTouch::pump(std::vector<disp::ClickEvent>& clicks, int& wheel, int canvasW,
                      int canvasH) {
    if (fd_ < 0) return;
    input_event ev{};
    while (::read(fd_, &ev, sizeof ev) == ssize_t(sizeof ev)) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_TRACKING_ID) {
                if (ev.value >= 0) {
                    active_ = true;
                    ending_ = false;
                    havePoint_ = false;
                    moved_ = false;
                    remainder_ = 0;
                } else {
                    ending_ = active_;
                    active_ = false;
                }
            } else if (ev.code == ABS_MT_POSITION_X) {
                x_ = ev.value;
            } else if (ev.code == ABS_MT_POSITION_Y) {
                y_ = ev.value;
            }
        }
        if (ev.type != EV_SYN || ev.code != SYN_REPORT || (!active_ && !ending_)) continue;

        const disp::Point p = point(canvasW, canvasH);
        if (!havePoint_) {
            start_ = last_ = p;
            havePoint_ = true;
        } else if (active_) {
            // 拖动按行累加：走够一行才翻一行，剩下的留着下次（手指慢移也不会丢步）
            remainder_ += p.y - last_.y;
            last_ = p;
            const int rowPx = std::max(8, canvasH * 21 / 720);
            while (std::abs(remainder_) >= rowPx) {
                const int d = remainder_ > 0 ? 1 : -1;
                wheel += d;
                remainder_ -= d * rowPx;
            }
            moved_ = moved_ || std::abs(p.y - start_.y) >= rowPx / 2;
        }
        if (ending_) {
            // 没挪过半行 = 轻点（tab 条点按走这里）
            if (!moved_ && havePoint_) clicks.push_back({last_.x, last_.y, 1});
            ending_ = false;
            havePoint_ = false;
        }
    }
    // ENODEV = 设备没了；非阻塞读的 EAGAIN 不算
    if (errno == ENODEV) lost_ = true;
}

}  // namespace input
