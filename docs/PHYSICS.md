# PHYSICS · 双指针行星引力（v1 规格）

实现目标：两个摇杆各驱动一个「粒子」，在各自 20 键（5 列 × 4 行；分层里的空键不参与）的引力场里飞；
吸附只高亮，上屏仍由 L（LB）确认。

## 1. 数据

```cpp
struct Pointer {
    float x, y;        // 设计像素坐标（跟 ui.lua 的 ui.keys 同一坐标系）
    float vx, vy;      // px/s
    int   snapped;     // 吸附的键下标（全局 0..39），-1 = 未吸附
    int   hovered;     // 最近的键（无吸附时），-1 = 无
};

struct Field {         // 一侧 20 键（5 列 × 4 行）
    int first;         // 左 = 0，右 = 20（ui.keys 的起点下标）
    int home;          // 零位键：左 = 12（d），右 = 32（k）
};
```

## 2. 每帧（固定 dt = 1/60）

输入：摇杆归一化向量 `(ix, iy)`，长度 0..1（**往外辐射的半径**）。进 `step()` **之前**先过
`app::physics::applyDeadzone(ix, iy, engine.input.deadzone)`：硬件回中读数不是 0（轴几何中点 ≠ 真实
回中位，还有漂移），半径落在死区内就滤成 0——`idle` 才会累积、松手才会回零位。只归零、不重新缩放，
所以下面折线的推杆量和摇杆实际推出去的比例一一对应（半径 10 = 死区 1.5 / 快速 5 / 外圈 5）。

1. **速度目标**：`mag = |(ix,iy)|`；`spd = max_speed * shape(mag)`；`tv = dir * spd`。
   `shape()` 是**两段 smoothstep**（死区之外，接缝处导数为 0，所以没有速度阶跃）：
   - 内段 `[deadzone, speed_slow_end]`：0 → `speed_low`，微操区。内圈再推也只这么快，轻推挪一个键
     （死区 0.15 → 半径 10 的 1.5→5 段，顶端 = 0.15 × max_speed）
   - 外段 `[speed_slow_end, 1.0]`：`speed_low` → 1.0，快速区。越推越快，推满最快（半径 5→10）

   单调递增、只有中段一个拐点：符合「推得越满走得越快」的直觉；落点精度靠吸附半径，不靠降速。

   `deadzone` 由 `engine.input.deadzone` 同步进 `Params`：曲线必须从死区边缘起算，否则边界处会从 0
   直接跳到某个速度。
2. **惯性**：`v = v + (tv - v) * (1 - damping)`（`damping=0.26` → 每步收敛 74%）
3. **引力**：对场内每个键 `k`，`d = |pos - center(k)|`；若 `d < attract_radius`：
   `v += unit(k - pos) * attract_power * (1 - d/attract_radius) * min(1, d/6px) * dt * 60`。
   末尾那个 `min(1, d/6px)` 是**键心淡出**：常量引力在键心上是硬推力，离散积分会留下约 2px 的极限环
   （画面上就是字母键一起抖），淡出之后球会稳稳停住
4. **积分**：`pos += v * dt`
5. **边界**：把 `pos` 夹在**本场有效键**的包围盒里（硬夹，不按半径外扩）。不是全 20 键：分层里
   空键很多（NUM 层左右各只剩 5~8 颗），所以空键既不参与包围盒、包围盒的**种子也取第一颗有效键**——
   取 `key[f.first]` 时它往往是空键，包围盒会一路扩到整行空槽上，指针就能停在没有键的地方。
   夹住的轴连速度一起归零（否则指针不动、底噪震动还顶格）。
   旧实现外扩 `hover_radius + pointer_radius`（≈60px），指针会被留在键区外的空白里：最近的角键
   被点亮却吸不住，看着就像按在一个不存在的键上。
6. **吸附判定**：取最近的键 `n`，距离 `dn`
   - `dn < snap_radius + pointer_radius`（= 55）且无吸附 → `snapped = n`，发 `snap`
   - 已吸附且 `d > snap_radius * release_ratio + pointer_radius` → `snapped = -1`，发 `release`
     （**只看距离**；原来还有「推的方向背离该键」的判据，它与引力打架会导致吸附键逐帧闪，见 §9.8）
   - 吸附中若另一个键明显更近（近 2px 以上）→ 直接换键，滑动时选中项跟着指针走
   - 无吸附时 `hovered = (dn < hover_radius + pointer_radius) ? n : -1`；变化时发 `hover_changed`
7. **空槽收拢**：既没吸附也没高亮（指针停在包围盒里的空槽），就朝最近的真键收 25%/帧
   （速度同时减半，最多 2×`attract_radius` 之外不收），保证只有真按键能停住指针。
8. **零位回落**：输入 `mag < 0.15` 持续 0.3s 且无吸附 → 施加向 `home` 的弱引力（`home_pull`），让它自己滑回 `d`/`k`

对应到画面上（`ui::drawKeyHand`）：每个键按同一份 `attract_radius` 算 `pull = 1 - d/radius`，边框 →
底色 → 字色依次向强调色（左青/右红）混过去。所以推摇杆时看到的就是真实受力：先 `f` 亮、推更深
`g` 才亮；松开后指针滑回零位，亮度也跟着退回 `d`。

**相机**（`app.cpp` 的主循环，不在 `step()` 里，每侧一个）：目标 = 吸附键中心 + `0.30 × (指针 − 键心)`；
没吸附时 = 最近的高亮键；连高亮都没有（指针落在分层空槽里）= **原地不动**（绝不对准空白）。
不夹取：选中的键钉在面板准心上，边缘留白接受。详见 `LAYOUT-v2.md` §3.5。

## 3. 事件

```cpp
enum class PhysEvent { HoverChanged, Snap, Release };
struct PhysEventData { PhysEvent kind; int key; float x, y; };
```
上层只关心：`snap` → 高亮 + 震动；`release` → 取消高亮；`hover_changed` → 弱高亮。

## 4. 参数（`config/engine.lua` → `engine.physics`）

下表是现在跑的现场值；C++ 里 `Params` 的默认值只是缺文件时的兜底。
改手感就是改 `config/engine.lua` 这十几个数，`systemctl restart switch-ime-term` 生效。

| 键 | 现用 | 含义 |
|---|---|---|
| `dt` | 1/60 | 固定步长，写死不读配置（保证可回放） |
| `damping` | 0.26 | 惯性衰减：越小越跟手（每步向目标速度收敛 `1-它`） |
| `attract_power` | 117 | 引力强度：每帧加多少 px/s（键心 6px 内淡出，所以加大不会抖） |
| `attract_radius` | 76 | 引力作用半径（px）＝键窗点亮半径（键距 58×62，够到邻键中心） |
| `snap_radius` | 32 | 吸附半径（px）；实际判据是 `snap_radius + pointer_radius` = 55，横向键距 58 |
| `release_ratio` | 1.3 | 脱开距离 = `snap_radius × 它 + pointer_radius` ≈ 65px（约 1.1 个横向键距） |
| `hover_radius` | 37 | 弱高亮半径：只比 snap 大 5px，「变亮」≈「马上吸附」 |
| `max_speed` | 1730 | px/s，推到底的时速（最快） |
| `deadzone` | 0.15 | 曲线起点（= `engine.input.deadzone`，app 里同步，不单独配） |
| `speed_slow_end` | 0.5 | 微操段结束的推杆量（半径 10 的 5） |
| `speed_power` | 1.6 | 外段曲线指数：1 = 线性，>1 = 越靠外斜率越大（旧实现用 smoothstep，斜率到边缘归零，推到底和八成一样快） |
| `speed_low` | 0.15 | 微操段顶端的速度（× max_speed）：×1730 ≈ 260px/s，内圈手感与旧值不变 |
| `home_pull` | 0.6 | 零位回落：每帧收回剩余距离的比例 |
| `pointer_radius` | 23 | **指针球半径**（px）。0 = 原点模型（旧行为）。>0 时吸附/悬停/脱开判据一律变成 `球心距 < 该半径 + pointer_radius`，即**球边缘碰键**才算 |
| ~~`release_power`~~ | 0.35 | **已废弃**，保留字段只为旧配置不报错，见 §9.8 |

调手感的对应关系（实测）：

| 想要 | 改哪个 |
|---|---|
| 更"咔"、更早吸住 | `pointer_radius` ↑（现 23，直径 46 ≈ 键宽 48；再大会吃掉两键之间的过渡）或 `snap_radius` ↑ |
| 太糊、误吸邻键 | `pointer_radius` ↓（→0 退回原点模型） |
| 更跟手、少滑行 | `damping` ↓（0.26→0.20） |
| 外圈还想更猛 | `speed_power` ↑（1.6→2.0）或 `max_speed` ↑ |
| 吸住后换键更费劲 / 更松 | `release_ratio` ↑ / ↓ |

## 4.5 震动（`engine.rumble`）

Joy-Con 走 evdev FF（`--audit-input` 不管这个；实测 `event8` 支持 `FF_RUMBLE`）。
两份效果各占一个 FF 槽，互不打断：

| 键 | 现用 | 含义 |
|---|---|---|
| `enabled` | true | 关掉就完全不开 FF 设备 |
| `move_max` | 0.30 | 移动底噪最大强度 |
| `move_speed_ref` | 500 | 指针速度到这么多 px/s 算推满 → 映射到 `move_max` |
| `move_smooth_ms` | 60 | **底噪一阶低通时间常数**：摇杆速度逐帧跳变，直接喂马达会一档一档地顿 |
| `rate_hz` | 40 | 底噪刷新率上限（每帧 ioctl 太重） |
| `snap_strong` | 0.55 | 吸附那一下：强马达（低频"咚"） |
| `snap_weak` | 0.30 | 吸附那一下：弱马达（高频"嗒"） |
| `snap_ms` | 55 | 持续时长；太长会和下一颗键的"咚"糊在一起 |
| `gain_left` | 0.50 | 左手柄马达增益（实测右侧偏弱，所以压左边而不是抬右边——FF 幅度已是 0xFFFF 上限） |
| `gain_right` | 1.00 | 右手柄马达增益 |

改这几项要 `systemctl restart switch-ime-term`（FF 槽在启动时上传，热重载不覆盖）。

**分侧震动**：左右 Joy-Con 各打开一台 FF 设备（`rumble.open("Left Joy-Con")` / `"Right Joy-Con"`，
按名字分四档挑：名字对上(3) → 同侧 left/right 或 (l)/(r)(2) → 有同侧摇杆轴(1) → 任意 `FF_RUMBLE`(0)），
所以左手键区的事件只震左手柄、右手只震右手柄，底噪也各算各的。名字含 `IMU` / `Serial` 的**辅助节点直接跳过**
——它们同样叫 `Left Joy-Con`，但没有马达，绑上去就是「震动没反应」。组合设备 `event8` 只在分侧设备
打不开时兜底（tier 0）；左右两条路径落到同一台设备时会打一行提示。

权限：这两个串口设备默认是 `0600 root:input`，`70-sit-input.rules` 没吃到，另加了
`/etc/udev/rules.d/99-sit-joycon-ff.rules`；service 里还有一道 `chmod 660 /dev/input/event*`
兜底（Joy-Con 重连会重建节点），程序每 2s 重试打开。

## 5. 可测性（硬要求）

- 纯函数式步进：`Pointer` + `Field` + 参数 → `step()`，不含 I/O、不含时间调用（dt 由外部传）
- `--audit-physics` 用脚本化序列断言：
  1. 零位静止 → 吸附在 `home`（`d` / `k`）
  2. 向右推 0.5s → 吸附键从 `d` 移向 `f`/`g`（下标递增）
  3. 速度曲线：内圈（0.3）比快速段（0.8）慢、0.8 比 0.48 快、1.0 最快；接缝两侧（0.48/0.52）耗时差 ≤2 帧（无阶跃）
  4. 松手 0.5s 后回到 `home`；引力能把球吸到键心，且松手后**不许残留摆动**（摆幅 <0.5px）
  5. 向左推再反向 → 触发 `release` 事件
- 同样的输入序列跑两次，轨迹与事件序列**逐字节相同**

## 6. 文件

```
src/app/physics.h       Pointer / Field / Params / step / 事件回调
src/app/physics.cpp     实现（无 I/O）
src/input/evdev_gamepad.h   加 stickVec(right) -> {dx,dy}（拿归一化向量，不是扇区）
src/config/lua_config.*     Engine.physics 那 10 个字段
config/engine.lua           physics 段
src/main.cpp                --audit-physics
```

## 7. 不做（本轮）

- 不接主循环、不改渲染、不做震动、不做指针绘制
- 不改输入法路径

## 8. 下一轮（接主循环时）

- `app.cpp` 每帧喂 `stickVec` → `step` → `AppState.hoverIdx/snapIdx`
- 渲染：吸附键反色 + 指针小圆点
- `L`（LB）上屏 `ui.layers[当前层][snapIdx]`
- `snap` 事件接 `FF_RUMBLE`（`event10` 已验证支持 `FF_RUMBLE/PERIODIC/SINE/GAIN`）

---

## 9. 实现与本文档的偏差（以代码为准）

v1 落地时发现规格本身有 7 处坑，实现按「能同时满足 §5 断言」的方式处理，之后以本节为准：

1. **`Pointer` 多一个 `idle` 字段**：§2.7 要求“持续 0.3s”，需要累计计时；`idle` = 连续 `< 0.15` 的秒数。
2. **§2.7 与 §5.4 直接冲突**：§2.7 写“零位回落要求且无吸附”，但 §5.4 要求“松手 0.5s 回 home”。实测按“无吸附才回落”实现时 §5.4 永远不过（松手时通常正吸附在某键上）。
   **实现选了 §5.4**：只要松手 ≥0.3s 就拉回 home，吸在别的键上也拉，路过时会自然发 `release`。
3. **`home_pull` 是“每秒收敛倍率”，不是“引力强度”**：实现为 `v_home = min(max_speed, home_pull × 到 home 的距离 / dt)`，即每帧收回剩余距离的 `home_pull`。若按 §2.3 的加速度写法用，0.5s 只能挪几像素，和 §5.4 不可能同时成立。
4. **可配置字段是 10 个**（`dt` 写死在 `physics.cpp`，Lua 表里不出现）。§4 的 11 行含 `dt`。
5. **§5.3 的“穿越 3 键”按 `d→f→g` 量**（`home+3` 会绕到下一行行首 `z`），测的是 `home+2` 的 x 距离（116px，即 2 × 横向键距 58）。实测：内圈 900 帧 / 快速段 5 帧 / 推满 3 帧（越快推越少帧，内圈循环到上限）。
6. **边界只夹位置、不清速度**：推到底贴着场边缘直到松手被拉回。
7. **`release_power` 取 `0.35`**（`LAYOUT-v2.md §6` 的旧表写 `1.8`，以本文档为准；旧表未同步）。

8. **`release_power` 已废弃（吸附键闪烁的根因）**：旧实现里脱开条件除了距离，还有
   「推的方向背离吸附键且速度超阈」。引力会把球拉过键心，此后「键→指针」方向立刻反向，
   而玩家仍在同向推 → 每帧判成"在往外推" → `release`，下一帧又 `snap` 回来，屏幕上就是键在闪。
   速度阈值也不可靠（轻推同样超）。现在：**脱开只看 `release_ratio` 的距离**，并且吸附期间
   **就近换键**（更近的键胜出就切，留 2px 黏性防跨中缝抖动），所以滑动时选中项连续跟随指针。
   `--audit-physics` 的「滑动选键不闪」是这条的回归测试（从 `d` 推到邻键，snap 次数必须 1..8）。

实测基线（`--audit-physics`，全部通过）：
```
零位吸附 左 d(12) / 右 k(32)        OK
右推 0.5s 下标递增（最大 snap=14）  OK
右推 0.5s 指针右移 Δx=116.0         OK
内圈 900 帧 / 快速段 5 帧 / 推满 3 帧  OK
接缝两侧 16 帧 vs 15 帧（差 ≤2）     OK
松手 0.5s 回 d(12)，距 home=0.0      OK
引力把球吸到键心 12 帧，距 0.0       OK
反向触发 release×6                  OK
滑动选键不闪 snap×4                 OK
同序列两次逐位相同（19 条事件）     OK
摇杆向量 右=+x 上=+y                OK
死区内 / 死区边界滤成 0，外不缩放   OK
```
