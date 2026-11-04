#include "input/rumble.h"

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace input {
namespace {

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

uint16_t mag16(float v) {  // 0..1 → 0..65535
    v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    return uint16_t(std::lround(v * 65535.f));
}

}  // namespace

bool Rumble::open() {
    close();
    for (int i = 0; i < 64; ++i) {
        char path[64];
        std::snprintf(path, sizeof path, "/dev/input/event%d", i);
        // FF 要写权限才能上传/播放：O_RDWR，只读直接跳过
        int fd = ::open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned long ff[4] = {0};
        if (::ioctl(fd, EVIOCGBIT(EV_FF, sizeof ff), ff) < 0 ||
            !(ff[FF_RUMBLE / (8 * sizeof(unsigned long))] &
              (1UL << (FF_RUMBLE % (8 * sizeof(unsigned long)))))) {
            ::close(fd);
            continue;
        }
        char name[256] = "?";
        ::ioctl(fd, EVIOCGNAME(sizeof name), name);
        fd_ = fd;
        dev_ = path;
        std::fprintf(stderr, "rumble: %s (%s)\n", dev_.c_str(), name);
        return true;
    }
    return false;
}

void Rumble::close() {
    if (fd_ < 0) return;
    if (idMove_ >= 0) ::ioctl(fd_, EVIOCRMFF, idMove_);
    if (idThunk_ >= 0) ::ioctl(fd_, EVIOCRMFF, idThunk_);
    ::close(fd_);
    fd_ = -1;
    idMove_ = idThunk_ = -1;
    lastMove_ = -1.f;
    moveOn_ = false;
}

bool Rumble::upload(int& id, float strong, float weak, int ms) {
    struct ff_effect e;
    std::memset(&e, 0, sizeof e);
    e.type = FF_RUMBLE;
    e.id = int16_t(id);  // -1 = 新建；否则原地改参数
    e.direction = 0;
    e.replay.length = uint16_t(ms);
    e.replay.delay = 0;
    e.u.rumble.strong_magnitude = mag16(strong);
    e.u.rumble.weak_magnitude = mag16(weak);
    if (::ioctl(fd_, EVIOCSFF, &e) < 0) {
        if (id < 0) std::fprintf(stderr, "rumble: EVIOCSFF 失败: %s\n", std::strerror(errno));
        return false;
    }
    id = e.id;
    return true;
}

void Rumble::play(int id, bool on) {
    struct input_event ev;
    std::memset(&ev, 0, sizeof ev);
    ev.type = EV_FF;
    ev.code = uint16_t(id);
    ev.value = on ? 1 : 0;
    if (::write(fd_, &ev, sizeof ev) != ssize_t(sizeof ev)) return;
}

void Rumble::setMove(float mag, int rateHz, float smoothMs) {
    if (fd_ < 0) return;
    const long long t0 = nowMs();
    const float dt = lastCallMs_ < 0 ? 0.016f : float(t0 - lastCallMs_) / 1000.f;
    lastCallMs_ = t0;
    mag = mag < 0.f ? 0.f : (mag > 1.f ? 1.f : mag);
    // 一阶低通：alpha = dt / (tau + dt)。摇杆速度是逐帧跳变的，不滤的话马达一档一档地顿
    const float tau = (smoothMs > 0.f ? smoothMs : 0.f) / 1000.f;
    const float a = tau <= 1e-5f ? 1.f : std::fmin(1.f, dt / (tau + dt));
    smooth_ += (mag - smooth_) * a;
    mag = smooth_;
    const long long t = t0;
    const long long gap = 1000 / (rateHz > 0 ? rateHz : 25);
    const bool significant = std::fabs(mag - lastMove_) > 0.04f;
    if (!significant && t - lastMoveMs_ < gap) return;  // 变化不大且没到刷新间隔：不发
    lastMove_ = mag;
    lastMoveMs_ = t;
    if (mag <= 0.001f) {
        if (moveOn_ && idMove_ >= 0) play(idMove_, false);  // 只停一次，别每 40ms 发一遍 stop
        moveOn_ = false;
        return;
    }
    // 底噪用弱马达为主、强马达少量：Joy-Con 的弱震是线性马达的高频"嗡"，接近滚动的质感
    if (!upload(idMove_, mag * 0.35f, mag, 0)) return;  // length=0 = 一直震到 stop
    play(idMove_, true);
    moveOn_ = true;
}

void Rumble::thunk(float strong, float weak, int ms) {
    if (fd_ < 0 || ms <= 0) return;
    if (!upload(idThunk_, strong, weak, ms)) return;
    play(idThunk_, true);
}

}  // namespace input
