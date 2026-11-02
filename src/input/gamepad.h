#pragma once
// 摇杆归一化：死区 + 8 扇区 + 8° 滞回（IMPLEMENTATION.md §6.6）
// 纯函数，不碰 evdev，可用合成样本自检
namespace input {

struct StickSample {
    float x = 0, y = 0;  // 屏幕坐标：y 向下
};

// 返回 0..7（0=E，逆时针 NE=1 N=2 …），-1 = 在死区内
class StickNormalizer {
public:
    explicit StickNormalizer(float deadzone = 0.35f, float hysteresisDeg = 8.f)
        : dz_(deadzone), hyst_(hysteresisDeg) {}

    int update(const StickSample& s);

    int sector() const { return sector_; }
    bool active() const { return sector_ >= 0; }

private:
    float dz_, hyst_;
    int sector_ = -1;
};

// 扇区 → 字母（左/右摇杆各 8 个）
const char* leftLetterFor(int sector);
const char* rightLetterFor(int sector);

}  // namespace input
