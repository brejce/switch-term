# switch-ime-term 项目设计手册 (v4.2 Final)

> **核心架构**：三分区纯字符 UI + 可插拔终端解析器 + Lua 启动装配 + 运行时纯 C++ 状态机。
> **目标**：打造极致的、可运行在 Switchroot / Linux 终端下的手柄输入法。
> **字体基准**：Maple Mono NF CN（等宽，汉字 2 格），回落 Noto Sans CJK SC。
> **视觉基调**：`docs/DESIGN-BRIEF.md`（white.execute 来稿的 LCD 彩色改编）；**屏幕规格**：
> `UI_DESIGN.md`（几何/颜色语义/候选区规则）；颜色值只在 `src/theme/palette.cpp`。

---

## 一、 总体系统架构与解耦

屏幕被划分为三大独立视口（Viewport），外加顶部/底部状态栏。所有 UI 元素均基于**纯字符网格**渲染，无任何图形化边框。

```text
┌──────────────────────────────────────────────────────────┐
│ 顶部状态栏 (SYS.TERM / 时间 / 电量)                      │
├──────────────┬────────────────────────┬──────────────────┤
│              │                        │                  │
│ 左侧手柄面板 │ 中间终端区域           │ 右侧手柄面板     │
│ (20 键键区)  │ (TerminalParser 接口)  │ (光标/ABXY)      │
│              │                        │                  │
├──────────────┴────────────────────────┴──────────────────┤
│ 底部状态栏 (LAYER / IME / MODS)                          │
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
*   **回调阶段**：只有触发复杂业务逻辑时才调用 Lua 绑定的闭包。

### 1.3 终端实例与 tab（多终端）

「只能开一个终端」曾经只是 `main` 里的写法问题：解析器与 PTY 本来就是实例化的（`createParser()` 返回新对象，`Pty` 持有自己的 fd/pid）。现在容器是 `app.cpp` 里的 `std::vector<Term>` + `active`。

*   **tab 切换/新建/关闭不走按键**：在终端里敲 `term [名字]` / `quit` / `exit`。前两个是启动时生成在 `PATH` 里的 sh 脚本，它们只往 stdout 打 `ESC ] 1337 ; newtab=…` / `quit` 控制序列，由 `src/term/osc_filter` 剥出来执行（于是不会在屏上显示乱码）。
*   同一个剥除器顺带支持标准标题序列 `OSC 0/1/2`，所以 fish 自己设的标题会直接成为 tab 名。
*   **每帧必须读所有 tab 的 PTY**，只画 `active`——不读的非活动 tab 会写满缓冲而卡死。
*   完整约束（四条不变量、OSC 协议表、`--run` 特例、上限 16）见 `IMPLEMENTATION.md` §4.4。
*   切换组合键尚未分配；`Ctrl+N` 与 readline 冲突，动手前先看 `IMPLEMENTATION.md` §4.4 末尾。

---

## 二、 视觉规范 (Visual Spec)

*   **字体**：`Maple Mono NF CN`（ASCII 等宽、汉字正好占 2 格，1:2 网格合格）；缺字回落 `Noto Sans CJK SC`。
*   **颜色规范**：全部来自主题令牌（`src/theme/palette.cpp`，默认 `switch`；另有 `light` / `gruvbox`）。
    调用点只写 `col::kXxx`，**不许在渲染代码里出现裸 hex**。完整语义表见 `UI_DESIGN.md` §二。

    | 令牌 | `switch` 值 | 语义 |
    |---|---|---|
    | `kBg` / `kPanel` / `kTerm` | `#05070c` / `#151a24` / `#0b0f16` | 底 / 面板（凸）/ 终端（凹）：**结构靠明度分层** |
    | `kLine` | `#3e4756` | 所有 1px 分隔线（键框、区带） |
    | `kText` / `kDim` / `kFaint` | `#ccd2dc` / `#8891a1` / `#5a6272` | 主文 / 次文 / 装饰 |
    | `kCyan` / `kRed` | `#46c5d3` / `#e87e66` | 左手 / 右手（活跃侧强调、受力点亮） |
    | `kBlue` | `#5f9fd0` | 光标、组字下划线、候选选中 |
    | `kYellow` / `kPurple` / `kGreen` | `#d9ab5c` / `#a78bea` / `#5cc98f` | 修饰键与数字层 / 符号层 / 成功・电量 |

*   **颜色只带语义**：同屏实心色块最多 2 个（吸附键 + 选中候选/模式 chip）；禁止渐变发光、
    扫描线纹理、悬浮卡片。
*   **无边框原则**：所有按键、网格、面板，只通过字符本身的颜色、反色（背景色）和排版留白来区分，杜绝任何实线或圆角绘制。

> 注：本节以前抄的是 `#08080c / #22d3ee / #4ab8e6` 那套老值，已随主题重做作废；
> 对比度标准（bg/panel 1.16:1、line/键面 1.64:1、text/bg 13.3:1）写在 `palette.cpp` 的注释里。

---

## 三、 手柄交互逻辑映射

### 3.1 键区：双摇杆 + 引力吸附

*   **左/右摇杆**：各驱动一个指针，在各自 5 列 × 4 行（20 键）的键场里按引力场运动（`docs/PHYSICS.md`）。
    死区外推杆量决定速度（内圈慢→中段快→推满最快），松手后滑回零位（左 `d` / 右 `j`）。
*   **吸附**：指针是有半径的球，球边缘碰到键（`snap_radius + pointer_radius`）就吸附，整键反色；
    邻键按真实受力点亮，所以推力方向不用十字准星也能看出来。吸附期间"就近换键"，滑动不闪。
*   **窗口**：一屏只露 3 列 × 4 行，列跟着吸附键水平滑，行一次全露（数字行永远在屏上，靠遮罩压暗）。
    几何与字符表 → `docs/LAYOUT-v2.md`。
*   **LB**：上屏**活跃侧**吸附住的字符（带 `Shift` 取上档），没吸附就不动作。
*   **LT**：组字中 = 上屏当前选中的候选；否则 = 空格。
*   **RB / RT**：组字中 = 移动候选选中项；Tab 窗口内（DPAD_UP 后 1.2s）= 切 tab；
    带 `Shift` = 光标上/下；其余 = 光标左/右。
*   **L3**：键区层循环 ALPHA → NUM → SYM。
*   **DPAD（十字键）—— 修饰符**：上 = `Tab`（顺带开 tab 窗口）/ 左 = `Shift` / 右 = `Alt` / 下 = `Ctrl`；
    单击 = 一次性（sticky），双击 = 锁定/解锁；锁定时面板上那颗变实心块、底栏亮 `MODS`。
*   **＋（START）**：`onboard`，拉起屏幕键盘（桌面模式被打断时兜底）。

### 3.2 右手区：ABXY 与虚拟鼠标

*   **ABXY**：`A` = 回车 / `B` = 退格 / `X` = Del / `Y` = Esc（真机摆位：X 上、Y 左、A 右、B 下）。
*   **虚拟鼠标（可选，默认没绑）**：动作 `mouse_left` / `mouse_right` 在 `app.cpp` 里是"只点不移动"
    （`/dev/uinput` 发 `BTN_LEFT` / `BTN_RIGHT`）。`config/keymap.lua` 默认把 RB/RT 绑给了光标左/右，
    要换回鼠标改两行即可；但 `/dev/uinput` 是 `0600 root:root`，打不开只会打一行警告、点了没反应
    （`STATUS.md` §3.5）。
*   **右摇杆 = 鼠标移动**：**暂缓，不再回头做**（得用本机键鼠去操作"摇杆模拟鼠标"，绕一圈证明不了什么）。
    动作名 `mouse_move` 已绑定（`keymap.bind_axis("STICK_R", "mouse_move")`），C++ 侧故意不实现。

### 3.3 修饰键与组合键规则

小屏幕手柄没有修饰键，电脑又必须有。现在只有**一条**路径：摇杆选键 + LB 上屏。

*   **单击 = 一次性（sticky），双击 400ms 内 = 锁定/解锁**；锁定态在左面板点亮、底栏亮 `MODS [CTRL]`。
    ⚠「用完自动释放」还没实现：`sticky` 只翻转不清零，按一下等于锁死（`STATUS.md` §3.1）。
*   **Ctrl/Alt + 字母只在英文态生效**；中文态不抢字母，否则拼音打不出来。
*   **中文态大写自动按小写组字**：锁了 Shift 打 `Nihao` 仍然组 `nihao`。
*   **组字中收到不认识的控制字节（0x01~0x1a）直接吞掉**，不污染 shell。
*   **Ctrl+C**：按一下 DPAD_DOWN（Ctrl sticky）→ 摇杆选 `c` → LB 上屏。
*   **中英切换**：只由 `Ctrl+空格`（DPAD_DOWN + LT）翻转，`keymap.lua` 里没有也不提供 `ime` 动作。

> 旧的「数字序列（网格态空格）/ LB 弹窗」两条路径已随九键弹窗一起删除（2026-11），
> 不要再按那套写文档或加回动作名。

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
    local function keys(rows)
        local out = {}
        for i, r in ipairs(rows) do out[i] = { tostring(i), r } end
        return out
    end
    ui.keys_alpha = keys{ ".,?", "ABC", "DEF", "GHI", "JKL",
                     "MNO", "PQRS", "TUV", "WXYZ" }
    ```

    chunk 跑完就 `lua_close`，所以代码段的副作用只发生在装配期。
* **没有事件钩子**：C++ 不会保存或回调任何 Lua 函数，`function ui.on_xxx()` 写了也不会被调用。
  需要「运行期回调」是另一个需求（与「运行期零 Lua」相冲突），当前未实现。
* **配置分两类文件**：

  | 类别 | 文件 | 管什么 |
  |---|---|---|
  | 看得见 | `config/ui.lua` | 尺寸、键位与层、面板标签、字体/主题相关排版参数 |
  | 看得见 | `config/keymap.lua` | 物理按键 → 动作名 |
  | 看不见 | `config/engine.lua` | 终端（滚动历史）、IME（每页候选/ngram）、输入（死区/双击）、日志、输出口 |

* C++ 侧入口：`cfg::loadProfile("config", prof)` 一次拿到 `prof.ui` / `prof.keymap` /
  `prof.engine`；`config/` 下三个文件都读不到才返回 `false`（此时用头文件里的默认值）。

### 4.2 看得见：`config/ui.lua`

尺寸单位 = 设计像素（1280×720 基准），实际渲染时乘窗口缩放。

| 键 | 默认 | 控制什么 |
|---|---|---|
| `ui.title` | `"SYS.TERM"` | 顶部状态栏左侧标题 |
| `ui.panel_w` | 220 | 左/右面板宽度 |
| `ui.top_bar_h` / `bot_bar_h` | 32 / 26 | 顶部、底部状态栏高度 |
| `ui.stick_box` | 180 | legacy 九宫格遗留：只喂旧路径的 `keyCell`，v2 渲染不读 |
| `ui.dpad_cell` / `abxy_cell` / `shoulder_h` | 28 / 30 / 20 | 十字键格、ABXY 格、肩键条高度 |
| `ui.term_pad_x` / `term_pad_top` / `term_pad_bot` | 14 / 10 / 8 | 终端网格区四周留白 |
| `ui.cell_w` / `cell_h` | 8 / 21 | 终端字符格（决定列数/行数） |
| `ui.ime_rows` | 2 | 底部固定留给组字串/候选的行数 |
| `ui.labels.*` | — | `dpad_title` / `abxy_title` 面板标题文字 |

### 4.3 看得见：`config/keymap.lua`

键名：`LB LT RB RT A B X Y DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT L3 START`
（`src/config/lua_config.cpp` 的 `kKeys`）；
动作名见同文件 `kActions`：`none magic space tab enter backspace delete escape
mod_ctrl/mod_alt/mod_shift lock_ctrl/lock_alt/lock_shift cursor_left/cursor_right
layer_next onboard mouse_left/mouse_right next_tab/prev_tab/new_tab/close_tab`。

```lua
-- 实际内容见 config/keymap.lua（这里是摘录，以那个文件为准）
keymap.bind("LB", "magic")        -- 上屏活跃侧吸附住的字符（Shift 取上档）
keymap.bind("LT", "space")        -- 组字中 = 上屏选中候选，否则 = 空格
keymap.bind("L3", "layer_next")   -- ALPHA → NUM → SYM
keymap.bind("DPAD_UP", "tab")
keymap.bind("DPAD_LEFT", "mod_shift")
keymap.bind("DPAD_RIGHT", "mod_alt")
keymap.bind("DPAD_DOWN", "mod_ctrl")
keymap.bind_double("DPAD_LEFT", "lock_shift")    -- 双击锁定修饰（不是切层）
keymap.bind("A", "enter")
keymap.bind("B", "backspace")
keymap.bind("X", "delete")
keymap.bind("Y", "escape")
keymap.bind("RB", "cursor_left")                 -- R：光标左 / 候选选中项上一条
keymap.bind("RT", "cursor_right")                -- ZR：光标右 / 候选选中项下一条
keymap.bind("START", "onboard")
keymap.bind_axis("STICK_R", "mouse_move")        -- 已注册，C++ 侧故意不实现
```

### 4.4 看不见：`config/engine.lua`

| 键 | 默认 | 控制什么 |
|---|---|---|
| `engine.terminal.scrollback` | 2000 | 终端向上保留多少行历史（`term::VTermBridge`） |
| `engine.ime.cand_per_page` | 7 | 底部候选条一页显示几个 |
| `engine.ime.ngram` | true | 候选是否按 bigram 上下文重排（只装配，未接线） |
| `engine.ime.enabled` | true | 内置输入法是否介入；`false` = 字符原样透传（桌面模式与 fcitx5 共存，见 §4.4.1） |
| `engine.input.deadzone` | 0.35 | 摇杆死区（归一化后） |
| `engine.input.hysteresis_deg` | 8 | 扇区切换滞回角，防边界抖动 |
| `engine.input.double_tap_ms` | 400 | 同键二按 = 双击的判定窗口 |
| `engine.log.level` | `"warn"` | `debug/info/warn/error`（只打印，未接线） |
| `engine.log.path` | `""` | 日志文件，空 = 只写 stderr（只打印，未接线） |
| `engine.sink` | `"pty"` | 输出口：`pty` 写自带伪终端 / `uinput` 当虚拟键盘打给系统（需 `/dev/uinput` 可写，打不开自动回落 `pty`） |

```lua
engine = {}

engine.terminal = {
    scrollback = 2000,   -- 终端向上保留多少行历史（--legacy 的滚动回放也读它）
}

engine.ime = {
    cand_per_page = 7,   -- 底部候选条一页显示几个（IME 内部翻页跨度跟着变）
    ngram         = true, -- 候选要不要用 bigram 上下文重排（目前只解析，未接线）
    enabled       = true, -- false = 内置输入法不介入，字符原样透传（桌面模式与 fcitx 共存）
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

engine.sink = "pty"       -- 输出口：pty = 写自带伪终端；uinput = 当虚拟键盘打给系统（需 /dev/uinput 可写，打不开自动回落 pty）
```

#### 4.4.1 桌面模式（与 fcitx5 共存）

Switch/TTY 上本程序自己就是输入法（内置拼音 → 写 PTY）。带桌面的 Linux 上 fcitx5 已经在做
同一件事，两个输入法同时介入 = 双重输入、抢快捷键。这时用这两个键把职责让出去：

| `engine.sink` | `engine.ime.enabled` | 用途 | 行为 |
|---|---|---|---|
| `pty` | `true` | **默认**：Switch / 裸 TTY | 内置拼音写 PTY，与今天完全一致 |
| `pty` | `false` | 桌面窗口里当终端，输入法交给 fcitx5 | 字符原样写 PTY（只做 Ctrl/Alt/Shift 转换），不组字、不切语言 |
| `uinput` | `false` | **只当虚拟键盘**：手柄按键 = 系统输入事件 | 按键经 `/dev/uinput` 打给当前焦点程序；PTY 仍照常 spawn 并显示输出（只当监视器） |
| `uinput` | `true` | 罕见：内置拼音上屏后交给系统 | 组字/选词在本程序内，上屏结果经 uinput 打给焦点程序（中文会被丢弃，见下） |

防冲突的三条原则：

1. **不注册任何全局热键**。程序不抓 X/Wayland 键盘，不抢 fcitx5 的组合键。
2. `ime.enabled = false` 时**不注入任何修饰组合**（尤其不替用户按 `Ctrl+Space`）：
   `Ctrl+Space`（字节 `0x00`）在直通路径上返回空且不消耗，切换键完整留给 fcitx5。
3. `uinput` **打不开就自动回落 `pty`**：打印一行警告（需要 root 或 input 组 / udev 规则），
   把 `sink` 强制改回 `pty`，继续跑。

**硬限制**：`uinput` 只能发 `EV_KEY` 1..127（US 布局的键码），打不出 Unicode。
非 ASCII 的 UTF-8 字符直接跳过并计数，退出时打印一次
`uinput: 丢弃 N 个非 ASCII 字符（中文需要 fcitx5/wtype，见 DESIGN §4.4）`。
中文出路：让 fcitx5 走 D-Bus 接（M6），或在 Wayland 下用 `wtype` 送文本。

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
5. `TerminalParser::resize` + `PTY resize`（SIGWINCH 会送给 shell）。
6. 滚动历史上限按新的 `engine.terminal.scrollback` 重设。
7. `markDirty()`，下一帧整屏重绘。

**不会保留的状态**（重载后复位，属预期）：

* 组字串 / 候选列表 / 候选页码（`ime` 的拼音缓冲本身不清，只清 UI 侧快照）
* 终端滚动位置与选区（新循环本来就不保存这两样；`--legacy` 的 TerminalView 不走这条重载）
* 双击判定（上一颗按下的键置空）

Ctrl/Shift/Alt 的**锁定态会保留**（它挂在后端 `bd->locks` 上，不属于 UI 瞬态）。

## 五、 开发里程碑 (Roadmap)

1.  **M0 - 骨架与渲染**：完成三分区坐标划分，实现假数据填充的纯字符 UI 渲染，接入 FreeType。
2.  **M1 - 终端接入**：封装 `TerminalParser`，跑通 PTY，确保中文宽字符显示无误。
3.  **M2 - 驱动**：evdev → PadButton + 摇杆向量，摇杆驱动引力场指针。
4.  **M3 - Lua 装配与输入法**（已完成）：Lua 装配转 C++ 表；拼音组字/候选 + ngram 重排。
5.  **M4 - Switch 移植**：交叉编译部署，调整死区、颜色对比度及 Framebuffer 刷新性能。

## 六、 视觉令牌与对比度基线（2026-11-04 重做）

设计原则：**外壳安静，内容说话**。结构靠明度分层拉开，颜色只承担两件事——"这是选中的"
和"这是左手还是右手"。术语与取舍来自 `ui-ux-pro-max` 的 Terminal CLI / Developer Tool 规则，
数值是本机实测（脚本见本文末）。

### 6.1 明度阶梯（`switch` 主题）

| 令牌 | 值 | 实测对比度 | 说明 |
|---|---|---|---|
| `bg` | `#05070c` | — | 最底层（近黑，不做纯黑） |
| `panel` | `#151a24` | vs bg **1.16:1** | 面板抬起一层 |
| `term` | `#0b0f16` | vs bg **1.05:1** | 终端凹槽（**必须比面板暗**，不能亮） |
| 键面 `kkBg` | `#1f2531` | vs panel **1.13:1** | 键帽表面 |
| `line` | `#3e4756` | vs 键面 **1.64:1** | 键框/分隔线 |
| `text` | `#ccd2dc` | vs bg **13.3:1** | 主文字 |
| `dim` | `#8891a1` | vs bg **6.3:1** | 副字/标签 |
| `faint` | `#5a6272` | vs bg **3.3:1** | 只做装饰，**不承载信息** |

旧值实测：`panel/bg 1.03:1`、`line/panel 1.13:1`（键框看不见）、`dim/bg 2.99:1`（副字读不清）。
这就是"画面糊"的全部原因——不是字号、不是圆角。

### 6.2 颜色用法

- **饱和色只有两个**：`cyan #46c5d3`（选中 / 左手）、`red #e87e66`（右手）。
- 面板标题、肩键提示、DPAD/ABXY 的字形一律 **`dim` 中性**；按下→反白，锁定→`accent`。
- 终端 ANSI 0–15 **不动**（否则 vim/ls 配色会变）。

### 6.3 测量脚本（改色前后都跑一遍）

```python
def lum(h):
    r,g,b=(h>>16)&255,(h>>8)&255,h&255
    f=lambda c:(c/255)/12.92 if c/255<=0.03928 else (((c/255)+0.055)/1.055)**2.4
    return 0.2126*f(r)+0.7152*f(g)+0.0722*f(b)
def cr(a,b):
    la,lb=lum(a),lum(b); hi,lo=max(la,lb),min(la,lb); return (hi+0.05)/(lo+0.05)
```

判据：正文 ≥4.5:1，副字 ≥4.5:1，装饰 ≥3:1；面的分层 1.1–1.2:1 刚好"看得出但不跳"，
键框 1.6–1.8:1 才看得见。

