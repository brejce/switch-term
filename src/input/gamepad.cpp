#include "input/gamepad.h"

#include <cmath>

namespace input {
namespace {
constexpr float kRad2Deg = 57.2957795f;

float norm360(float a) {
    while (a < 0) a += 360.f;
    while (a >= 360.f) a -= 360.f;
    return a;
}

float angDiff(float a, float b) {
    float d = norm360(a - b);
    return d > 180.f ? d - 360.f : d;
}
}  // namespace

int StickNormalizer::update(const StickSample& s) {
    float x = s.x, y = -s.y;  // 屏幕坐标 y 向下 → 数学坐标
    if (std::sqrt(x * x + y * y) < dz_) {
        sector_ = -1;
        return -1;
    }
    float ang = norm360(std::atan2(y, x) * kRad2Deg);  // 0=右，逆时针
    // 滞回：已在某扇区时，偏离中心超过 22.5° + h/2 才换扇区
    if (sector_ >= 0 && std::fabs(angDiff(ang, sector_ * 45.f)) <= 22.5f + hyst_ / 2.f)
        return sector_;
    sector_ = int(std::floor(norm360(ang + 22.5f) / 45.f)) % 8;
    return sector_;
}

const char* leftLetterFor(int sector) {
    static const char* kLeft[8] = {"n", "h", "i", "e", "a", "u", "g", "z"};
    return (sector >= 0 && sector < 8) ? kLeft[sector] : "";
}

const char* rightLetterFor(int sector) {
    static const char* kRight[8] = {"y", "x", "s", "q", "d", "c", "j", "v"};
    return (sector >= 0 && sector < 8) ? kRight[sector] : "";
}

}  // namespace input
