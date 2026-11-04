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

    // 连续底噪：mag = 0..1，0 = 停。rateHz 限流（别每帧发 ioctl）；
    // smoothMs = 一阶低通时间常数：摇杆速度是跳变的，直接喂给马达会一档一档地顿，
    // 滤过之后才是连续的"滚"。0 = 不滤。
    void setMove(float mag, int rateHz = 25, float smoothMs = 60.f);

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
    float smooth_ = 0.f;      // 低通后的底噪强度
    long long lastMoveMs_ = 0;
    long long lastCallMs_ = -1;
    bool moveOn_ = false;
};

}  // namespace input
