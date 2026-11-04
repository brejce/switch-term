#include "app/physics.h"

#include <cmath>

namespace app::physics {
namespace {

constexpr float kDt = 1.f / 60.f;   // §4：dt 写死、不读配置（可回放的前提）
constexpr int kFieldKeys = 15;
constexpr float kIdleMag = 0.15f;   // §2.7：推杆量低于它算「松手」
constexpr float kIdleSecs = 0.3f;   // 松手这么久之后才开始回 home
constexpr float kHomeEps = 0.5f;    // 离 home 这么近就停住，免得在零点上抖
constexpr float kSettlePx = 6.f;    // 引力在键心这个范围内淡出（全强度 → 键心 0）

// 3x²-2x³：两端导数为 0，所以分段的接缝不会有速度阶跃
float smooth01(float x) {
    x = x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
    return x * x * (3.f - 2.f * x);
}

// 推杆量 → 速度倍率（PHYSICS.md §2.1）：死区之外两段，smoothstep 衔接，接缝处导数为 0。
// 内段微操（到 speedSlowEnd 才 speedLow 那么快），外段快速（越推越快，推满最快）。
float speedShape(float t, const Params& pr) {
    const float dz = pr.deadzone;
    const float se = pr.speedSlowEnd;
    if (t <= dz) return 0.f;
    if (se <= dz || se >= 1.f) return t;  // 参数配反了：退化成线性，不要除零
    if (t <= se) return pr.speedLow * smooth01((t - dz) / (se - dz));
    return pr.speedLow + (1.f - pr.speedLow) * smooth01((t - se) / (1.f - se));
}

float dist(float ax, float ay, float bx, float by) {
    const float dx = ax - bx, dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

void emit(const EventSink& sink, PhysEvent kind, int key, const Pointer& p) {
    if (sink) sink(PhysEventData{kind, key, p.x, p.y});
}

}  // namespace

void step(Pointer& p, const Field& f, const float* keyX, const float* keyY, float ix, float iy,
          const Params& pr, const EventSink& sink) {
    iy = -iy;  // 摇杆数学坐标（上 = +）→ 屏幕坐标（y 向下），和键位坐标同一套
    float mag = std::sqrt(ix * ix + iy * iy);
    if (mag > 1.f) {  // 合成事件可能给超 1 的向量：夹住，别超速
        ix /= mag;
        iy /= mag;
        mag = 1.f;
    }

    // 1. 速度目标：推杆量 → 速度（死区外两段，smoothstep 衔接：内段加速 / 外圈收细）
    float tvx = 0.f, tvy = 0.f;
    if (mag > 0.f) {
        const float spd = pr.maxSpeed * speedShape(mag, pr);
        tvx = ix / mag * spd;
        tvy = iy / mag * spd;
    }
    // 2. 惯性
    p.vx += (tvx - p.vx) * (1.f - pr.damping);
    p.vy += (tvy - p.vy) * (1.f - pr.damping);

    // 2.7 零位回落：松手够久 → 朝 home 滑，速度 = home_pull × 剩余距离/帧，上限 max_speed。
    // 按比例收回（home_pull < 1）所以越近越慢、不会冲过 home 再摆回来。
    // 吸在别的键上也照样拉：§2.7 的「无吸附」和 §5.4「松手 0.5s 回 home」互斥，后者才是要的行为——
    // 松手就是回零位，路过已吸附的键会自然发 release。
    p.idle = mag < kIdleMag ? p.idle + kDt : 0.f;
    const float hx = keyX[f.home], hy = keyY[f.home];
    if (p.idle >= kIdleSecs) {
        const float dh = dist(hx, hy, p.x, p.y);
        if (dh > kHomeEps) {
            const float v = std::fmin(pr.maxSpeed, pr.homePull * dh / kDt);
            p.vx = (hx - p.x) / dh * v;
            p.vy = (hy - p.y) / dh * v;
        } else {
            p.vx = p.vy = 0.f;
        }
    }

    // 3. 引力：场内每个键按距离衰减地拉。键心附近必须淡出（kSettlePx），
    // 否则那个「每帧硬加一把」的常量力会在键心上摆成 2px 极限环——画面就是字母键在抖
    for (int i = 0; i < kFieldKeys; ++i) {
        const int k = f.first + i;
        const float d = dist(keyX[k], keyY[k], p.x, p.y);
        if (d >= pr.attractRadius || d <= 0.f) continue;
        const float a = pr.attractPower * (1.f - d / pr.attractRadius) *
                        std::fmin(1.f, d / kSettlePx) * kDt * 60.f;
        p.vx += (keyX[k] - p.x) / d * a;
        p.vy += (keyY[k] - p.y) / d * a;
    }

    // 4. 积分
    p.x += p.vx * kDt;
    p.y += p.vy * kDt;

    // 5. 边界：本场 15 键的包围盒外扩 hover_radius
    float minX = keyX[f.first], maxX = minX, minY = keyY[f.first], maxY = minY;
    for (int i = 1; i < kFieldKeys; ++i) {
        const int k = f.first + i;
        minX = std::fmin(minX, keyX[k]);
        maxX = std::fmax(maxX, keyX[k]);
        minY = std::fmin(minY, keyY[k]);
        maxY = std::fmax(maxY, keyY[k]);
    }
    p.x = std::fmin(std::fmax(p.x, minX - pr.hoverRadius), maxX + pr.hoverRadius);
    p.y = std::fmin(std::fmax(p.y, minY - pr.hoverRadius), maxY + pr.hoverRadius);

    // 6. 吸附 / 悬停
    int n = -1;
    float dn = 1e30f;
    for (int i = 0; i < kFieldKeys; ++i) {
        const int k = f.first + i;
        const float d = dist(keyX[k], keyY[k], p.x, p.y);
        if (d < dn) {
            dn = d;
            n = k;
        }
    }
    if (p.snapped < 0) {
        if (dn < pr.snapRadius) {
            p.snapped = n;
            emit(sink, PhysEvent::Snap, n, p);
        }
    } else {
        const float d = dist(keyX[p.snapped], keyY[p.snapped], p.x, p.y);
        // 输入方向背离该键：调用方给的是「摇杆指向」，键到指针的方向和它反向 = 在往外推
        const bool back = (keyX[p.snapped] - p.x) * ix + (keyY[p.snapped] - p.y) * iy < 0.f;
        const float vmag = std::sqrt(p.vx * p.vx + p.vy * p.vy);
        if (d > pr.snapRadius * pr.releaseRatio ||
            (back && vmag > pr.releasePower * pr.maxSpeed)) {
            emit(sink, PhysEvent::Release, p.snapped, p);
            p.snapped = -1;
        }
    }
    if (p.snapped < 0) {  // 吸附时不动 hovered（§2.6：只有无吸附时才更新）
        const int h = dn < pr.hoverRadius ? n : -1;
        if (h != p.hovered) {
            p.hovered = h;
            emit(sink, PhysEvent::HoverChanged, h, p);
        }
    }
}

}  // namespace app::physics
