# ARCH · 模块边界与解耦现状

> 最后更新：2026-11-03。定位：**代码怎么分层、谁依赖谁、哪里漏了、哪些设计已作废**。
> 现状与下一步看 `../STATUS.md`；机器实测事实看 `DEVICE.md`；布局权威规格看 `LAYOUT-v2.md`。
> 合并自早期的实现规范与设计手册（TOML/epoll/DRM-EGL/fcitx5-D-Bus 那套已被现实取代，
> 作废部分在 §10 存档，不删只是为了让旧结论可查）。原文 git 历史可查。

## 1. 分层与依赖方向

现行主循环只有一条：`main.cpp` 解析参数、选后端，然后进 `src/app/app.cpp` 的 `run()`
（`--legacy` 才走 `ui::panels` + `ui::terminal_view` 的老路径，见 §8）。

```text
main.cpp ──┬─ cfg::loadProfile("config")            src/config/lua_config.*
           ├─ disp::DisplayRegistry::create(name)   src/platform/display_*
           └─ app::run(bd)                          src/app/app.cpp
                ├─ term::Pty            (fork + TIOCSWINSZ)      src/term/pty.*
                ├─ term::TerminalParser (createParser → vterm)   src/modules/terminal/*
                ├─ term::oscFilter      (OSC 1337 剥除/标题)      src/term/osc_filter.*
                ├─ input::EvdevGamepad → PadState → cfg::KeyMap  src/input/*
                ├─ ime::BuiltinIme                              src/ime/*
                ├─ ui::screen 绘制 → render::Renderer → SoftCanvas → DisplayBackend
                └─ engine.sink：pty（默认）/ uinput（虚拟键盘）
```

**依赖方向（改代码时守住）**

- `ui/` 只吃 `cfg::Ui` + `AppState`，不认 PTY、不认 evdev。
- 纯映射 / 纯状态机层**不含时间、不含 UI**，所以能单测：
  `input::PadState`、legacy `GamepadMapper`、`app::routeKey`（`src/app/ime_route.cpp`）、
  `app::tabStep/tabAfterClose`（`src/app/tabs.h`）。
  时间参数（400ms 双击、150ms 闪白）只在 app 层。
- `term::TerminalParser` 是唯一终端入口（`src/modules/terminal/terminal_parser.h`），
  上层不认 libvterm；`createParser("vterm")` 找不到别的实现时回落 vterm，接口不变。
- `platform/` 只认 `disp::DisplayBackend` 接口（`src/platform/display_backend.h`），不认 UI；
  UI 只通过 `render::Renderer` 画，后端差异封在接口后面。
- 唯一的胶水是 `app.cpp`：只有它同时认识 cfg / term / input / ime / ui / platform。

**线程模型**：单线程、事件驱动，不做多线程，不 60fps 轮询。终端解析、输入、渲染串行执行。
仅有的「时间」是双击 400ms 与弹窗闪白 150ms（`steady_clock`，主循环里判）。

**Lua 边界**：只在装配期跑 Lua（`luaL_dofile` 三个文件），转成原生 `cfg::Ui/KeyMap/Engine`，
**运行期零 Lua**。C++ 不保存、不回调任何 Lua 函数——早期设计里的「回调阶段」没有实现（§10）。

## 2. 模块清单

| 路径 | 职责 | 状态 |
|---|---|---|
| `src/main.cpp` | 参数、后端选择、自检/审计入口、`--legacy` 分支 | 生效（legacy 分支待删） |
| `src/app/app.cpp` | 新主循环：装配、tab 容器、事件、路由、绘制 | **唯一运行路径** |
| `src/app/ime_route.cpp` | 按键 → 字节/动作（IM 代数），纯函数 | 生效，`--audit-ime` 覆盖 |
| `src/app/tabs.h` | tab 下标运算 | 生效，`--selftest` 断言 |
| `src/config/lua_config.*` | Lua 三类配置 → 原生结构；`cfg::Action` 枚举 | 生效 |
| `src/term/pty.*` | fork/exec、非阻塞读写、resize、析构 SIGHUP | 生效 |
| `src/term/vterm_bridge.*` | libvterm 封装（网格/光标/历史） | 生效 |
| `src/term/osc_filter.*` | 剥 OSC 1337 / 标题序列，跨 read 续接 | 生效，`--selftest` 4 条 |
| `src/modules/terminal/vterm_parser.cpp` | `TerminalParser` 的 vterm 实现 | 生效 |
| `src/ime/*` | 拼音索引、ngram、拼音缓冲/候选 | 生效 |
| `src/input/evdev_gamepad.*` | evdev → `PadState`/legacy `GamepadMapper` | 生效 |
| `src/input/uinput_kbd.*` `uinput_mouse.*` | 虚拟键盘/鼠标（`/dev/uinput`） | 生效（控制码缺口见 §7） |
| `src/platform/display_{registry,fbdev,headless,sdl}.cpp` `soft_canvas.*` | 显示后端 | fbdev/headless/SDL 生效；drm/egl/glx 零代码 |
| `src/render/*` `src/theme/palette.*` | 字体/atlas/绘制/配色 | 生效 |
| `src/ui/screen.*` `layout.*` `theme.h` `char_grid.h` | 三分区绘制、几何、颜色常量 | 生效 |
| `src/ui/panels.*` `terminal_view.*` `layout_lua.*` + 根 `layout.lua` | legacy UI | **双栈**，待删（§8） |
| `tools/fbtest.cpp` | fbdev 只读/写像素/旋转探针 | 上机工具 |
| `tools/make_pinyin_index.py` `build_ngram.py` `pc-host-switch.sh` | 数据生成、PC↔Switch 网线直连 | 工具 |

## 3. 终端层

### 3.1 Pty

`spawn(argv, cols, rows)` / 非阻塞 `readMaster` / `writeMaster` / `resize`（`ioctl TIOCSWINSZ`）/
`masterFd()`。析构里 SIGHUP 子进程——所以 `erase` 掉一个 `Term` 就等于关掉那个 shell。

### 3.2 解析接口

```cpp
class TerminalParser {          // src/modules/terminal/terminal_parser.h
    bool init(int rows, int cols); void resize(int rows, int cols); void feed(const char*, size_t);
    Cell cellAt(int row, int col) const; void cursor(int& row, int& col, bool& visible) const;
    void setHistoryLimit(int); int historySize() const; Cell historyAt(int, int) const;
    bool dirty() const; void clearDirty(); void markDirty();
    int rows() const; int cols() const; const char* name() const;
};
TerminalParser* createParser(const char* name);   // vterm（默认）/ dummy / ghostty（未接）
```

上层（`app.cpp` 的 `drawGrid`）只认这个接口。`ghostty` 只是一个回落别名，真实现是零代码。

### 3.3 OSC 过滤（tab 的触发通道）

`src/term/osc_filter.cpp` 在喂 vterm **之前**剥掉这些序列（一个字节都不能漏，否则屏上出乱码），
其余字节原样透传：

| 序列 | 动作 | 谁发 |
|---|---|---|
| `ESC ] 1337 ; newtab=<名字>` `BEL`/`ST` | 新建 tab（名字可空） | `term [名字]` shim |
| `ESC ] 1337 ; quit` `BEL`/`ST` | 关当前 tab | `quit` shim |
| `ESC ] 0 ; <文本>`（`1`/`2` 同） | 改当前 tab 标题 | 标准终端标题序列，fish 等自己会发 |
| PTY 读返回 `-1` | 关当前 tab | shell 自己 `exit` / Ctrl+D |

两条约束写死在实现里：**必须跨 read 边界**（内核把一条序列拆成两次 `read` 是常态，用 `pend` 续接）；
未闭合序列累积超过 **512 字节**就放弃识别、原样放行（`kPendCap = 512`，`pend` 不会无上限增长）。

### 3.4 多 tab（终端实例容器）

一个 tab = **一套 `TerminalParser` + 一套 `Pty`**，互不共享状态（`VTermBridge` 状态全在成员里，
`static VTermScreenCallbacks` 只存函数指针、user data 是 `this`）。

```cpp
struct Term { unique_ptr<TerminalParser> tp; unique_ptr<Pty> pty; string title; string pend; };
vector<Term> terms; int active = 0;
```

用 `unique_ptr` 不是为了抽象：`vector` 扩容时元素会搬家；`erase` 时 `Pty` 析构自动 SIGHUP 子进程。

**四条不变量（改这块之前先读）**

1. **每帧要读所有 tab 的 master fd**，只画 `active`。不读非活动 tab，对方写满 PTY 缓冲（默认 64KB）就卡死，
   `tmux`/`vim` 这类程序会直接哑掉。
2. **遍历 `terms` 期间不许改 `terms`**。读循环里只往 `closing` / `opening` 记账，循环结束后先按**倒序**关，
   再统一新建。
3. 尺寸变化（`SIGWINCH`）和热重载（`SIGUSR1`）都要**遍历所有 tab** `tp->resize` + `pty.resize`，
   漏一个就有一个 tab 的行列跟画面对不上。
4. tab 上限 **16**（`spawnTerm()` 里硬编码）。OSC 能让 PTY 输出直接生 tab，不设上限就是
   可以被 `cat` 一个恶意文件点爆的内存放大器。

**三条使用路径（都不需要组合键）**：终端里敲 `term` / `quit` / `exit`；点 tab 条切换；点末尾 `+` 新建。
tab 条画在顶栏里（起点 x=常量 180，格宽 96），绘制与命中共用 `tabRect`。

`installPathShims()` 启动时写 `/tmp/sit-bin-<pid>/{term,quit}`（0755 sh 脚本，只往 stdout 打控制序列），
塞到 `PATH` 最前面。退出不清理（代码里有 `shortcut:` 注释）。

**`--run` 的三个特例**（都是为了无头截图不炸）：`newtab` 事件被忽略（否则 `--run "term x"`
自己生自己，无限递归）；死 tab 不 erase（3 个 `bash -c` 立刻全退会让 `terms` 变空，
后面 `terms[0]` 直接崩）；全死光当作退出信号（旧语义）。`quit` 事件照常关。

**已知未做**：tab 组合键没分配（`cfg::Action` 的 `kNextTab/kPrevTab/kNewTab/kCloseTab` 留着，
`config/keymap.lua:32-35` 注释掉了）。`Ctrl+N` = `0x0e` 撞 readline 的「历史下一条」，
且 `src/app/ime_route.cpp:11` 先判 ctrl 后判 shift，`Ctrl+Shift+N` 也会被折成 `0x0e`——
真要绑组合键，先把判断顺序倒过来，或用 shell 打不出来的组合（`Ctrl+Shift+N/W/Tab`）。

**已知未验**：只用假 shell 验过 OSC 路径；真实 bash/fish 的交互（敲 `term` 真能建、`quit` 真能关）
还没在带输入的环境跑过。

## 4. IME 层

### 4.1 分层

```
魔法键（LB）在光标吸附的键上取字符 → push('n') → 拼音引擎 → 候选 → 上屏
```

键区只负责产生**字母流**，拼音/分词层（`ime::BuiltinIme`）不知道布局是 40 键还是 26 键。

### 4.2 IM 按键代数（`src/app/ime_route.cpp`，`--audit-ime` 逐条核对）

| 按键 | 行为 |
|---|---|
| 字母 | 组字；中文态大写自动转小写 |
| 空格 | 上屏首个候选 |
| **回车** | **拼音原样上屏**（`nihao` 就是打出 `nihao`，不确认候选） |
| 数字 1-7 | 选本页候选（8/9 不吞，当普通字符） |
| Tab | 翻页 |
| Backspace | 删一个字母 |
| Esc | 取消组字，Esc 本身照发 |
| 标点 | **先把候选上屏，再发标点**（否则标点跑到中文前面） |
| Ctrl+Space | 切中英；组字中切换则拼音原样落下 |

**已知缺口**：没有词级候选，`nihao` 的第 2 候选是「你号」（单字交叉组合，非词表）。

### 4.4 修饰键规则（两条互不干扰的路径）

| 路径 | 何时用 | 组合键能不能用 |
|---|---|---|
| 数字序列（网格态空格） | 打中文/英文正文 | 不能（数字没有大小写/控制码之分） |
| 明确字母（LB 弹窗） | 精确选字母、控制键 | **能**：Ctrl/Alt + 字母 → `0x03` / `ESC+字母` |

- **Ctrl/Alt + 字母只在英文态生效**；中文态不抢字母，否则拼音打不出来。
- **中文态大写自动按小写组字**：锁了 Shift 打 `Nihao` 仍然组 `nihao`。
- **组字中收到不认识的控制字节（0x01~0x1a）直接吞掉**，不污染 shell。
- **Ctrl+C 的打法**：按一下 Ctrl（sticky，用完自动释放）→ 摇杆吸到 `c` → L（魔法键）上屏。
  一个坑：桌面模式（`sink=uinput`）下 `0x01~0x1a` 没有键码映射（§7.3）。
- 十字键：三种形态共存 —— 按住（`holding`，抬起即清）/ 单击（`sticky`，下一颗真按键用过就清）/ 双击 400ms 内（`locks`）；
  实际生效的掩码 = 三者按位或（`app.cpp` 的 `modMask()`）。

### 4.5 虚拟鼠标（RB/RT）

`src/input/uinput_mouse.*`：懒创建 `/dev/uinput` 设备（`EV_KEY` + `BTN_LEFT/BTN_RIGHT`），
只发点击不发 `REL`，指针位置不动（留给真鼠标）。`--probe-mouse` 会建设备、查 `/sys/class/input`
是否注册、点一下。**右摇杆移动指针已决定不做**（动作名 `mouse_move` 绑定保留，C++ 侧故意不实现）。

### 4.6 自检

```bash
./build/switch-ime-term --audit-ime   # IM 按键代数 + 修饰键
./build/switch-ime-term --audit-input  # 手柄按键/摇杆映射 + keymap 装配
./build/switch-ime-term --probe-mouse # 虚拟鼠标设备注册 + 点击
```

## 5. UI 层

### 5.1 三分区

```text
┌──────────────────────────────────────────────────────────┐
│ 顶部状态栏（标题 / tab 条 / 时间）                        │
├──────────────┬────────────────────────┬──────────────────┤
│ 左手柄面板   │ 终端区                 │ 右手柄面板       │
│ (40 键键区)  │ (TerminalParser)       │ (光标/ABXY)      │
├──────────────┴────────────────────────┴──────────────────┤
│ 底部状态栏（Layer / Mods / IME 状态）                    │
└──────────────────────────────────────────────────────────┘
```

设计基准 1280×720，所有尺寸 = 基准 × `kScale`，**字体也按同一系数重栅格化**（放大是原生清晰，不是位图拉伸）。

| 量 | 默认（`config/ui.lua`） |
|---|---|
| 面板宽 / 顶栏 / 底栏 | 176 / 32 / 26 |
| 旧九宫格外框（v2 不用） | 180 |
| 十字键格 / ABXY 格 / 肩键条 | 34 / 30 / 20 |
| 终端内边距（左右/上/下） | 14 / 10 / 8 |
| 终端字符格 | 8 × 21 |
| 底部 IME 行 | 2 |
| 键帽 / 键距 / 一屏几列几行 | 48 / 58×62 / 3×4 |
| 派生：终端列×行 | **112 × 28**（(1280−2×176−2×14)/8 = 112；(720−32−26−10−8)/21 = 30，再扣 ime 2 行） |

### 5.4 指针模型

- **摇杆**：每手一个指针，在各自 5×4 的键场里按引力场运动（`app::physics`，见 `PHYSICS.md`），
  吸附到键就整键反色上屏——这是唯一已接线的指针源。
- **鼠标**：真实鼠标只用于 tab 条（`bd->mouseX/mouseY` → `ui::tabAt`），键区不接鼠标。
- **触摸**：SDL `SDL_FINGER*` / fbdev `ABS_MT_POSITION_X/Y`，接进来等同于鼠标左键。

### 5.5 v2 分体键盘（唯一路径）

- 数据在 `config/ui.lua`：`ui.keys`（40 个坐标）+ `ui.layers.alpha/num`（每层 40 字符，上档在 `ui.shifts`），
  权威规格是 `LAYOUT-v2.md`。
- 绘制/交互对象是 `ui::drawKeyHand` / `ui::drawKeyPanels`（`src/ui/screen.cpp`）：主字符 = 当前层
  （大/亮），副字符 = 该键上档（小/淡），吸附键整键反色，周围键按引力强度点亮。
- 主循环（`app.cpp`）只有这一条路径；`--preview-keys [--preview-layer 0|1]` 出静态图，
  配 `--backend fbdev` 可直接画到 Switch 屏上。

### 5.6 视觉 token

- 字体：`Maple Mono NF CN`（ASCII 等宽、汉字正好 2 格，1:2 网格合格），缺字回落 `Noto Sans CJK SC`；
  `⇄`/`＋` 靠回退。本机 fc-list 0 命中主字体（`DEVICE.md` §3.5）。
- 颜色（`src/theme/palette.cpp`，3 套预设 `switch`/`light`/`gruvbox`，`--theme`、`--list-themes`）：
  底 `#05070c`、面板 `#151a24`、主文本 `#ccd2dc`、左手青 `#46c5d3`、右手红 `#e87e66`、
  键面 `#1f2531`、选中蓝 `#5f9fd0`。
- UI 与终端 ANSI 0-15 都从 palette 取色，改一处全变；16-255 色仍走 vterm 自带 256 色表。
- **无边框原则**：按键/网格/面板只靠颜色、反色、留白区分，不画实线或圆角。

### 5.7 缩放与热重载

- `kScale` = 窗口输出像素 / 设计尺寸，尺寸与字体同系数重栅格化。
- 窗口尺寸变化 → `scaleHint()` → `applyScale()`：重排布局、重建字体、换画布、`vt.resize` + `pty.resize`。
- 配置重载是**手动**的：`kill -USR1 $(pgrep -x switch-ime-term)`（`-HUP` 也行）或窗口内 `Ctrl+Shift+R`。
- 取整副作用：不同缩放下终端行列差 1~2（1x = 112×28），像素取整不可避免。

### 5.8 渲染 / 交互硬约束（踩过的坑）

- **宽字符必须先铺完整行底色、再逐格画字**。以前边铺边画，宽字符跨两格时后一格的底色会盖掉前一字的
  右半边；fzf 选中行、反显（`\033[7m`）必现。
- **`SDL_TEXTINPUT` 不可用**：`src/platform/display_sdl.cpp:99` 无条件 `SDL_StopTextInput()` →
  X11 下不挂 XIM → fcitx5 永远进不来（细节见 `DEVICE.md` §9.2）。可打印字符只能由 `keySequence()`
  从 keysym 合成；Shift 判定必须用自己按事件顺序跟踪的状态（`shiftDown_`），用 `keysym.mod` 会滞后一个字，
  打出 `_`→`-`、大小写翻转。
- **小字号（<16px）** 要用 `FT_LOAD_TARGET_LIGHT`，且字号取整数：12.5px 的 ppem 会被取整到 13
  但缩放仍按 12.5 算，笔画粗细不均、横画断裂。
- **fish 启动会 `tcsetattr(TCSAFLUSH)`** 丢掉已写入的输入，所以 `--run` 要等它先吐字节；
  还要应答它的设备属性查询（`answerDA`），否则启动卡住等超时。
- `data/` 是相对路径，换目录启动会警告（不崩）。
- SDL 窗口被合成器缩放时（1280×720 → 1024×576），点击坐标按比例换算。

## 6. 配置规范

### 6.1 原则

- **配置就是 Lua**：C++ 用 `luaL_dofile` 读一遍，读成原生 `cfg::Ui / cfg::KeyMap / cfg::Engine`，
  运行期主循环零 Lua。
- **一个文件分两半**：上半段纯数据赋值（`ui.panel_w = 176`、`keymap.bind("A","enter")`），
  C++ 只认这些固定键名，缺键用 `src/config/lua_config.h` 的默认值；下半段是函数/循环/表构造，
  用来生成或校验配置。`cfg::loadProfile` 跑整个 chunk，chunk 结束就 `lua_close`，
  副作用只发生在装配期。
- **没有事件钩子**：C++ 不保存也不回调任何 Lua 函数，`function ui.on_xxx()` 写了不会被调用。
- 三类文件：

| 类别 | 文件 | 管什么 |
|---|---|---|
| 看得见 | `config/ui.lua` | 尺寸、键位与层、面板标签、排版参数 |
| 看得见 | `config/keymap.lua` | 物理按键 → 动作名 |
| 看不见 | `config/engine.lua` | 终端（滚动历史）、IME、输入（死区/滞回/双击）、日志、屏幕方向、输出口 |

入口：`cfg::loadProfile("config", prof)` 一次拿到 `prof.ui / prof.keymap / prof.engine`；
三个文件都读不到才返回 `false`（此时用头文件默认值）。`app.cpp` 还会退回相对路径 `config`。

### 6.2 `config/ui.lua`

尺寸单位 = 设计像素（1280×720 基准），实际渲染乘缩放。

| 键 | 默认 | 控制什么 |
|---|---|---|
| `ui.title` | `"sit/0.1"` | 顶部状态栏左侧标题 |
| `ui.panel_w` | 176 | 左/右面板宽度 |
| `ui.top_bar_h` / `bot_bar_h` | 32 / 26 | 顶部、底部状态栏高度 |
| `ui.stick_box` | 180 | 旧九宫格外框边长（v2 不用） |
| `ui.dpad_cell` / `abxy_cell` / `shoulder_h` | 34 / 30 / 20 | 十字键格、ABXY 格、肩键条高度 |
| `ui.term_pad_x` / `term_pad_top` / `term_pad_bot` | 14 / 10 / 8 | 终端网格区留白 |
| `ui.cell_w` / `cell_h` | 8 / 21 | 终端字符格（决定列数/行数） |
| `ui.key_size` / `ui.key_window` / `ui.key_area_dy` | 48 / `{cols=3, rows=4}` / −100 | 键帽边长、一屏几列几行、键区整体上移量 |
| `ui.ime_rows` | 2 | 底部固定留给组字串/候选的行数 |
| `ui.keys` / `ui.layers` / `ui.shifts` | 40 坐标 + 2×40 字符 + 2×40 上档 | v2 分体键盘（`LAYOUT-v2.md`） |
| `ui.labels.*` | — | `dpad_title` / `abxy_title`（配置里还在，渲染已不画） |

### 6.3 `config/keymap.lua`

键名：`LB LT RB RT A B X Y DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT L3 START`；
动作名见 `src/config/lua_config.cpp` 的 `kActions` / `actionName()`（`magic/space/tab/enter/backspace/delete/escape/
mod_*/lock_*/cursor_left/cursor_right/layer_next/onboard/mouse_left/mouse_right/next_tab/prev_tab/new_tab/close_tab`）。

```lua
keymap.bind("LB", "magic")        -- 上屏活跃侧吸附住的字符（Shift 取上档）
keymap.bind("LT", "space")        -- 组字中 = 上屏选中候选，否则 = 空格
keymap.bind("DPAD_UP", "tab")
keymap.bind("DPAD_LEFT", "mod_shift")
keymap.bind("DPAD_RIGHT", "mod_alt")
keymap.bind("DPAD_DOWN", "mod_ctrl")
keymap.bind_double("DPAD_LEFT", "lock_shift")   -- 双击锁定修饰（不是切层）
keymap.bind("A", "enter") ; keymap.bind("B", "backspace")
keymap.bind("X", "delete") ; keymap.bind("Y", "escape")
keymap.bind("RB", "cursor_left") ; keymap.bind("RT", "cursor_right")
keymap.bind_axis("STICK_R", "mouse_move")        -- 已注册，C++ 侧故意不实现
```

动作枚举收敛到 `cfg::Action`：`ui::ActionId` 与 `input::kGpAct*` 都只是它的常量别名，不再各写一套编号。

### 6.4 `config/engine.lua`

| 键 | 默认 | 控制什么 |
|---|---|---|
| `engine.terminal.scrollback` | 2000 | 终端向上保留多少行历史（`term::VTermBridge`） |
| `engine.ime.cand_per_page` | 7 | 底部候选条一页几个（IME 内部翻页跨度跟着变） |
| `engine.ime.ngram` | true | 候选是否按 bigram 上下文重排（**只装配，未接线**） |
| `engine.ime.enabled` | true | 内置输入法是否介入；`false` = 字符原样透传（桌面模式） |
| `engine.input.deadzone` | 0.15 | 摇杆死区（归一化后） |
| `engine.input.hysteresis_deg` | 8 | 扇区切换滞回角，防边界抖动 |
| `engine.input.double_tap_ms` | 400 | 同键二按 = 双击的判定窗口 |
| `engine.log.level` | `"warn"` | `debug/info/warn/error`（**只打印，未接线**） |
| `engine.log.path` | `""` | 日志文件，空 = 只写 stderr（**只打印，未接线**） |
| `engine.display.rotate` | 270（`lua_config.h` 默认 90） | 屏幕软件旋转 0/90/180/270，只对 fbdev 生效 |
| `engine.sink` | `"pty"` | 输出口：`pty` 写自带伪终端 / `uinput` 当虚拟键盘（需 `/dev/uinput`，打不开自动回落 `pty`） |

### 6.5 桌面模式（与 fcitx5 共存）

| `engine.sink` | `engine.ime.enabled` | 用途 | 行为 |
|---|---|---|---|
| `pty` | `true` | **默认**：Switch / 裸 TTY | 内置拼音写 PTY |
| `pty` | `false` | 桌面窗口里当终端，输入法交给 fcitx5 | 字符原样写 PTY（只做 Ctrl/Alt/Shift 转换），不组字、不切语言 |
| `uinput` | `false` | **只当虚拟键盘**：手柄按键 = 系统输入事件 | 按键经 `/dev/uinput` 打给焦点程序；PTY 仍 spawn 并显示输出（当监视器） |
| `uinput` | `true` | 罕见：内置拼音上屏后交给系统 | 组字/选词在本程序内，上屏结果经 uinput 打给焦点程序（中文会被丢弃） |

防冲突三条原则：

1. **不注册任何全局热键**，不抓 X/Wayland 键盘，不抢 fcitx5 的组合键。
2. `ime.enabled = false` 时**不注入任何修饰组合**（尤其不替用户按 `Ctrl+Space`）：`Ctrl+Space`（字节 `0x00`）
   在直通路径上返回空且不消耗，切换键完整留给 fcitx5。
3. `uinput` **打不开就自动回落 `pty`**：打一行警告（需要 root 或 input 组 / udev 规则），继续跑。

**硬限制**：`uinput` 只能发 `EV_KEY` 1..127（US 布局键码），打不出 Unicode。非 ASCII 的 UTF-8 直接跳过并计数，
退出时打印一次 `uinput: 丢弃 N 个非 ASCII 字符（中文需要 fcitx5/wtype，见 DESIGN §4.4）`
（程序里的字符串仍引用旧文档名，属已知遗留）。中文出路：fcitx5 走 D-Bus（零代码），或 Wayland 下用 `wtype`。

### 6.6 热切换语义

```fish
kill -USR1 (pgrep -x switch-ime-term)     # SIGHUP 等价；窗口内 Ctrl+Shift+R 同入口
```

一次重载 = **全量重读 + 全量重建**（不做增量）：

1. `cfg::loadProfile("config", …)` 重读三个文件；三个都读不到就打印警告、**保持旧配置继续跑**。
   只改坏其中一个时，那个文件对应部分回落默认值（缺键 = 默认值，不保留旧值）。
2. `Geometry` 重算（尺寸一变，终端列数/行数跟着变）。
3. 字体重建（缩放没变也重建：`FontBook::init`）。
5. `TerminalParser::resize` + `PTY resize`（SIGWINCH 送给 shell）。
6. 滚动历史上限按新的 `engine.terminal.scrollback` 重设。
7. `markDirty()`，下一帧整屏重绘。

**重载后复位（预期）**：组字串/候选/页码（拼音缓冲本身不清，
只清 UI 快照）；滚动位置与选区；双击判定（上一颗按下的键置空）。
**保留**：Ctrl/Shift/Alt 的锁定态（挂在后端 `bd->locks`，不属于 UI 瞬态）。输出口开机时定下，热重载不换设备。

### 6.7 死配置清单（解析了但没接线）

| 键 / 绑定 | 现状 |
|---|---|
| `engine.ime.ngram` | 只解析，候选重排实际一直走 ngram（开关不生效） |
| `engine.log.level` / `engine.log.path` | 只打印到 stderr，不落文件、不过滤级别 |
| `keymap.bind_axis("STICK_R","mouse_move")` | `lua_config.cpp:86` 只设一个 bool，**无消费点** |
| `cfg::kNextTab/kPrevTab/kNewTab/kCloseTab` | 动作存在，`keymap.lua:32-35` 注释掉了 |

## 7. 已知泄漏点（审计发现的待修项，带 file:line）

按严重度排；修法都在对应小节里。**这些是「现实与设计不一致」的地方，不是设计定稿。**

1. **剪贴板新路径是空实现**：`src/app/app.cpp:423-424` 的 `case cfg::kCopy/kPaste` 直接 break；
   `src/platform/display_backend.h:103` 的 `copyReq/pasteReq` 只有 legacy 循环消费，`app::run` 从不读；
   唯一实现是 legacy 的 `src/main.cpp`（`popen("wl-paste"/"wl-copy")`），而本机工具全缺 → X11 下
   `Ctrl+Shift+C/V` 完全静默。
2. **fcitx5 进不来**：`src/platform/display_sdl.cpp:99` 无条件 `SDL_StopTextInput()`，
   `engine.ime.enabled=false` 只做到「程序内不抢」。要共存得按 `ime.enabled` 决定 Start/Stop。
3. **uinput 静默丢控制码**：`src/input/uinput_kbd.cpp:23-66` 的 `asciiToKey()` 不认 `0x01~0x1a`
   → `default: return {}` → `key=0` → `tap()` 首行 `if (fd_ < 0 || !keycode) return`（`:120`）直接返回。
   后果：**桌面模式（sink=uinput）下 Ctrl+A/C/V/D 一个键都不发**。PTY 模式本来就对。
   修法：`KeyStroke` 加 `bool ctrl`；`ch 1..26 → {KEY_A + ch - 1, false, true}`；`tap()` 像 shift 一样
   夹 `KEY_LEFTCTRL`，约 15 行。
4. ~~**sticky 从不清零**~~ —— **已修**：`onPadButton` 在本颗键的 `doAction` 之后调 `clearSticky()`
   （修饰键本身与 `kNone` 除外），键盘路径每键清；抬起时若按住期间被用过也不再留一次性修饰（`bd->heldUsed`）。
5. **中文态 Ctrl 被吞**：`src/app/ime_route.cpp:11` 的 `if (!ime.chinese && …)` 让中文态下 Ctrl+c
   走到「字母起组字」（`:109`），Ctrl 静默失效、底栏却还亮着 CTRL。
   桌面模式不受影响（`ime.enabled=false` 走直通，不看中文态）。
6. **SDL 渲染器没有软件回落**：`src/platform/display_sdl.cpp:87` 只请求 `ACCELERATED | PRESENTVSYNC`，
   失败即退出（`DEVICE.md` §3.4）。
7. **fbdev 每帧整屏 clear**：`src/platform/display_fbdev.cpp:95` 的 `beginFrame()` 是 `canvas_.clear()`，
   脏矩形化之前必须先拆掉它（`DEVICE.md` §6.9）。
8. **右摇杆 `mouse_move` 无消费点**：`src/config/lua_config.cpp:86`（已决定不做，属故意留的泄漏）。
10. **legacy 双栈**：`--legacy` 走 `ui::panels` / `terminal_view` / `layout_lua`，与 `app.cpp` 并行（§8）。
11. **`engine.sink` 权限**：`/dev/uinput` 是 `0600 root:root`，打不开静默回落 `pty`
    （`src/app/app.cpp:176-180`，只有一行警告，容易以为在打 uinput）。
12. **`uinput` 只发按键不发 Unicode**：非 ASCII 丢弃并计数（`app.cpp` 退出时打印），
    所以 `uinput + ime.enabled=true` 那条路中文必丢（§6.5）。

## 8. legacy 双栈

`--legacy` 是旧 UI：`src/ui/panels.*`（左右面板 + HitTest）、`src/ui/terminal_view.*`（光标/选区/回看）、
`src/ui/layout_lua.*` + 根 `layout.lua`（旧版式）、`main.cpp` 里的旧事件循环。

**删除前必须先把 legacy 独有能力搬进新路径**：

- 点击 HitTest（面板格子能点）
- 选区（`kActSelLeft/kActSelRight`，Shift+←/→ 扩选，`selStep/selClear/selectedText` 在 `ui/terminal_view`）
- 剪贴板（`copyReq/pasteReq` 的消费端）
- 滚轮回看（滚轮 / Shift+PgUp·PgDn，历史 2000 行，底部提示 `SCROLL n/N`；`sb_popline` 返回 0，
  往下不还原被吞的行）
- SDL 写 `sticky`

新路径现状：终端滚动/选区/剪贴板都还没有（§7.1），所以 legacy 暂时不能删。

## 9. 已定稿 vs 待修

**已定稿（代码按这个写，不用再讨论）**

| 决定 | 依据 |
|---|---|
| 配置是 Lua 三类文件，运行期零 Lua、无事件钩子 | §6.1 |
| 动作枚举收敛到 `cfg::Action`，别名只在 `ui::ActionId`/`input::kGpAct*` | §6.3 |
| 终端解析走 `TerminalParser`，vterm 是默认实现 | §3.2 |
| tab 不靠按键，靠 `term`/`quit`/`exit` + OSC 1337，上限 16 | §3.4 |
| 输出口 `pty`/`uinput` 由 `engine.sink` 定，开机定死，热重载不换 | §6.5 |
| 右摇杆不模拟鼠标（RB/RT 只点不动） | §4.5 |
| v2 分体键盘是唯一布局（旧九键已删除） | `LAYOUT-v2.md` §7 |
| 屏幕旋转由我们自己做（`engine.display.rotate`），不指望驱动 | `DEVICE.md` §4.4 |

**待修（审计发现，按 §7 顺序）**：剪贴板、StopTextInput、uinput 控制码、sticky 清零、中文态 Ctrl、
SDL 软件回落、fbdev 脏矩形、legacy 搬迁、v2 接线。

## 10. 作废设计存档（只有数字和结论还有参考价值，勿实现）

### 10.1 早期架构（epoll / D-Bus / 四类可插拔后端）

```text
main / EventLoop (epoll): PTY fd | gamepad fd | keyboard fd | pointer fd | D-Bus | timer
  InputManager（三源：EvdevGamepad/Keyboard/Pointer）→ GamepadNormalize → ActionMap(keymap.toml)
    → HitTest → ModeFSM → ImeBackend → OutputSink(PTY master / uinput)
  DisplayBackend（fbdev/drm/egl_gles/glx）+ OverlayStack（registry 空）
```

作废理由：实际是**单线程阻塞主循环 + SDL/evdev 事件轮询**，没有 epoll 事件循环、没有 D-Bus、
没有 InputManager/ModeFSM/OverlayStack 类。四类后端里只有 `headless`/`fbdev`/`sdl` 落地；
`drm`/`egl_gles`/`glx` 零代码（本机无 `/dev/dri`，见 `DEVICE.md` §3）。`fcitx5`/`ibus` 后端零代码。

### 10.2 TOML 配置（被 Lua 取代）

早期规范：`[display] backend/width/height/font/font_size`、`[input] deadzone=0.35 sectors=8 profiles`、
`[input.gamepad] device/virtual`、`[input.keyboard] route=pty|ime|action layout=us`、
`[input.pointer] click_hit_test/selection/scroll`、`[terminal] shell/cols/rows/scrollback=1000`、
`[ime] backend=builtin page_size=7 ui=statusbar`、`[ime.builtin] pinyin_index/ngram/context_window=1000`、
`[ui] annotation_max_chars=12 annotation_style=inline`、`[mode] default=terminal|typing`、
`[typing] inject=uinput_key`。配套 `data/keymap.toml`（`[[map]] when={mode,layer,action} do={type,…}`）
与 `data/gamepad_profiles.toml`（按 `name_regex`/`vendor`/`product` 匹配 Switch Pro/Xbox/PS4/PS5）
——**全部作废**，一个都没实现；现行配置见 §6。

被一并作废的还有：`ScreenBuffer`（`Cell{codepoint,fg,bg,attrs}` + `wcwidth`，已被 `TerminalParser` 取代）、
候选词 `annotation`/`annotation_style`（未实现）、IME 状态栏固定 1 行（现在是 `ui.ime_rows = 2`）、
`ImeBackend` 抽象接口与 `fcitx5` D-Bus 协议流程（`org.fcitx.Fcitx5` / `InputContext1` /
`CreateInputContext` / `UpdateLookupTable` / `CandidateWord.comment`）、打字模式（`mode.default = "typing"`）。

### 10.3 旧像素推导链（图形版 UI 时代，216px 面板）

已作废：现行是纯字符网格，面板 176、无圆角/描边/辉光。历史数值：

```text
s        = min(W/1280, H/720)
panel_w  = round(216 * s)        tbar_h = titlebar ? round(38 * s) : 0
pad_x    = round(20 * s)         pad_top = round(12 * s)    pad_bot = round(14 * s)
cell_w   = floor(ascii_advance_px)          cell_h = round(line_spacing * font_size)   // 默认 1.5
cols     = (term_w - 2*pad_x) // cell_w     rows   = (term_h - pad_top - pad_bot) // cell_h - 1
```

字体度量必须运行时测（不能写死 `0.6em`）：ASCII `U+0020`–`U+007E` advance 全相等才可用；
`cell_w = ASCII advance`，校验 `CJK advance == 2 * cell_w`（容差 ±1px），不满足取
`cell_w = max(cell_w, CJK_advance/2)`；`cell_h` 用 `round(line_spacing * font_size)`，不用 FreeType 的
`metrics.height`（CJK 字体偏紧，行贴行）。

**本机实测（CachyOS，14px）**：`Noto Sans CJK`（TTC 内 Sans 索引 0–4）ASCII 不等宽
（`M`=11.375px / `a`=7.875px），**不能用于终端**；`Noto Sans Mono CJK`（索引 5–9）ASCII 恒 7.0px、
CJK 14.0px，比例正好 1:2，可用。两个坑：`fc-match "monospace:lang=zh-cn"` 会解析到
`Noto Sans Mono CJK KR`（韩式汉字字形），配置要显式写 `... SC` 或匹配后断言 `FAMILY` 含 `SC`；
同一个 `.ttc` 里 Sans 与 Mono 是不同 face index，FreeType 必须按 face index 加载。

`cols/rows` 与 `font_size` 的过约束（规则仍适用）：`sizing = "fill"`（默认，按字号算格数，多余空间吃掉）
或 `"fixed"`（从 `font_size` 往下试到 8，取第一个装得下的；找不到用 8 并告警）。

旧算例（font_size=14，line_spacing=1.5，216px 面板）：

| 分辨率 | s | 面板宽 | cell | cols×rows（含状态栏） | 终端占比 |
|---|---|---|---|---|---|
| 1280×720（底座/掌机） | 1.00 | 216 | 7×21 | 115×31 → PTY 115×30 | 66% |
| 1280×720（无标题栏 fbdev） | 1.00 | 216 | 7×21 | 115×33 → PTY 115×32 | 66% |
| 1920×1080 | 1.50 | 324 | 7×21 | 173×46 → PTY 173×45 | 66% |
| 1024×600（树莓派屏） | 0.80 | 173 | 7×21 | 92×26 → PTY 92×25 | 66% |
| 854×480 | 0.67 | 144 | 7×21 | 77×22 → PTY 77×21 | 66% |

854×480 上 77 列 < 80 列，这正是需要 auto-shrink 的理由。反推 80×24：1280×720 可用 font 17
（89 列 × 24 行），1024×600 可用 font 14。

旧侧面板内部尺寸（基准坐标 × s）：肩键 L/R 宽 92%、高 16；ZL/ZR 宽 62%、高 14；
摇杆盘 190×190 圆、内格 3×3 每格 42 gap 3、外圈键 26×26；十字键 3×3 每格 26 gap 3；
ABXY 3×3 每格 30 gap 3；面板内边距 9 上下 / 12 左右；`s < 0.72` 时外圈键改半透明叠加、hit 优先外圈。
旧候选弹窗几何：锚在 PTY 光标格下一行，高度 = 2 行 + 2×padding(4*s)，每页容量按宽度算（不是固定
`page_size`），光标在最后一行时翻到上方。

### 10.4 旧双摇杆 26 字母表（v2 之前，已作废）

左摇杆 13（元音全在左）：内上 `i`、右上 `h`、右 `n`、右下 `z`、下 `g`、左下 `u`、左 `a`、左上 `e`、
中心（r < deadzone + 按下）`o`、外上 `m`、外右 `t`、外下 `l`、外左 `r`。
右摇杆 13（辅音）：内上 `s`、右上 `x`、右 `y`、右下 `v`、下 `j`、左下 `c`、左 `d`、左上 `q`、
中心 `b`、外上 `f`、外右 `w`、外下 `p`、外左 `k`。并集 = 26 字母无重复；`ü` 用 `v` 代替（`lv` → 绿/律/旅）。
配套规则：回中式提交（推过死区高亮、**回到死区才提交**，不连发）；`deadzone = 0.35`、`sectors = 8`、
`hysteresis_deg = 8`；外圈 4 键要 Action 带半径后缀（`LStickInnerUp`/`LStickOuterUp`/`LStickPress`）。
符号层（L 键）13 格：`；，？。、！：“”（ ）— …`；数字层 13 格：`7 8 9 / 4 5(按住 0) 6 / 1 2 3` + 外圈 `+ − × ÷`。

### 10.5 旧待决项 D1–D11（多数已被 v2 解决）

| # | 问题 | 后来的结论 |
|---|---|---|
| D1 | 外圈 4 字母怎么到达（半径双环 / L3 按住 / 12 扇区） | 随双摇杆方案一起作废 |
| D2 | 中心格由谁触发（RB 居中 vs 摇杆按下） | 摇杆回零位自动吸附 home 键 |
| D3 | 数字层 5 与 0 怎么区分 | 作废，数字进 NUM 层直接选键 |
| D4 | 候选第 2–7 个怎么选 | 魔法键在数字层按候选编号 |
| D5 | ZL/ZR「光标移动」指 compose 光标还是终端光标 | 未落地；`kCursorLeft/Right` 只有 legacy 用 |
| D6 | compose 串画在哪（浮层 / 状态栏） | 底部 `ime_rows = 2` 行 |
| D7 | 状态栏位置（底部 1 行 vs 右上 pill） | 底部状态栏 |
| D8 | `cols/rows` 与 `font_size` 过约束 | 规则保留（§10.3），`sizing` 键未实现 |
| D9 | fbdev 上要不要标题栏 | 无标题栏，顶栏 32px 画 tab |
| D10 | 侧面板吃 34% 屏宽 | v2 面板更窄，终端 320..960 = 80 列 |
| D11 | 非 16:9 屏幕（letterbox 在哪层做） | 未做；缩放统一在 `ui::layout::kScale` |
