#pragma once
// Joy-Con 震动（Linux evdev FF 接口）。两种用法：
//   setMove(mag)  —— 连续弱震，mag 跟指针速度走，移动时有"在滚"的底噪
//   thunk()       —— 短促一下，吸附到键时"咚"地落进去
// 两个效果各占一个 FF 槽位，所以震动中的移动底噪不会被"咚"打断，反之亦然。
#include <string>

namespace input {

class Rumble {
public:
    ~Rumble() { close(); }

    // 扫 /dev/input/event*，打开第一个支持 FF_RUMBLE 且可写的设备
    bool open();
    void close();
    bool ok() const { return fd_ >= 0; }
    const std::string& device() const { return dev_; }

    // 连续底噪：mag = 0..1，0 = 停。rateHz 限流（默认 25Hz，别每帧发 ioctl）
    void setMove(float mag, int rateHz = 25);

    // 短促一下：strong/weak = 0..1，ms = 持续时长
    void thunk(float strong, float weak, int ms);

private:
    bool upload(int& id, float strong, float weak, int ms);
    void play(int id, bool on);

    int fd_ = -1;
    std::string dev_;
    int idMove_ = -1;
    int idThunk_ = -1;
    float lastMove_ = -1.f;
    long long lastMoveMs_ = 0;
    bool moveOn_ = false;
};

}  // namespace input
