# PROPOSAL · 物理/引力 UI（外部提案，仅存构想）

> 最后更新：2026-11-03。定位：**外部模型写的交互构想存档**，不是本项目的实现规格。
>
> 这份提案是外部模型在不了解本项目的情况下写的；已核实其与环境冲突的部分（GL/ImGui/Wayland/DBus、
> 运行期 Lua 回调、JSON 布局），保留仅作交互构想参考；真正采纳的只有「行星引力选键」这一条，
> 见 `LAYOUT-v2.md` §4/§6（两个独立物理场、`snap` 只高亮不上屏、参数走 `engine.physics`）。
> 实际约束：本机无 `/dev/dri`、跑 X11 或 fbdev；运行期零 Lua、无事件钩子（`ARCH.md` §1/§6.1）。

---

这个架构思路非常高级且极具扩展性！你实际上是在设计一个 **“带有物理引擎的、脚本化驱动的、支持多后端的输入法框架”**。

这种 **“C++ 核心引擎 + Lua 前端表现 + 独立输入法核心”** 的解耦架构，不仅能完美实现你想要的“水滴引力”交互，还能让你未来轻松更换皮肤、调整布局，甚至接入完全不同的输入法引擎（比如从内置的 Papers3 切换到 Rime 或 Fcitx5）。

结合你提供的 `KEYMAP.md` 和之前的交互设想，我为你梳理了这套**四层解耦架构**的设计方案：

---

### 🏗️ 整体架构分层设计

我们将系统分为四个完全解耦的层，它们之间通过明确的 API 和事件总线进行通信。

#### 1. 硬件与系统 I/O 层 (C++ 底层)
这一层只负责“脏活累活”，与具体的业务逻辑完全无关。
*   **输入读取 (`evdev`)**：读取 Joy-Con 的 `EV_KEY`（按键）和 `EV_ABS`（摇杆），进行基础的死区过滤和归一化。
*   **震动控制 (`FF_*`)**：接收来自上层的“触觉事件”（如：吸入、脱离、确认），转化为 Joy-Con 的 HD 震动波形。
*   **输出注入 (`uinput` / `IME 协议`)**：将最终的字符或控制指令注入系统（模拟键盘输入，或通过 Wayland/X11 的文本输入协议发送给当前焦点的应用）。
*   **系统 IME 桥接**：通过 DBus 或 IPC 与系统输入法（如 Fcitx5 / IBus）通信，获取候选词和**词注**。

#### 2. 核心引擎层 (C++ 逻辑层)
这是整个系统的大脑，负责物理计算和状态管理。
*   **物理引擎 (Physics Engine)**：
    *   实现“水滴/行星引力”模型。维护“飞行器”的坐标、速度向量。
    *   计算按键的引力场、吸附判定、脱离阻力。
    *   输出当前“飞行器”悬停/吸附的**目标按键 ID**。
*   **状态机与事件分发 (Event Bus)**：
    *   将底层的物理键码（如 `310 BTN_TL`）转化为**逻辑基础事件**（如 `EVENT_LB_DOWN`）。
    *   维护修饰键状态（根据 `KEYMAP.md`，处理 Shift/Ctrl/Alt 的单次与锁定逻辑）。
*   **Lua 虚拟机 (Lua VM)**：
    *   使用 `sol2` 或 `LuaBridge` 将 C++ 的物理状态、按键事件暴露给 Lua。
    *   接收 Lua 传来的 UI 布局数据和渲染指令。

#### 3. 输入法核心层 (IME Core)
独立于 UI 和物理引擎，专注于“拼音/英文 -> 候选词”的转换。
*   **内置引擎**：你实现的 Papers3 同款算法。
*   **统一数据结构**：无论底层是内置引擎还是系统 Fcitx5，都向上层输出统一格式的候选词列表：
    ```cpp
    struct Candidate {
        std::string text;      // 候选词文本，如 "架构"
        std::string comment;   // 词注/注音/翻译，如 "jià gòu" 或 "architecture"
        int weight;            // 权重
    };
    ```

#### 4. 前端 UI 与表现层 (Lua + 渲染后端)
这一层完全由 Lua 脚本驱动，渲染后端（如 SDL2/OpenGL 或 ImGui）只负责“无脑”绘制 Lua 传来的数据。
*   **布局定义 (Lua)**：定义左右分体键盘的坐标、每个按键的 `gravity_radius`（引力半径）。
*   **主题与风格 (Lua)**：定义“水滴”的颜色、形变参数、涟漪效果的持续时间。
*   **交互逻辑 (Lua)**：定义 `L` 键 (Magic) 按下后，到底是要上屏字母，还是要切换键盘层。

---

### 🔄 核心数据流演示

让我们以 **“用户推左摇杆，被 `A` 键吸入，然后按下 `L` (Magic) 确认”** 为例，看看数据是如何在各层流动的：

1.  **硬件层**：`evdev` 捕获到左摇杆的 `EV_ABS` 变化，计算出归一化的方向向量 `(dx, dy)`，传给**核心引擎**。
2.  **核心引擎 (物理)**：物理引擎根据 `(dx, dy)` 更新“飞行器”坐标。发现飞行器进入 `A` 键的 `snap_radius`，触发**吸附**。引擎将当前 `hovered_key_id = "A"` 记录在状态中。
3.  **核心引擎 (震动)**：引擎向硬件层发送 `HAPTIC_SNAP` 事件，Joy-Con 发出“哒”的一声短震。
4.  **核心引擎 (Lua 回调)**：引擎通知 Lua：“当前悬停在 `A` 键”。
5.  **前端 UI (Lua)**：Lua 脚本收到通知，向渲染后端发送指令：“将 `A` 键渲染为高亮融合的水滴状态”。
6.  **硬件层**：`evdev` 捕获到 `L` 键 (`310 BTN_TL`) 按下，转化为 `EVENT_LB_DOWN`。
7.  **前端 UI (Lua)**：Lua 脚本中定义的 `on_key_down("LB")` 被触发。Lua 查询物理引擎的 `hovered_key_id`，发现是 `"A"`。
8.  **输入法层**：Lua 将 `"A"` 发送给输入法核心。如果是英文模式，直接上屏；如果是拼音模式，加入预编辑区，输入法核心返回候选词 `[{text="啊", comment="a"}, {text="阿", comment="a"}]`。
9.  **前端 UI (Lua)**：Lua 将候选词及**词注**排版，交给渲染后端画在屏幕上方。

---

### 💻 技术栈与落地建议

为了实现这个架构，推荐以下 C++ 技术栈：

1.  **Lua 绑定**：**`sol2`** (C++17 兼容极好，API 极其优雅，性能高)。
2.  **渲染后端**：
    *   如果需要极度轻量且跨平台：**`SDL2` + `SDL2_gfx`** 或 **`Dear ImGui`** (ImGui 画水滴和引力场非常方便，且自带 Lua 绑定)。
    *   如果需要炫酷的 2D 粒子/流体效果：**`Raylib`** 或直接用 **`OpenGL`/`Vulkan`** 写 Shader。
3.  **系统输入法接入**：
    *   在 Linux 下，接入 Fcitx5 最标准的方式是通过 **`libfcitx5core`** 或者走 **`DBus`** 调用其输入法协议。
4.  **配置与布局格式**：
    *   虽然 UI 用 Lua，但基础的键盘布局（坐标、引力参数）可以考虑用 **`JSON`** 或 **`YAML`** 编写，然后由 Lua 读取并实例化。这样修改布局不需要懂 Lua 语法。

---

### 🎨 Lua 前端脚本示例 (概念)

为了让你感受一下这种解耦带来的自由度，你的 Lua 脚本大概会长这样：

```lua
-- 1. 定义物理布局 (左右分体)
local left_keys = {
    { id="q", x=10, y=20, label="q" },
    { id="a", x=10, y=60, label="a", gravity=1.2 }, -- A键引力大一点
    -- ...
}
physics.register_zone("left", left_keys)

-- 2. 定义主题风格
theme.set({
    water_color = {0.2, 0.8, 1.0, 0.9},
    snap_ripple_duration = 0.3,
    show_candidate_comment = true -- 开启词注显示
})

-- 3. 定义按键映射逻辑 (结合 KEYMAP.md)
function on_key_down(logical_key)
    if logical_key == "magic" then -- 对应物理键 L
        local target = physics.get_hovered_key()
        if target then
            ime.commit_char(target.label)
            haptics.play("commit")
        end
    elseif logical_key == "lock_shift" then -- 对应十字键左双击
        ime.toggle_shift()
        theme.flash_indicator("SHIFT_LOCKED")
    end
end

-- 4. 处理输入法候选词渲染
function on_candidates_update(candidates)
    ui.clear_candidate_bar()
    for i, cand in ipairs(candidates) do
        -- 渲染候选词，并带上词注 (comment)
        ui.draw_candidate(i, cand.text, cand.comment) 
    end
end
```

### 下一步怎么走？

这个架构非常庞大且迷人。为了稳步推进，我建议我们分步走：

1.  **第一步：搭建 C++ 骨架与 Lua 绑定**。先跑通一个“摇杆移动虚拟光标，Lua 接收坐标并打印”的最小原型。
2.  **第二步：实现物理引擎**。把“水滴引力、吸附、脱离”的 C++ 代码写出来，并用简单的控制台字符画验证手感。
3.  **第三步：接入渲染与输入法**。把画面画出来，并接入你的 Papers3 引擎。

# 这里面说的papers3，是指之前papers3-ecs-ui里面的基础输入法的实现。
