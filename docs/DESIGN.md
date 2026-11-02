# switch-ime-term 项目设计手册 (v4.2 Final)

> **核心架构**：三分区纯字符 UI + 可插拔终端解析器 + Lua 启动装配 + 运行时纯 C++ 状态机。
> **目标**：打造极致的、可运行在 Switchroot / Linux 终端下的手柄 T9 输入法。
> **字体基准**：Maple Mono NF CN（等宽，汉字 2 格），回落 Noto Sans CJK SC。

---

## 一、 总体系统架构与解耦

屏幕被划分为三大独立视口（Viewport），外加顶部/底部状态栏。所有 UI 元素均基于**纯字符网格**渲染，无任何图形化边框。

```text
┌──────────────────────────────────────────────────────────┐
│ 顶部状态栏 (SYS.TERM / 时间 / 电量)                      │
├──────────────┬────────────────────────┬──────────────────┤
│              │                        │                  │
│ 左侧手柄面板 │ 中间终端区域           │ 右侧手柄面板     │
│ (T9输入法)   │ (TerminalParser 接口)  │ (鼠标模拟/ABXY)  │
│              │                        │                  │
├──────────────┴────────────────────────┴──────────────────┤
│ 底部状态栏 (Layer / Mods / IME状态)                      │
└──────────────────────────────────────────────────────────┘
```

### 1.1 终端解析层（Terminal Backend）解耦
主程序不绑定任何特定的终端库，而是通过统一的 `TerminalParser` 接口与底层通信：
*   `feed(data, len)`：接收 PTY 字节流。
*   `getScreenGrid()`：返回二维字符网格（包含字符、颜色、宽字符标记）。
*   `getCursorState()`：返回光标位置与状态。
*   **支持实现**：`VTermParser` (基于 libvterm，稳定首选)、`GhosttyParser` (基于 libghostty-vt，性能极佳)、`DummyParser` (用于 UI 开发)。

### 1.2 Lua 与 C++ 的边界划分（极速架构）
**绝对不在高频循环中执行 Lua 解释器。**
*   **启动装配阶段**：C++ 启动 Lua 虚拟机，执行 `ui.lua`、`keymap.lua` 和 `engine.lua`。Lua 将 UI 字符矩阵、按键映射表、引擎参数通过 C API 注册给 C++，C++ 将其转换为原生的 `std::unordered_map` 和 `std::vector`。改完配置发 `SIGUSR1` 会把这套装配再跑一遍（见 §4.5）。
*   **运行阶段**：主事件循环**纯 C++ 驱动**。鼠标移动、按键高亮、状态切换完全不查 Lua，零性能损耗。
*   **回调阶段**：只有触发复杂业务逻辑（如 T9 弹窗状态切换）时，才调用 Lua 绑定的闭包。

---

## 二、 视觉规范 (Visual Spec)

*   **字体**：`Maple Mono NF CN`（ASCII 等宽、汉字正好占 2 格，1:2 网格合格）；缺字回落 `Noto Sans CJK SC`。
*   **颜色规范**：
    *   背景底色：`#08080c` (极深黑)
    *   面板底色：`#0c0c14` (深灰黑)
    *   主文本/普通状态：`#c8c8d4` (灰白)
    *   左手柄高亮：青 `#22d3ee` / 右手柄高亮：红 `#ff6b5a`
    *   符号层/魔法键：紫 `#a855f7`
    *   聚焦/弹窗选中：蓝 `#4ab8e6` (反色/下划线/底色)
*   **无边框原则**：所有按键、网格、面板，只通过字符本身的颜色、反色（背景色）和排版留白来区分，杜绝任何实线或圆角绘制。

---

## 三、 手柄交互逻辑映射

### 3.1 左手区：T9 极客输入法
*   **左摇杆 (3x3 物理映射)**：
    *   完美对应标准九键布局（1-9）。摇杆居中时，默认选中中心键 `5 (JKL)`（高亮反色）。
    *   推摇杆改变焦点格子。
*   **LB (左肩键) —— 魔法键 (Magic Key)**：
    *   **Grid 状态按下**：当前选中的格子“弹开”（其他格子变暗），进入 Popup 状态。
    *   **Popup 状态按下**：输入当前选中的字母。**不退出弹窗**，允许连击。
    *   **摇杆左右推**：在 Popup 状态下，切换该格子的三个/四个字母。
*   **空格键 (LT)：两种含义，看状态**
    *   **网格态**（没开弹窗）：敲**当前格子的数字**，走 T9 预测式输入 —— `6 4 4 2 6` → 候选「你好」。
    *   **弹窗态 / 已用字母组字**：上屏首个候选。

    > 为什么网格态是敲数字而不是选字母：T9 的本质就是「按数字、输入法猜字母」。
    > 全拼要按 5 次 n-i-h-a-o，T9 也是 5 次但每次只认数字键；模糊性交给候选排序
    > （ngram bigram，`ni|hao` 和 `mi|gan` 同为 64426，前者排第一）。
*   **DPAD (十字键) —— 修饰符**：
    *   上 = `Tab` (单击触发，不可锁定)。
    *   左 = `Ctrl` / 下 = `Alt` / 右 = `Shift`。
    *   **双击锁定**：双击 `Ctrl/Alt/Shift` 任意键，锁定该修饰符，底部状态栏亮起提示。再次双击解锁。

### 3.2 右手区：鼠标与全局控制
*   **RB (右肩键) = 鼠标左键 / RT (右扳机) = 鼠标右键**：已实现。走 `/dev/uinput` 建一个
    "switch-ime-term virtual mouse" 虚拟设备，只发 `BTN_LEFT`/`BTN_RIGHT` 的按下+抬起，
    **不移动指针** —— 指针位置留给真鼠标，手柄只负责点。
*   **右摇杆 = 鼠标移动**：**暂缓，不再回头做**。理由是验证路径太折腾——本机得用键鼠去操作
   "摇杆模拟鼠标"，绕一圈证明不了什么。RB/RT 点击即止，不再加测试。
   动作名 `mouse_move` 已绑定（`keymap.bind_axis("STICK_R", "mouse_move")`），C++ 侧故意不实现。
*   **ABXY**：
    *   `A` = 回车 (Enter)
    *   `B` = 退格 (Backspace)
    *   `X` = 删除 (Del)
    *   `Y` = 取消 (Esc)

### 3.3 修饰键与组合键规则

小屏幕的 T9 没有修饰键，电脑又必须有 —— 用**两条互不干扰的路径**解决：

| 路径 | 何时用 | 组合键能不能用 |
|---|---|---|
| 数字序列（网格态空格） | 打中文/英文正文 | 不能（数字没有大小写/控制码之分） |
| 明确字母（LB 弹窗） | 精确选字母、控制键 | **能**：Ctrl/Alt + 字母 → `0x03` / `ESC+字母` |

*   **Ctrl/Alt + 字母只在英文态生效**。中文态不抢字母，否则拼音根本打不出来。
*   **中文态大写自动按小写组字**：锁了 Shift 打 `Nihao` 仍然组 `nihao`。
*   **组字中收到不认识的控制字节（0x01~0x1a）直接吞掉**，不污染 shell。
*   **Ctrl+C 的打法**：按一下 Ctrl（sticky，用完自动释放）→ LB 进弹窗 → 摇杆选到 `c` → LB 上屏。
*   十字键：单击 = 一次性修饰（sticky），双击 400ms 内 = 锁定/解锁。

---

## 四、 Lua 配置文件参考

### 4.1 原则

* **配置就是 Lua**。C++ 侧只用 `luaL_dofile` 读一遍，读成原生结构（`cfg::Ui` /
  `cfg::KeyMap` / `cfg::Engine`），运行期主循环零 Lua。
* **一个文件分两半**：
  * **上半段 = 配置**：纯数据赋值（`ui.panel_w = 220`、`keymap.bind("A", "enter")`）。
    C++ 只认这一段 —— 键名固定，缺键就用 `src/config/lua_config.h` 里的默认值。
    想用 `if`/`for` 算值也行，但要保证执行完这些键名落到了全局表上。
  * **下半段 = 代码段**：函数、循环、表构造，用来**生成或校验**上面的配置。
    `cfg::loadProfile` 用 `luaL_dofile` 跑整个 chunk，代码段在装配期就执行完，
    它写进 `ui` / `keymap` / `engine` 的东西一样算数：

    ```lua
    -- 代码段在装配期执行，结果同样被 C++ 读到
    local function t9(rows)
        local out = {}
        for i, r in ipairs(rows) do out[i] = { tostring(i), r } end
        return out
    end
    ui.t9_alpha = t9{ ".,?", "ABC", "DEF", "GHI", "JKL",
                     "MNO", "PQRS", "TUV", "WXYZ" }
    ```

    chunk 跑完就 `lua_close`，所以代码段的副作用只发生在装配期。
* **没有事件钩子**：C++ 不会保存或回调任何 Lua 函数，`function ui.on_xxx()` 写了也不会被调用。
  需要「运行期回调」是另一个需求（与「运行期零 Lua」相冲突），当前未实现。
* **配置分两类文件**：

  | 类别 | 文件 | 管什么 |
  |---|---|---|
  | 看得见 | `config/ui.lua` | 尺寸、T9 键表、面板标签、字体/主题相关排版参数 |
  | 看得见 | `config/keymap.lua` | 物理按键 → 动作名 |
  | 看不见 | `config/engine.lua` | 终端（滚动历史）、IME（每页候选/ngram/T9）、输入（死区/滞回/双击）、日志、输出口 |

* C++ 侧入口：`cfg::loadProfile("config", prof)` 一次拿到 `prof.ui` / `prof.keymap` /
  `prof.engine`；`config/` 下三个文件都读不到才返回 `false`（此时用头文件里的默认值）。

### 4.2 看得见：`config/ui.lua`

尺寸单位 = 设计像素（1280×720 基准），实际渲染时乘窗口缩放。

| 键 | 默认 | 控制什么 |
|---|---|---|
| `ui.title` | `"SYS.TERM"` | 顶部状态栏左侧标题 |
| `ui.panel_w` | 220 | 左/右面板宽度 |
| `ui.top_bar_h` / `bot_bar_h` | 32 / 26 | 顶部、底部状态栏高度 |
| `ui.stick_box` | 180 | T9 九宫格外框边长 |
| `ui.dpad_cell` / `abxy_cell` / `shoulder_h` | 28 / 30 / 20 | 十字键格、ABXY 格、肩键条高度 |
| `ui.term_pad_x` / `term_pad_top` / `term_pad_bot` | 14 / 10 / 8 | 终端网格区四周留白 |
| `ui.cell_w` / `cell_h` | 8 / 21 | 终端字符格（决定列数/行数） |
| `ui.ime_rows` | 2 | 底部固定留给组字串/候选的行数 |
| `ui.t9_alpha` | `{"1",".,?"}…{"9","WXYZ"}` | 字母层九键：`{ 数字, 字母 }`，row-major 1..9 |
| `ui.t9_symbol` | ``{"+","-*"}…{"~","^`"}`` | 符号层九键（锁 Shift 进入） |
| `ui.labels.*` | — | `t9_title` / `dpad_title` / `mouse_title` / `abxy_title` 面板标题文字 |

### 4.3 看得见：`config/keymap.lua`

键名：`LB LT RB RT A B X Y DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT`；
动作名见 `src/config/lua_config.cpp` 的 `kActions`（`magic/space/tab/enter/backspace/
delete/escape/mod_*/lock_*/mouse_left/mouse_right`）。

```lua
keymap.bind("LB", "magic")        -- 网格 ↔ 弹窗 ↔ 上屏（连击）
keymap.bind("LT", "space")        -- 网格态 = 敲数字，否则 = 上屏首候选
keymap.bind("DPAD_UP", "tab")
keymap.bind("DPAD_LEFT", "mod_ctrl")
keymap.bind("DPAD_RIGHT", "mod_shift")
keymap.bind_double("DPAD_RIGHT", "lock_shift")   -- 双击锁定 = 符号层
keymap.bind("A", "enter")
keymap.bind("B", "backspace")
keymap.bind("X", "delete")
keymap.bind("Y", "escape")
keymap.bind("RB", "mouse_left")
keymap.bind("RT", "mouse_right")
keymap.bind_axis("STICK_R", "mouse_move")        -- 已注册，C++ 侧故意不实现
```

### 4.4 看不见：`config/engine.lua`

| 键 | 默认 | 控制什么 |
|---|---|---|
| `engine.terminal.scrollback` | 2000 | 终端向上保留多少行历史（`term::VTermBridge`） |
| `engine.ime.cand_per_page` | 7 | 底部候选条一页显示几个 |
| `engine.ime.ngram` | true | 候选是否按 bigram 上下文重排（只装配，未接线） |
| `engine.ime.t9` | true | 九键数字预测开关（只装配，未接线） |
| `engine.input.deadzone` | 0.35 | 摇杆死区（归一化后） |
| `engine.input.hysteresis_deg` | 8 | 扇区切换滞回角，防边界抖动 |
| `engine.input.double_tap_ms` | 400 | 同键二按 = 双击的判定窗口 |
| `engine.log.level` | `"warn"` | `debug/info/warn/error`（只打印，未接线） |
| `engine.log.path` | `""` | 日志文件，空 = 只写 stderr（只打印，未接线） |
| `engine.sink` | `"pty"` | `pty` 自带终端 / `uinput` 打给前台程序（只打印，未接线） |

```lua
engine = {}

engine.terminal = {
    scrollback = 2000,   -- 终端向上保留多少行历史（--legacy 的滚动回放也读它）
}

engine.ime = {
    cand_per_page = 7,   -- 底部候选条一页显示几个（IME 内部每页仍是 7，见报告说明）
    ngram         = true, -- 候选要不要用 bigram 上下文重排（目前只解析，未接线）
    t9            = true, -- 九键数字预测输入开关（目前只解析，未接线）
}

engine.input = {
    deadzone       = 0.35, -- 摇杆死区：归一化后小于它的位移当作回中
    hysteresis_deg = 8,    -- 扇区切换滞回角：防止在边界上来回抖出字母
    double_tap_ms  = 400,  -- 同一个键在这个时间内按第二下 = 双击（十字键锁定修饰）
}

engine.log = {
    level = "warn",      -- 日志级别：debug | info | warn | error（目前只打印，未接线）
    path  = "",          -- 日志文件路径，空 = 只写 stderr
}

engine.sink = "pty"       -- 输出口：pty（自带 PTY）| uinput（打到前台程序）
```

### 4.5 热切换语义

**改任何一个 `config/*.lua` 都不用重启进程**：

```fish
kill -USR1 (pgrep -x switch-ime-term)     # SIGHUP 等价
```

窗口内按 `Ctrl+Shift+R` 是同一个入口。

一次重载 = **全量重读 + 全量重建**（不做增量）：

1. `cfg::loadProfile("config", …)` 重读 `ui.lua` / `keymap.lua` / `engine.lua`；
   三个都读不到就打印一行警告、**保持旧配置继续跑**（不崩、不退出）。
   只改坏其中一个文件时，那一个文件对应的部分回落到默认值（缺键 = 默认值，不保留旧值）。
2. `Geometry` 重算（尺寸一变，终端列数/行数跟着变）。
3. 字体重建（缩放没变也重建：`FontBook::init`）。
4. T9 键表跟着新的 `ui.t9_alpha` / `ui.t9_symbol` 换掉（主循环每帧按当前层 setKeys）。
5. `TerminalParser::resize` + `PTY resize`（SIGWINCH 会送给 shell）。
6. 滚动历史上限按新的 `engine.terminal.scrollback` 重设。
7. `markDirty()`，下一帧整屏重绘。

**不会保留的状态**（重载后复位，属预期）：

* T9 焦点格（回到中心 `5(JKL)`）和弹窗状态（关闭）
* 组字串 / 候选列表 / 候选页码（`ime` 的拼音缓冲本身不清，只清 UI 侧快照）
* 终端滚动位置与选区（新循环本来就不保存这两样；`--legacy` 的 TerminalView 不走这条重载）
* 双击判定（上一颗按下的键置空）

Ctrl/Shift/Alt 的**锁定态会保留**（它挂在后端 `bd->locks` 上，不属于 UI 瞬态）。

## 五、 开发里程碑 (Roadmap)

1.  **M0 - 骨架与渲染**：完成三分区坐标划分，实现假数据填充的纯字符 UI 渲染，接入 FreeType。
2.  **M1 - 终端接入**：封装 `TerminalParser`，跑通 PTY，确保中文宽字符显示无误。
3.  **M2 - 驱动**（左摇杆已通，鼠标部分暂缓）：evdev → PadButton/扇区，左摇杆 8 扇区移动 T9 焦点。
4.  **M3 - Lua 装配与 T9**（已完成）：Lua 装配转 C++ 表；Grid/Popup 状态机 + LB 连击；
   九键拼音（数字串→候选，bigram 排序）与英文 T9（1 万词频表）。
5.  **M4 - Switch 移植**：交叉编译部署，调整死区、颜色对比度及 Framebuffer 刷新性能。
