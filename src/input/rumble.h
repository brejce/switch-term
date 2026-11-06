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

    // 扫 /dev/input/event*，优先打开设备名包含 preferName 的那个（如 "Left Joy-Con"），
    // preferName = nullptr 就取第一个支持 FF_RUMBLE 的设备（组合手柄走这条）。
    bool open(const char* preferName = nullptr);

    // 增益：两侧马达出厂力度不同（实测右边偏弱），用这个把两边调到一致
    void setGain(float g) { gain_ = g < 0.f ? 0.f : (g > 1.f ? 1.f : g); }
    void close();
    bool ok() const { return fd_ >= 0; }
    const std::string& device() const { return dev_; }

    // 连续底噪：mag = 0..1，0 = 停。rateHz 限流（别每帧发 ioctl）；
    // smoothMs = 一阶低通时间常数：摇杆速度是跳变的，直接喂给马达会一档一档地顿，
    // 滤过之后才是连续的"滚"。0 = 不滤。
    void setMove(float mag, int rateHz = 25, float smoothMs = 60.f);

    // 短促一下：strong/weak = 0..1，ms = 持续时长
    void thunk(float strong, float weak, int ms);

    // 一体化手柄（Combined Joy-Cons）：一个节点带两个马达，strong=左手、weak=右手（实测确认）。
    // 这两个接口只在那种设备上用，和上面的 setMove/thunk 不能混用（语义不同：
    // 单手柄的 strong/weak 是同一颗马达的低频/高频，一体化的是一个通道管一侧）
    void setMoveLR(float magL, float magR, int rateHz = 25, float smoothMs = 60.f);
    void thunkLR(float magL, float magR, int ms);

private:
    bool upload(int& id, float strong, float weak, int ms);
    void play(int id, bool on);

    int fd_ = -1;
    std::string dev_;
    int idMove_ = -1;
    int idThunk_ = -1;
    float gain_ = 1.f;
    float lastMove_ = -1.f;
    float smooth_ = 0.f;      // 低通后的底噪强度
    float smoothL_ = 0.f, smoothR_ = 0.f;  // 双通道版（左/右各一路）
    long long lastMoveMs_ = 0;
    long long lastCallMs_ = -1;
    bool moveOn_ = false;
};

}  // namespace input
