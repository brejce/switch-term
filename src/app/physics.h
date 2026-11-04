#pragma once
// 双指针引力场（docs/PHYSICS.md）：每侧 15 键一个场，摇杆推杆量 → 粒子速度 → 吸附/悬停。
// 纯函数式步进：不读写文件、不读系统时间，dt 固定 1/60 写在实现里，
// 所以同一输入序列必然得到逐位相同的轨迹和事件（可离线回放）。
#include <cmath>
#include <functional>

namespace app::physics {

// 输入侧死区。硬件「回中」读数不是 0：轴范围的几何中点和真实回中位对不上、摇杆还有漂移，
// 不滤的话推杆量永远大于 idle 阈值，松手也永远回不了零位（§2.7）。
// 只做归零、不重新缩放：半径 [0, dz) 当作没推，往外的推杆量原样交给速度曲线，
// 所以 engine.lua 里的「半径 10 = 死区 1 / 快速 5 / 外圈 4」能直接对上报值。
inline void applyDeadzone(float& ix, float& iy, float dz) {
    if (ix * ix + iy * iy <= dz * dz) ix = iy = 0.f;
}

struct Pointer {
    float x = 0, y = 0;        // 设计像素，和 ui.lua 的 ui.keys 同一套坐标（键中心）
    float vx = 0, vy = 0;      // px/s
    int snapped = -1;          // 吸附的键下标（全局 0..29），-1 = 未吸附
    int hovered = -1;          // 最近的键（无吸附时），-1 = 无
    float idle = 0;            // 推杆量 < 0.15 的累计秒数：§2.7 零位回落用
};

struct Field {                 // 一侧 15 键
    int first = 0;             // 左 = 0，右 = 15（ui.keys 的起点下标）
    int home = 7;              // 零位键：左 = 7（d），右 = 22（k）
};

// §4 的旋钮，默认值 = PHYSICS.md 的表
struct Params {
    float damping = 0.88f;      // 惯性衰减：每步向目标速度收敛 (1 - damping)
    float attractPower = 60;    // 引力强度：每帧直接给速度加这么多 px/s（满强度时）。
                                // 1（原来那个值）等于没有磁力；60 = 0.3s 内把球拽到键心。
                                // 键心附近（physics.cpp 的 kSettlePx）淡出，所以加大不会抖
    float attractRadius = 60;   // 引力作用半径（px），键距 50/56 → 能吸到邻键边缘
    float snapRadius = 18;      // 进入就吸附
    float releaseRatio = 1.6f;  // 脱开距离 = snapRadius × 它
    float releasePower = 0.35f; // 已废弃（保留字段只为旧配置不报错）：它跟引力打架导致键闪，
                                // 现在脱开只看 releaseRatio 的距离
    float maxSpeed = 900;       // px/s，推到底的时速（最快）
    // 推杆量 → 速度（死区之外两段，smoothstep 衔接，接缝处导数为 0）：
    // 内段 [deadzone, speedSlowEnd] 从 0 槽到 speedLow（微操：内圈再推也只有这么慢）；
    // 外段 [speedSlowEnd, 1] 从 speedLow 槽到 1.0（长距离：越推越快，推满最快）。
    float deadzone = 0.1f;      // = engine.input.deadzone（app 里同步）：曲线从死区边缘起算
    float speedSlowEnd = 0.5f;  // 微操段结束的推杆量（推杆半径 10 就是 5）
    float speedLow = 0.3f;      // 微操段顶端的速度（× maxSpeed）
    float hoverRadius = 26;     // 无吸附时的最近键判定
    float homePull = 0.6f;      // 零位回落：每帧收回剩余距离的比例（< 1 才不冲过头）
    // 指针球半径（px）：0 = 原点模型（完全兼容旧行为）。>0 时吸附/悬停/脱开判定都是
    // 「球心到键心距离 < 键半径 + 它」，即球边缘碰键才算，键也更早被点亮。
    float pointerRadius = 0.f;
};

enum class PhysEvent { HoverChanged, Snap, Release };

struct PhysEventData {
    PhysEvent kind;
    int key;      // snap = 吸上哪个键；release = 放开哪个键；hover_changed = 新最近的键（-1 = 无）
    float x, y;   // 事件发生时指针的位置
};

using EventSink = std::function<void(const PhysEventData&)>;

// keyX/keyY：30 个键的中心（设计像素），下标 = ui.keys 的下标（本函数只碰本场那 15 个）。
// ix/iy：与 input::PadState::stickVec 同一套——数学坐标（上 = +y）、长度 0..1、死区已在外面滤掉。
// 每帧调一次；事件按状态变化在内部触发，sink 可以为空。
void step(Pointer& p, const Field& f, const float* keyX, const float* keyY, float ix, float iy,
          const Params& pr, const EventSink& sink = {});

}  // namespace app::physics
