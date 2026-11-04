# Q：双缓冲和脏矩形怎么同时要（本机实测约束下的方案）

> 命名规则：一个文件回答一个问题，`docs/Q-<主题>.md`。这份只回答「脏矩形 + 双缓冲怎么落地」。
> 前置事实见 `docs/PROBE-fbdev-lvgl.md`（fb0 单缓冲 / 4MB carveout / tegra_dc_ext 可用）。

## 1. 两个东西是正交的，别混着谈

| | 解决什么 | 手段 |
|---|---|---|
| **脏矩形** | 少写多少像素（带宽） | 只重绘/只搬运变化的矩形 |
| **双缓冲** | 什么时候写上去（时序） | 前/后缓冲，vblank 时交换 |

- 只有脏矩形、没有双缓冲 → 写入量小，但硬件扫描同一块内存，仍有撕裂（窗口小到看不见）
- 只有双缓冲、没有脏矩形 → 不撕裂，但每帧仍要拷 3.5 MB / 重绘整屏
- **两个都要 = 每帧只改小块 + 改完在 vblank 交换** → 这是终端 UI 的正确终局

## 2. 本机的两组硬约束（决定实现形态）

**fb0 路线**（现在能跑）

- `/dev/fb0` 只够一帧（`tegra_fbmem=0x400000`，一帧 3,686,400 B），`yres_virtual == yres`
- 但 `/sys/class/graphics/fb0/pan` 存在 → `tegra_fb` 实现了 `fb_pan_display`
- 想用 pan 翻页必须让 `yres_virtual` 翻倍 → 要改 boot.scr 的 `tegra_fbmem` 到 8 MB + 重打包 uImage（`mkimage`）
- **结论**：fb0 上做「真」双缓冲的成本 = 改引导 + 4 MB 保留内存 + 仍然要停 X；收益只是消掉已经看不见的撕裂 →
  **不值**。fb0 上做的是「软件双缓冲 + 脏矩形」：RAM 里一块 back canvas，front 是 fb。

**tegra_dc_ext 路线**（能做，要停 X）

- `/dev/nvmap` + `/dev/tegra_dc_0` 都能 `O_RDWR` 打开（brejce 在 video 组）
- nvmap 分配的扫描缓冲**不占 fb0 那 4 MB**，走系统内存 → 双缓冲、三缓冲都没有内存问题
- `TEGRA_DC_EXT_FLIP` 翻页 + `TEGRA_DC_EXT_EVENT_VBLANK` 事件 → 真 vsync、零撕裂
- 前提：dc 是排他占用的，X 在跑时拿不到 → 必须停合成器

## 3. 统一抽象：`present(damage)`

现有后端接口（`src/platform/display_backend.h:60-70`）是：

```cpp
beginFrame() → fillRect()/drawTexture() → endFrame()
```

`Rect` 已经有了（`:12-17`）。加三样东西就够：

```cpp
struct DamageList {                     // 纯逻辑，可单测
    void push(const Rect& r);           // 与已有矩形合并 / 超阈值就合并成全屏
    void clip(int w, int h);
    const Rect* data() const; int size() const; bool empty() const;
    long area() const;                  // 面积占比，决定要不要放弃优化
};
virtual void beginFrame(const DamageList& damage);   // 带裁剪的帧
virtual void endFrame(const DamageList& damage);     // 带脏区提交
```

**关键技巧：不改渲染循环。** `SoftCanvas` 加一个当前 clip 矩形，`fillRect/drawTexture` 进来先跟 clip 求交，
空了就早退。上层代码照旧"整屏重画"，但只有脏区的像素被真正写（`soft_canvas.cpp` 的 blendPixel 层做一次边界判断即可）。
这样 `ui::screen.cpp` / `terminal_view.cpp` 一行都不用改。

## 4. 脏区从哪来（不要做通用 damage 追踪）

在 `fillRect` 里逐个累加矩形会产生几百个碎片，合并开销比省下的拷贝还大。TUI 天然有结构，直接取：

| 脏源 | 位置 | 粒度 |
|---|---|---|
| 终端内容变化 | `TerminalParser::dirty()/clearDirty()`（`terminal_parser.h:39-41`）已有标记 | 目前整个视图 → **建议加 `dirtyRows()` 行位图**（vterm 每行本来就有回调） |
| 终端光标 | `cursor(row,col,visible)` | 2 格（旧位置 + 新位置） |
| 输入法候选行 | `ui::layout::kImeRows` = 2 行 | 1 个矩形 |
| 顶/底状态栏 | 每次状态变化 | `1280×32` / `1280×26` |
| 面板键格 | `ui::screen` 自己知道画了哪格 | 单格（`dpad_cell=28`） |
| 弹窗 | 弹窗 rect | 1 个矩形 |
| 滚动 / 换 tab / 窗口 resize | 整屏 | 直接整屏（不要强行优化） |

**最小可用版本**：先只做「终端行级 + 顶栏 + 底栏 + 整屏兜底」，四类矩形足够覆盖 95% 的帧。

## 5. 双缓冲在两个后端里的写法

### A. fbdev（软件双缓冲 + 脏矩形）

```
back  = SoftCanvas(720×1280 @ 设计尺寸)     // 已有
front = /dev/fb0 的 mmap

每帧：
  damage = 收集脏区
  beginFrame(damage)  → canvas clip = damage
  渲染（照旧整屏跑一遍，只有脏区落像素）
  endFrame(damage)    → 逐矩形逐行 memcpy: canvas → fb
                        （行宽用 fix.line_length，不要用 w*4）
```

- 撕裂仍在（硬件扫同一块），但一次只写几十 KB → 撕裂窗口 <1 ms
- 每帧收益（真实尺寸：cell 8×21、终端 101×28）：

| 场景 | 写入量 | 占整帧 |
|---|---|---|
| 整屏 | 3,686,400 B | 100% |
| 敲一个键（1 行终端） | 808×21×4 = **67,872 B** | 1.8% |
| 光标移动（2 格） | 2×8×21×4 = 1,344 B | 0.04% |
| 底栏变化 | 1280×26×4 = 133,120 B | 3.6% |
| 一个面板键格 | 28×28×4 = 3,136 B | 0.09% |

### B. tegra_dc_ext（真双缓冲 + 脏矩形 + vsync）

```
buf[0], buf[1] = nvmap(CREATE, 3.5MB each)      ← 不占 fb0 carveout
dc_flip(buf[cur])
循环：
  poll(VBLANK 事件)                              ← 等上一帧真正上屏
  cur ^= 1
  beginFrame(damage_cur)  → 画进 buf[cur]（离屏，用户看不见）
  dc_flip(buf[cur])  或  FLIP4
  damage_next = damage_cur ∪ damage_stale        ← 关键
```

**双缓冲 + 脏矩形的唯一坑**：翻转之后，另一块 buffer 是**两帧前**的内容。
如果你只把本帧脏区重绘进当前 buffer，另一块在那些区域还是旧像素，下次翻上去会闪一下旧内容。
所以每个 buffer 要各自维护一份「我比最新状态缺哪些区域」：

```
dirty[i] = 脏区累计（针对 buffer i）
画进 buffer cur 时：只重绘 dirty[cur]，画完 dirty[cur].clear()
翻转后：dirty[other] |= 这一帧发生的全部变化
```

这就是标准的 "damage per buffer" 记账，25 行左右，纯逻辑，可单测。

## 6. 改动清单（按落地顺序，每步独立可验）

1. `src/platform/damage.h`（新）：`DamageList`（push/merge/clip/area）+ `--selftest` 里 3~4 条断言
2. `soft_canvas.*`：加 `setClip(Rect)`；`fillRect/drawTexture/blendPixel` 求交早退
3. `display_backend.h`：`beginFrame/endFrame` 加 damage 参数，**给默认实现 = 忽略 damage 走旧路径**（SDL/headless 不用改）
4. `display_fbdev.cpp:102` 的 `endFrame()`：改成按 damage 逐矩形逐行 memcpy
5. `VTermParser`：`dirtyRows()` 行位图（vterm 行回调里 mark）；`terminal_view` 把脏行 → 像素矩形
6. `ui::screen`：面板键格 / 弹窗 / 顶底栏各自 `push` 矩形
7. `app.cpp` 主循环：攒 damage → `beginFrame(damage)` → 渲染 → `endFrame(damage)`；**没有 damage 就整帧跳过**
8. （可选，之后）`display_dc_ext.cpp`：nvmap 双缓冲 + FLIP + VBLANK，per-buffer damage 记账

第 7 步顺带解决"空闲帧还在重绘"的问题 —— 现在空转也在 full memcpy，改完空闲时是 0 拷贝。

## 7. 注意事项（都是本项目踩过的坑）

- **宽字符/反色**：damage 必须按**格子边界向外扩**，否则跨两格的中文右半边会被裁掉（`STATUS.md`「已知坑」第 1 条同一个成因）
- **滚动**：`VTermBridge` 滚一行等于整屏位移 → 滚动时直接整屏，别试图拆矩形
- **`beginFrame` 现在 `canvas_.clear()`**（`display_fbdev.cpp:95`）整屏清 → 改脏矩形后**不能**整屏 clear，
  否则等于每帧全屏写。要么只清脏区，要么把每个绘制调用都改成"填自己的矩形"
- **`line_length` ≠ `w*4`** 在本机恰好相等（2880 = 720×4），但代码不能依赖
- fb0 路线**不要**碰 `FBIOPUT_VSCREENINFO`（会把机器搞复位，见 `PROBE-fbdev-lvgl.md` §5）
- tegra_dc_ext 路线**必须停 X**，且这是唯一能验证「真双缓冲」的路径

## 8. 一句话结论

- **fb0 + 软件双缓冲 + 脏矩形**：现在就能做，收益 3.5 MB → 几十 KB，撕裂小到看不见，不碰引导不改硬件
- **tegra_dc_ext + nvmap 真双缓冲 + vblank**：也能做，零撕裂，但必须停 X，工作量是一个 mini-DRM（几天）
- 两者共用同一个 `present(damage)` 接口，先做前者，显示层实现换掉即可升级到后者
