#include "input/rumble.h"

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

bool Rumble::open(const char* preferName) {
    close();
    // 按可信度分三档挑，名字没对上也不至于两边都落到同一台设备：
    //   3 名字里含 preferName（原来的子串匹配）
    //   2 名字里有同侧的 left/right 或 (l)/(r)——换手柄后名字常变成 "Joy-Con (L)"
    //   1 这台设备有同侧摇杆轴（左 = ABS_X，右 = ABS_RX）
    //   0 只要支持 FF_RUMBLE 就用（左右一体的组合手柄）
    const std::string want = preferName ? preferName : "";
    auto lower = [](const char* s) {
        std::string o;
        for (const char* c = s; *c; ++c) o += char(std::tolower((unsigned char)*c));
        return o;
    };
    const std::string wantLow = lower(want.c_str());
    const char* sideWord = wantLow.find("left") != std::string::npos
                               ? "left"
                               : (wantLow.find("right") != std::string::npos ? "right" : nullptr);
    int bestTier = -1, bestFd = -1;
    std::string bestDev;
    for (int i = 0; i < 64; ++i) {
        char path[64];
        std::snprintf(path, sizeof path, "/dev/input/event%d", i);
        // FF 要写权限才能上传/播放：O_RDWR，只读直接跳过
        // 不用 O_NONBLOCK：这个 fd 只写不读，非阻塞写在某些 FF 驱动上会 EAGAIN
        int fd = ::open(path, O_RDWR);
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
        // IMU / Serial 是同一台手柄的辅助节点：名字里含 "Left Joy-Con" 一样会被认成手柄，
        // 但它们没有马达，绑上去就是"震动没反应"
        if (std::strstr(name, "IMU") != nullptr || std::strstr(name, "Serial") != nullptr) {
            ::close(fd);
            continue;
        }
        const std::string nameLow = lower(name);
        int tier = 0;
        if (!want.empty() && std::strstr(name, want.c_str())) {
            tier = 3;
        } else if (sideWord != nullptr) {
            if (nameLow.find(sideWord) != std::string::npos ||
                nameLow.find(std::string("(") + sideWord[0] + ")") != std::string::npos) {
                tier = 2;
            } else {  // 名字看不出来，就按「有没有同侧摇杆轴」认
                unsigned char abs[(ABS_MAX + 8) / 8] = {0};
                const int ax = sideWord[0] == 'l' ? ABS_X : ABS_RX;
                if (::ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs) >= 0 &&
                    (abs[ax / 8] & (1 << (ax % 8))))
                    tier = 1;
            }
        }
        if (tier > bestTier) {
            if (bestFd >= 0) ::close(bestFd);
            bestFd = fd;
            bestTier = tier;
            bestDev = std::string(path) + " (" + name + ")";
            if (tier >= 3) break;  // 名字对上了就不用再猜
        } else {
            ::close(fd);
        }
    }
    if (bestFd < 0) return false;
    fd_ = bestFd;
    dev_ = bestDev;
    std::fprintf(stderr, "rumble: %s%s\n", dev_.c_str(),
                 bestTier >= 2 ? "" : "（名字没对上，按摇杆轴/FF 猜的）");
    return true;
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
    e.u.rumble.strong_magnitude = mag16(strong * gain_);
    e.u.rumble.weak_magnitude = mag16(weak * gain_);
    if (::ioctl(fd_, EVIOCSFF, &e) < 0) {
        if (id < 0) std::fprintf(stderr, "rumble: EVIOCSFF 失败: %s\n", std::strerror(errno));
        return false;
    }
    if (std::getenv("SIT_DEBUG") && (id < 0 || e.id != id))
        std::fprintf(stderr, "rumble[%s]: effect id=%d ms=%d\n", dev_.c_str(), e.id, ms);
    id = e.id;
    return true;
}

void Rumble::play(int id, bool on) {
    struct input_event ev;
    std::memset(&ev, 0, sizeof ev);
    ev.type = EV_FF;
    ev.code = uint16_t(id);
    ev.value = on ? 1 : 0;
    if (::write(fd_, &ev, sizeof ev) != ssize_t(sizeof ev)) {
        std::fprintf(stderr, "rumble[%s]: 播放失败 id=%d on=%d: %s\n", dev_.c_str(), id, int(on),
                     std::strerror(errno));
        return;
    }
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

void Rumble::setMoveLR(float magL, float magR, int rateHz, float smoothMs) {
    if (fd_ < 0) return;
    const long long t0 = nowMs();
    const float dt = lastCallMs_ < 0 ? 0.016f : float(t0 - lastCallMs_) / 1000.f;
    lastCallMs_ = t0;
    auto cl = [](float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); };
    const float tau = (smoothMs > 0.f ? smoothMs : 0.f) / 1000.f;
    const float a = tau <= 1e-5f ? 1.f : std::fmin(1.f, dt / (tau + dt));
    smoothL_ += (cl(magL) - smoothL_) * a;
    smoothR_ += (cl(magR) - smoothR_) * a;
    const long long gap = 1000 / (rateHz > 0 ? rateHz : 25);
    const bool significant =
        std::fabs(smoothL_ - lastMove_) > 0.04f || std::fabs(smoothR_ - lastMove_) > 0.04f;
    if (!significant && t0 - lastMoveMs_ < gap) return;
    lastMove_ = smoothL_ > smoothR_ ? smoothL_ : smoothR_;  // 复用一个"上次值"做粗滤
    lastMoveMs_ = t0;
    if (smoothL_ <= 0.001f && smoothR_ <= 0.001f) {
        if (moveOn_ && idMove_ >= 0) play(idMove_, false);
        moveOn_ = false;
        return;
    }
    if (!upload(idMove_, smoothL_, smoothR_, 0)) return;  // strong=左 weak=右
    play(idMove_, true);
    moveOn_ = true;
}

void Rumble::thunkLR(float magL, float magR, int ms) {
    if (fd_ < 0 || ms <= 0) return;
    if (!upload(idThunk_, magL * gain_, magR * gain_, ms)) return;
    play(idThunk_, true);
}

void Rumble::thunk(float strong, float weak, int ms) {
    if (fd_ < 0 || ms <= 0) return;
    if (!upload(idThunk_, strong, weak, ms)) return;
    play(idThunk_, true);
}

}  // namespace input
