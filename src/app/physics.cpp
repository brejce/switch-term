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
constexpr float kRetargetPx = 2.f;  // 吸附期间换键要「近这么多」才换（防正跨中缝时抖动）

// 3x²-2x³：两端导数为 0，所以分段的接缝不会有速度阶跃
float smooth01(float x) {
    x = x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
    return x * x * (3.f - 2.f * x);
}

// 推杆量 → 速度倍率（PHYSICS.md §2.1）：死区之外两段。
// 内段微操：smoothstep 起手（接缝导数为 0，刚出死区不会一下窜出去）。
// 外段：u^speedPower —— 幂函数在 u=0 处导数也是 0，所以接缝仍然平滑；但斜率随 u 单调变陡，
// 越靠外越快。旧实现外段也用 smoothstep，导数在 u=0.5 见顶、到边缘又归零，
// 结果推到底和推到八成差不多快，手感"发木"。
float speedShape(float t, const Params& pr) {
    const float dz = pr.deadzone;
    const float se = pr.speedSlowEnd;
    if (t <= dz) return 0.f;
    if (se <= dz || se >= 1.f) return t;  // 参数配反了：退化成线性，不要除零
    if (t <= se) return pr.speedLow * smooth01((t - dz) / (se - dz));
    const float u = std::fmin(1.f, (t - se) / (1.f - se));
    const float p = pr.speedPower > 0.f ? pr.speedPower : 1.f;
    return pr.speedLow + (1.f - pr.speedLow) * std::pow(u, p);
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
    p.x = std::fmin(std::fmax(p.x, minX - pr.hoverRadius - pr.pointerRadius),
                     maxX + pr.hoverRadius + pr.pointerRadius);
    p.y = std::fmin(std::fmax(p.y, minY - pr.hoverRadius - pr.pointerRadius),
                     maxY + pr.hoverRadius + pr.pointerRadius);

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
        if (dn < pr.snapRadius + pr.pointerRadius) {
            p.snapped = n;
            emit(sink, PhysEvent::Snap, n, p);
        }
    } else {
        const float d = dist(keyX[p.snapped], keyY[p.snapped], p.x, p.y);
        // 脱开只看距离。原来还有个「推的方向背离吸附键且有速度就算脱开」的条件，
        // 它跟引力直接打架：球被引力拉过键心后，「键→指针」方向立刻反向，而玩家还在
        // 往同一方向推 → 每一帧都被判成「在往外推」→ release，下一帧又 snap 回来，
        // 屏幕上就是键在闪。速度也不是可靠的信号：轻推时同样会超阈值。
        if (d > pr.snapRadius * pr.releaseRatio + pr.pointerRadius) {
            emit(sink, PhysEvent::Release, p.snapped, p);
            p.snapped = -1;
            if (dn < pr.snapRadius + pr.pointerRadius) {  // 直接落到更近的键上，不空一帧
                p.snapped = n;
                emit(sink, PhysEvent::Snap, n, p);
            }
        } else if (n != p.snapped && dn + kRetargetPx < d) {
            // 更近的键胜出：滑动时选中项直接跟着走（留 2px 黏性，避免指针正跨中缝时高频抖动）
            emit(sink, PhysEvent::Release, p.snapped, p);
            p.snapped = n;
            emit(sink, PhysEvent::Snap, n, p);
        }
    }
    if (p.snapped < 0) {  // 吸附时不动 hovered（§2.6：只有无吸附时才更新）
        const int h = dn < pr.hoverRadius + pr.pointerRadius ? n : -1;
        if (h != p.hovered) {
            p.hovered = h;
            emit(sink, PhysEvent::HoverChanged, h, p);
        }
    }
}

}  // namespace app::physics
