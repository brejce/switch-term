# switch-ime-term · 实现规范

> **注意：本文档是早期设计稿，部分内容（尤其 TOML 配置、epoll、fcitx5、DRM/EGL）已被现实取代。**
> 当前有效的配置规范见 `docs/DESIGN.md` §4（配置文件是 Lua，分 `ui.lua` / `keymap.lua` /
> `engine.lua` 三类，热切换走 `kill -USR1`）；已实现的能力清单见 `STATUS.md`。

> 配套：`PLAN.md`（计划书）、`ui-mockup.html`（视觉基准）
> 本文档定义接口签名、模块边界、数据流。写代码时对照本文档。

---

## 1. 总体架构

```text
┌──────────────────────────────────────────────────────────────┐
│                      main / EventLoop (epoll)                 │
│  PTY fd | gamepad fd | keyboard fd | pointer fd | D-Bus | timer│
└──────────────┬──────────────────────────────┬────────────────┘
               │                              │
┌──────────────▼──────────────┐  ┌────────────▼────────────────┐
│       输入子系统              │  │         终端子系统           │
│  InputManager (三源并行)      │  │  Pty (openpty/fork)          │
│    ├─ EvdevGamepadSource     │  │  VTermBridge (libvterm)      │
│    ├─ EvdevKeyboardSource    │  │  ScreenBuffer (cell 网格)    │
│    └─ EvdevPointerSource     │  │    ↓                        │
│    ↓ 统一 Event               │  │  Renderer (fontconfig+FT)    │
│  GamepadNormalize → Action   │  │    ↓                        │
│  ActionMap (keymap.toml)     │  │  DisplayBackend (可插拔)     │
│    ↓                         │  │    fbdev/drm/egl_gles/...    │
│  HitTest (指针)              │  │                             │
│    ↓                         │  │  OverlayStack (registry 空)  │
│  ModeFSM ──► ImeBackend      │  │                             │
│    ↓ UTF-8                   │  │                             │
│  OutputSink                  │  │                             │
│    PTY master / uinput       │  │                             │
└──────────────────────────────┘  └─────────────────────────────┘
```

**线程模型**：单线程事件循环，不做多线程。终端解析、输入处理、渲染串行执行。渲染事件驱动，不 60fps 轮询。

---

## 2. 四类可插拔后端

| 类别 | 接口 | 内置实现 | 注册方式 |
|---|---|---|---|
| 显示 | `disp::DisplayBackend` | `headless`、`fbdev`、`drm`、`egl_gles`、`glx` | 静态自注册 |
| 输入 | `input::InputSource` | `EvdevGamepadSource`、`EvdevKeyboardSource`、`EvdevPointerSource` | `InputManager` 枚举 |
| 输出 | `output::OutputSink` | `pty`、`uinput` | 配置选择 |
| IME | `ime::ImeBackend` | `builtin`（默认）、`fcitx5`、`ibus` | 静态自注册 |
| 弹窗 | `ui::Overlay` | 无（registry 空，接口预留） | 静态自注册 |

---

## 3. 显示后端

### 3.1 接口

```cpp
namespace disp {
enum class PixelFormat { RGBA8888, BGRA8888, A8 };
struct Rect { int x=0,y=0,w=0,h=0; };
struct Color { uint8_t r=0,g=0,b=0,a=255; };
struct TextureDesc { int width=0,height=0; PixelFormat format=PixelFormat::RGBA8888; bool dynamic=false; };
using TextureId = uint32_t;
struct Capabilities { bool alpha_blend=false; bool hw_scale=false; int max_texture_size=0; };

class DisplayBackend {
public:
  virtual ~DisplayBackend() = default;
  virtual bool init(int width, int height) = 0;
  virtual void shutdown() = 0;
  virtual Capabilities caps() const = 0;
  virtual TextureId createTexture(const TextureDesc&) = 0;
  virtual void destroyTexture(TextureId) = 0;
  virtual void updateTexture(TextureId, const uint8_t* pixels, int row_stride) = 0;
  virtual void beginFrame() = 0;
  virtual void fillRect(const Rect&, Color) = 0;
  virtual void drawTexture(TextureId, const Rect& src, const Rect& dst, Color tint) = 0;
  virtual void endFrame() = 0;
  virtual bool captureFrame(const std::string& path) { return false; }
  virtual const char* name() const = 0;
};
}
```

### 3.2 关键约束

- 上层只产生 `fillRect` / `drawTexture`，不碰像素。
- 字形 atlas 是 A8 纹理，图标是 RGBA 纹理。fbdev 后端用内存位图模拟，GPU 后端用真纹理。
- 静态注册表：`DisplayRegistry::registerBackend(name, factory)`，`auto` 按 `egl_gles > drm > fbdev > headless` 尝试。
- `fbdev` 后端内部维护脏矩形，`endFrame` 只 memcpy 脏区；GPU 后端忽略。
- `headless` 后端用于 CI 像素回归（写 PNG 对比基准图）。

### 3.3 后端矩阵

| 后端 | 依赖 | Switch | 桌面 | CI |
|---|---|---|---|---|
| `fbdev` | `/dev/fb0` | 首选回退 | — | — |
| `drm` | libdrm | 可选 | — | — |
| `egl_gles` | libEGL+GLESv2 | 优先 | — | — |
| `glx` | X11+libGL | — | 调试 | — |
| `headless` | 无 | — | — | 回归 |

---

## 4. 终端核心

### 4.1 Pty

```cpp
class Pty {
public:
  bool spawn(const std::vector<std::string>& argv, int cols, int rows);
  ssize_t readMaster(char* buf, size_t n);   // 非阻塞
  void writeMaster(const char* buf, size_t n);
  void resize(int cols, int rows);           // ioctl TIOCSWINSZ
  int masterFd() const;
};
```

### 4.2 ScreenBuffer

> **已过时**：现在的实现是 `src/modules/terminal/terminal_parser.h` 的 `TerminalParser` 接口（`cellAt/cursor/history/dirty`），
> 默认实现是 `VTermParser`（包 libvterm）。下面的 `ScreenBuffer` 类已不存在，看 §4.5。

```cpp
struct Cell { uint32_t codepoint; uint8_t fg,bg,attrs; };
class ScreenBuffer {
  int rows_, cols_;
  std::vector<Cell> cells_;
public:
  Cell& at(int r, int c);
  void resize(int rows, int cols);
};
```

`wcwidth()` 判定宽字符，占 2 cell 时第二 cell 标记为延续。

### 4.3 IME UI 位置

终端网格底部保留 **1 行** 输入法状态栏（`rows - 1` 给 PTY）。显示 compose 串、光标、候选词、层/语言状态。`uim-fep` LASTLINE 模式同类做法，避免和 shell 输出打架。

### 4.4 多 tab（终端实例容器）

一个 tab = **一套 `TerminalParser` + 一套 `Pty`**，互不共享状态（`VTermBridge` 的全部状态都在成员里，
`static VTermScreenCallbacks` 只存函数指针、user data 是 `this`，所以多实例安全）。
容器在 `src/app/app.cpp` 的 `run()` 里：

```cpp
struct Term {
    std::unique_ptr<term::TerminalParser> tp;
    std::unique_ptr<term::Pty> pty;
    std::string title;   // tab 条显示
    std::string pend;    // oscFilter 的跨 read 残余
};
std::vector<Term> terms;
int active = 0;
```

用 `unique_ptr` 不是为了抽象，是为了两件事：`vector` 扩容时元素会搬家；`erase` 时 `Pty` 析构
自动 SIGHUP 子进程（`pty.cpp` 的析构里）。

**四条不变量（改这块之前先读）**

1. **每帧要读所有 tab 的 master fd**，只画 `active`。不读非活动 tab 的话，对方写满 PTY 缓冲
   （默认 64KB）就卡死，`tmux`/`vim` 这类程序会直接哑掉。
2. **遍历 `terms` 期间不许改 `terms`**。读循环里只往 `closing` / `opening` 两个列表记账，
   循环结束后先按**倒序**关（erase 不影响前面的下标）、再统一新建。
3. 尺寸变化（窗口缩放 `SIGWINCH`）和热重载（`SIGUSR1`）都要**遍历所有 tab**
   `tp->resize` + `pty.resize`，漏一个就会有一个 tab 的行列跟画面对不上。
4. tab 上限 **16**（`spawnTerm()` 里硬编码）。OSC 能让 PTY 输出直接生 tab，不设上限就是个
   可以被 `cat` 一个恶意文件点爆的内存放大器。

**触发方式：OSC 1337 私有协议**

新建/关闭**不靠按键**，靠终端里的命令。`installPathShims()` 启动时在 `/tmp/sit-bin-<pid>/` 下写
两个 0755 脚本并塞到 `PATH` 最前面，脚本不和我们通信，只往 stdout 打控制序列：

| 序列 | 动作 | 谁发 |
|---|---|---|
| `ESC ] 1337 ; newtab=<名字>` `BEL`/`ST` | 新建 tab（名字可空） | `term [名字]` 脚本 |
| `ESC ] 1337 ; quit` `BEL`/`ST` | 关当前 tab | `quit` 脚本 |
| `ESC ] 0 ; <文本>`（`1`/`2` 同） | 改当前 tab 标题 | 标准终端标题序列，fish 等自己会发 |
| PTY 读返回 `-1` | 关当前 tab | shell 自己 `exit` / Ctrl+D |

`src/term/osc_filter.cpp` 在喂 vterm **之前**把这些序列剥掉（一个字节都不能漏，否则终端里显示
乱码），其余字节原样透传。两个约束写死在那里：

* **必须跨 read 边界**：一条序列被内核拆成两次 `read` 是常态，靠调用方持有的 `pend` 续接。
* 未闭合序列累积超过 512 字节就放弃识别、原样放行，`pend` 不会无上限增长。

关 tab 的语义：**只剩一个时不关，原地重启一个 shell**（像 xterm，窗口不会空掉）。

**`--run` 的三个特例**（都是为了无头截图测试不炸）：`newtab` 事件被忽略（否则
`--run "term x"` 自己生自己，无限递归）；死 tab 不 erase（3 个 `bash -c` 立刻全退会让 `terms`
变空，后面 `terms[0]` 直接崩）；全死光当作退出信号（旧语义）。`quit` 事件不在此列，照常关。

**已知未做**：tab 切换的组合键还没分配（`cfg::Action` 里 `kNextTab/kPrevTab/kNewTab/kCloseTab`
留着，`config/keymap.lua` 里注释掉了）；`Ctrl+N` = `0x0e` 和 readline 的「历史下一条」冲突，
且 `ime_route.cpp` 先判 ctrl 后判 shift，`Ctrl+Shift+N` 目前也会被折成 `0x0e`。
`/tmp/sit-bin-<pid>/` 退出不清理。

### 4.5 终端解析接口现状

`createParser(name)` 是唯一入口，返回 `TerminalParser*`（`feed` / `cellAt` / `cursor` / `history` /
`dirty` / `resize`）。当前只有 `VTermParser`，ghostty 未接时回落 vterm，接口不变。
上层（`app.cpp` 的 `drawGrid`）只认这个接口，不认 libvterm。

---

## 5. 渲染层

- **FontManager**：fontconfig 查 `monospace:lang=zh-cn`，FreeType 加载，14px 默认。
- **GlyphAtlas**：1024×1024 A8 纹理，LRU 缓存；ASCII 常驻，CJK 按需动态加载（不能全量预栅格化）。
- **Renderer**：只持有 `DisplayBackend*` 和 `TextureId`，遍历 `ScreenBuffer` 查 atlas → `drawTexture`，**无后端分支**。
- 渲染顺序：背景 → 左面板 → 终端 → 输入法状态栏 → 右面板 → OverlayStack（最后）。

### 5.1 候选弹窗布局

每个候选支持可选的 `annotation` 小字（横向 inline）：

```text
[1 你好 hello] [2 你 you] [3 尼 ni] ...
```

标注超过 `ui.annotation_max_chars` 截断加 `…`。`annotation_style = "inline" | "below"`。默认 `inline`。

---

## 6. 输入子系统

### 6.1 三源并行

```cpp
namespace input {
enum class SourceKind { Gamepad, Keyboard, Pointer };
struct Event {
  SourceKind source;
  enum class Type { GamepadButton, GamepadAxis, KeyPress, KeyRelease,
                    PointerDown, PointerUp, PointerMove } type;
  int evdev_code; int value;
  uint32_t keysym; uint32_t modifiers;
  int x, y; int pointer_id;
};
class InputSource {
public:
  virtual SourceKind kind() const = 0;
  virtual int fd() const = 0;
  virtual bool poll(Event& out) = 0;
  virtual ~InputSource() = default;
};
}
```

| 实现 | 设备 | 说明 |
|---|---|---|
| `EvdevGamepadSource` | `ABS_X`+`BTN_SOUTH` | 归一化为 Action |
| `EvdevKeyboardSource` | `KEY_A`…`KEY_Z` | xkbcommon 翻译 keysym，默认直写 PTY |
| `EvdevPointerSource` | 鼠标 + 触屏 | 统一 PointerEvent |

热插拔：`InputManager` 用 inotify 监听 `/dev/input/`，插拔时重新枚举。

### 6.2 键盘路由

```toml
[input.keyboard]
route = "pty"     # pty | ime | action
```

- **`pty`（默认）**：物理键盘直写 PTY，不经过手柄映射和内置 IME。
- `ime`：调试内置 IME 时用。
- `action`：纯手柄演示时用。

### 6.3 并行输入与焦点模型

```text
所有输入源 → InputManager::poll() → 统一 Event 队列
  ├─ Gamepad  → Normalize → Action → ActionMap → 分发
  ├─ Keyboard → 按 route 路由
  └─ Pointer  → HitTest → 命中 UI → 等价 Action；未命中 → 选区/滚动
```

**不抢夺**：键盘按 `a` 写 PTY，不触发手柄字母；手柄推摇杆触发 Action，不影响键盘；三者可同帧产生事件。

**Overlay 打开时**：`modal()` 为 true 时，手柄和指针的 action 优先给 Overlay；物理键盘仍按 `route` 走。

### 6.4 HitTest

```cpp
struct HitRegion { disp::Rect rect; input::Action action; int z_order; bool enabled; };
class HitTest {
  void clear();
  void add(const HitRegion&);
  std::optional<input::Action> hit(int x, int y);
};
```

`Renderer` 画每个摇杆格子、外围键、肩键、ABXY 时同步 `hitTest.add()`。鼠标/触摸点击走和手柄**完全相同的 Action 链路**。终端区域不注册 hit region，拖拽直接走选区。

### 6.5 虚拟手柄 UI

```toml
[input.gamepad]
virtual = "auto"      # auto | always | never
```

- `auto`：探测到物理手柄 → 实时状态；探测不到 → 静态映射图，可点击。
- `always`：永远显示，物理手柄事件也反映到 UI。
- `never`：不显示，终端占满全屏。

**虚拟不是伪造输入**：只是显示层始终渲染左右面板；鼠标点击产生标准 Action，不是伪造摇杆轴值。无手柄时日志提示 `virtual gamepad UI enabled (no physical device)`。

### 6.6 手柄归一化

**Action 枚举**：`Dpad*`、`A/B/X/Y`、`L/R/ZL/ZR/LB/RB`、`LStick*`、`RStick*`、`Plus/Minus`、`Home/Capture/Touchpad/Extra1/Extra2`。

摇杆判定：死区 0.35 + 角度滞回 8° + 分区数 8（可配 12）。

归一化表：`data/gamepad_profiles.toml`，按 `name_regex`/`vendor`/`product` 匹配 Switch Pro、Xbox、PS4/PS5。

元按键能力探测：`libevdev_has_event_code()` 探测 `BTN_MODE`（Home）、`BTN_Z`/`KEY_SCREENSHOT`/`BTN_SELECT`（Capture）、`BTN_TOUCH`（Touchpad）。探测不到就跳过，`--list-gamepad` 打印可用按键。

---

## 7. IME 后端

### 7.1 接口

```cpp
namespace ime {
struct KeyEvent {
  enum class Type { Char, Backspace, Delete, Enter, Left, Right, Up, Down, Esc, Tab, Space };
  Type type; char32_t ch; uint32_t modifiers;
};
struct Candidate {
  std::string text;        // UTF-8
  std::string annotation;  // 可选标注（翻译），空串=无
  std::string label;
  float score;
};
struct Preedit { std::string text; size_t cursor; };

class ImeBackend {
public:
  virtual ~ImeBackend() = default;
  virtual bool init(const ImeConfig& cfg) = 0;
  virtual void shutdown() = 0;
  virtual const char* name() const = 0;
  virtual void feedKey(const KeyEvent&) = 0;
  virtual void focusIn() = 0;
  virtual void focusOut() = 0;
  virtual bool hasPreedit() const = 0;
  virtual Preedit preedit() const = 0;
  virtual std::vector<Candidate> candidates() const = 0;
  virtual int candidatePage() const = 0;
  virtual int candidatePageCount() const = 0;
  virtual void selectCandidate(int index) = 0;
  virtual void pageNext() = 0;
  virtual void pagePrev() = 0;
  virtual std::string takeCommitted() = 0;
  virtual std::vector<int> pollFds() const { return {}; }
  virtual void onFdReady(int fd) {}
  virtual bool ownsCandidateUi() const = 0;
  // 扩展：异步后端 / 上下文感知
  virtual void pump() {}
  virtual bool isPending() const { return false; }
  virtual void setContext(const std::string&) {}
};
}
```

### 7.2 后端矩阵

| 后端 | 依赖 | 候选 UI | 适用场景 |
|---|---|---|---|
| **`builtin`（默认）** | 无（自带 pinyin.idx + ngram.bin） | 我们画 | 无桌面、无 fcitx5、Switchroot 纯终端 |
| `fcitx5` | D-Bus + fcitx5 | 我们画（数据来自 fcitx5） | 有桌面的终端环境 |
| `ibus` | D-Bus + ibus | 我们画（可选） | fcitx5 不可用时 |

**默认 `backend = "builtin"`**：开箱即用，不需要 D-Bus，最贴合 Switchroot。桌面用户可显式改 `fcitx5` 或 `auto`。

`auto` 探测顺序：`fcitx5` → `ibus` → `builtin`。

### 7.3 builtin 后端

```text
ime/
├── ime_builtin.cpp        # ImeBackend 实现
├── pinyin_buffer.cpp      # 组字串 + 光标
├── candidate_gen.cpp      # pinyin.idx 查询 + 分页
├── ngram_rank.cpp         # ContextBuffer + 重排
└── data/
    ├── pinyin.idx         # 最小版 < 5MB 随仓库
    └── ngram.bin          # bigram/trigram，mmap
```

候选生成：完整拼音查 `pinyin.idx` 前缀匹配 → 无完整匹配退化为逐字匹配 → `NgramRanker` 用最近 1k token 算上下文分 → `final = base * ctx` 排序 → 分页 7 个。选字提交写 PTY master，更新 ContextBuffer。

`setContext()` 接收终端最近文本，供 n-gram 重排使用。`pump()` / `isPending()` 当前空实现（同步后端）。

### 7.4 fcitx5 后端

D-Bus 接 fcitx5，协议参考 `fcitx5-frontend-fbterm`：

```text
服务: org.fcitx.Fcitx5
接口: org.fcitx.Fcitx5.InputContext1
流程: init() → CreateInputContext → feedKey 调 ProcessKeyEvent
      → 监听 CommitString 存队列 → takeCommitted 取走
      → 监听 UpdateLookupTable 更新 candidates()
      → pollFds() 返回 D-Bus fd
```

**`Candidate.annotation` 从 `CandidateWord.comment` 映射**。fcitx5 提供什么注释就显示什么，用户装带译词标注的引擎时翻译自动出现。

`ownsCandidateUi()` 返回 false：fcitx5 只提供候选数据，弹窗由我们画，视觉风格统一。

### 7.5 LLM 输入法

**不内置**。LLM 输入法在生态里的正确位置是“系统输入法内部的增强”。桌面用户想用 LLM，装 fcitx5 + 带 LLM 的引擎插件，我们的程序 `backend = "fcitx5"` 接过去，**零感知、零配置、零代码**。`builtin` 保持传统拼音 + n-gram 的轻量和确定性。

`pump()` / `isPending()` / `setContext()` 是通用异步扩展点，不是 LLM 专属。当前同步后端空实现，将来如果 `builtin` 要加异步增强，挂载点现成。

### 7.6 两种运行模式

| 模式 | 触发 | IME | 输出 |
|---|---|---|---|
| **终端**（默认） | `mode.default = "terminal"` | `builtin` 或 `fcitx5` | PTY master |
| **打字** | `mode.default = "typing"` | 不参与 | uinput 虚拟键盘 |

打字模式不创建 IME 后端，只通过 uinput 发标准键盘按键，中文交给系统 IME。

---

## 8. Overlay（接口预留）

```cpp
namespace ui {
struct OverlayContext { const ScreenBuffer& term; const ImeUiState& ime; int screenW, screenH; };
class Overlay {
public:
  virtual ~Overlay() = default;
  virtual const char* id() const = 0;
  virtual bool onAction(input::Action) { return false; }
  virtual void draw(disp::DisplayBackend&, const OverlayContext&) = 0;
  virtual bool modal() const { return true; }
};
class OverlayStack { push/pop/top/dispatch/drawAll };
class OverlayRegistry { registerOverlay/create/available };
}
```

- **当前 registry 为空**，没有注册任何具体 overlay。
- `Home` → `overlay_open("menu")`、`Capture` → `overlay_open("settings")`，查不到注册项时只记日志，不崩溃、不阻塞输入。
- 渲染顺序：终端 → 输入法状态栏 → 左右面板 → OverlayStack 最后画。
- `modal()` 为 true 时输入不再传给 IME/PTY；`B` 键由 overlay 通用关闭映射优先消费。

---

## 9. 配置文件结构

```toml
[display]
backend = "auto"          # auto | fbdev | drm | egl_gles | glx | headless
width = 1280
height = 720
font = "monospace:lang=zh-cn"
font_size = 14

[input]
deadzone = 0.35
sectors = 8
profiles = "data/gamepad_profiles.toml"
mouse = true
touch = true

[input.gamepad]
device = "auto"
virtual = "auto"          # auto | always | never

[input.keyboard]
route = "pty"             # pty | ime | action
layout = "us"

[input.pointer]
click_hit_test = true
selection = true
scroll = true

[terminal]
shell = "/bin/bash"
cols = 80
rows = 24
scrollback = 1000

[ime]
backend = "builtin"       # builtin（默认）| fcitx5 | ibus | auto
page_size = 7
ui = "statusbar"
file = "data/keymap.toml"

[ime.builtin]
pinyin_index = "data/pinyin.idx"
ngram = "data/ngram.bin"
context_window = 1000

[ui]
annotation_max_chars = 12
annotation_style = "inline"    # inline | below

[mode]
default = "terminal"      # terminal | typing

[typing]
inject = "uinput_key"
```

`data/keymap.toml` 关键映射：

```toml
[[map]] when = { mode="terminal", layer="alpha", action="LStickUp" }    do = { type="ime_letter", value="i" }
[[map]] when = { mode="terminal", layer="alpha", action="LStickPress" } do = { type="ime_letter", value="o" }
[[map]] when = { mode="terminal", layer="alpha", action="RStickPress" } do = { type="ime_letter", value="b" }
[[map]] when = { mode="terminal", layer="symbol", action="LStickUp" }   do = { type="ime_letter", value="，" }
[[map]] when = { mode="terminal", layer="symbol", action="RStickUp" }   do = { type="ime_letter", value="7" }
[[map]] when = { mode="terminal", action="L" }       do = { type="ime_toggle_layer" }
[[map]] when = { mode="terminal", action="Y" }       do = { type="ime_toggle_lang" }
[[map]] when = { mode="terminal", action="R" }       do = { type="ime_space" }
[[map]] when = { mode="terminal", action="A" }       do = { type="ime_commit_or_enter" }
[[map]] when = { mode="terminal", action="B" }       do = { type="ime_backspace" }
[[map]] when = { mode="terminal", action="X" }       do = { type="ime_delete" }
[[map]] when = { mode="terminal", action="Tab" }     do = { type="cand_next_page" }
[[map]] when = { mode="terminal", action="ZL" }      do = { type="cursor_left" }
[[map]] when = { mode="terminal", action="ZR" }      do = { type="cursor_right" }
[[map]] when = { mode="terminal", action="Shift", double_click = true } do = { type="ime_toggle_caps" }
[[map]] when = { mode="terminal", action="Home" }    do = { type="overlay_open", value="menu" }
[[map]] when = { mode="terminal", action="Capture" } do = { type="screenshot" }
[[map]] when = { mode="typing", action="A" }         do = { type="uinput_key", value="KEY_ENTER" }
[[map]] when = { mode="typing", action="LStickUp" }  do = { type="uinput_key", value="KEY_I" }
```

---

## 10. 目录结构

```text
switch-ime-term/
├── CMakeLists.txt
├── README.md
├── docs/  (ui-mockup.html / IMPLEMENTATION.md / PLAN.md)
├── cmake/aarch64-linux-gnu.cmake
├── src/
│   ├── main.cpp
│   ├── config/config.{h,cpp}
│   ├── event/event_loop.{h,cpp}
│   ├── platform/
│   │   ├── display_backend.h / display_registry.{h,cpp}
│   │   ├── display_headless.cpp / display_fbdev.cpp / display_drm.cpp
│   │   ├── display_egl_gles.cpp / display_glx.cpp
│   │   ├── input_manager.cpp / input_gamepad_evdev.cpp
│   │   ├── input_keyboard_evdev.cpp / input_pointer_evdev.cpp
│   │   └── sink_pty.cpp / sink_uinput.cpp
│   ├── term/ pty.cpp / vterm_bridge.cpp / screen_buffer.cpp / terminal_session.cpp
│   ├── render/ font_manager.cpp / glyph_atlas.cpp / renderer.cpp / dirty_rect.cpp / hit_test.cpp
│   ├── input/ gamepad.cpp / gamepad_normalize.cpp / action_map.cpp / mode_fsm.cpp
│   ├── ime/ ime_backend.h / ime_registry.cpp / ime_builtin.cpp
│   │        / ime_fcitx5.cpp / ime_ibus.cpp
│   │        / pinyin_buffer.cpp / candidate_gen.cpp / ngram_rank.cpp
│   └── ui/ overlay.h / overlay_stack.cpp / overlay_registry.cpp
├── data/ keymap.toml / gamepad_profiles.toml / pinyin.idx / ngram.bin
└── tests/ + golden/
```

---

## 11. 构建

```bash
# CachyOS 开发
cmake -B build -DBACKEND_GLX=ON -DBACKEND_HEADLESS=ON -DENABLE_DEBUG_OVERLAY=ON
cmake --build build -j$(nproc)

# Switchroot 部署
cmake -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake \
  -DBACKEND_FBDEV=ON -DBACKEND_EGL_GLES=ON -DBACKEND_GLX=OFF
cmake --build build-arm -j$(nproc)

# CI
cmake -B build-ci -DBACKEND_HEADLESS=ON -DBACKEND_FBDEV=OFF -DBACKEND_EGL_GLES=OFF
```

依赖：libvterm、freetype2、fontconfig、libevdev、xkbcommon、toml++。可选：libdrm、libEGL+GLESv2、X11+libGL、libpng、libdbus。

---

## 12. 测试

| 层级 | 工具 | 内容 |
|---|---|---|
| 单元测试 | Catch2/GoogleTest | PinyinBuffer、CandidateGen、ActionMap、GamepadNormalize、HitTest |
| 像素回归 | headless + libpng + diff | 固定 fixture 渲染一帧，对比 golden，容差 2% |
| 交互测试 | `--script` 脚本模式 | 注入 Action 序列，验证终端状态和提交序列 |
| 实机测试 | Switch + Xbox + 鼠标 | M3/M3.5/M4/M9 |
| 兼容性 | CachyOS/Switchroot/树莓派 | fbdev/DRM/EGL |

CI 只跑 headless，实机手动触发。

---

## 13. 布局求解（像素推导规则）

> **已被 `UI_DESIGN.md` 取代（2026-10-27）**：UI 改成纯字符网格，不再有圆角/描边/辉光/侧面板图形。
> 现行数值（与 `ui-mockup.html` 实测对齐）：1280×720；面板各 220px、终端 840px、顶栏 32px、底栏 26px；
> 终端内边距 10/14/8，字符格 8×21，网格 101×30；摇杆 180×180 的 5×5 字符矩阵（格 36，中心 3×3 + 外圈四个正方向）；
> 十字键 26px 格、ABXY 28px 格；所有分隔线 1px `#1a1a26`。实现见 `src/ui/layout.h`、`src/ui/panels.cpp`。
> 下面 13.1–13.7 是旧图形版（216px 面板 / 圆角按键）的历史推导，实现已删除，仅留作记录。

本节定义**从窗口尺寸到每一个像素**的推导链。实现时不需要任何魔法数字，所有数值由此链算出；`ui-mockup.html` 里的像素值是 1280×720 基准下的结果，不是常量来源。

### 13.1 设计基准与等比缩放

基准画布 `1280×720`。所有面板/摇杆/按键/标题栏的尺寸按单一比例缩放：

```text
s = min(W / 1280, H / 720)        // 等比，不拉伸
```

缩放后的设计空间坐标统一取整：`round(v * s)`，最小 1px。剩余区域（非 16:9 屏）居中留黑，**不拉伸**。

### 13.2 推导链

```text
s        = min(W/1280, H/720)
panel_w  = round(216 * s)                  // 单个侧面板宽，基准 216
tbar_h   = titlebar ? round(38 * s) : 0    // 标题栏，fbdev 默认关
pad_x    = round(20 * s)                   // 终端左右内边距
pad_top  = round(12 * s)
pad_bot  = round(14 * s)

term_x   = panel_w
term_w   = W - 2*panel_w                   // 恒为 66.25% 画面宽
term_y   = tbar_h
term_h   = H - tbar_h

cell_w   = floor(ascii_advance_px)         // 由字体实测，见 13.3；必须整数
cell_h   = round(line_spacing * font_size) // line_spacing 默认 1.5
cols     = (term_w - 2*pad_x) // cell_w
rows     = (term_h - pad_top - pad_bot) // cell_h - 1   // 减掉底部 IME 状态栏 1 行
```

`cols`/`rows` 是**输出量**。PTY 的 `TIOCSWINSZ` 用 `rows × cols`（已扣状态栏）。

### 13.3 字体与 cell 度量（实测，不可假设）

字体必须**运行时测**，不能写死 `0.6em`：

1. `fontconfig` 按 `font` 配置匹配，取 Unicode 覆盖 `U+4E00`–`U+9FFF` 的等宽字体。
2. 校验 ASCII 等宽：`U+0020`–`U+007E` 全部 advance 相等，否则该字体不可用于网格终端，换下一个候选。
3. `cell_w = ASCII advance`；再校验 `CJK advance == 2 * cell_w`（容差 ±1px）。不满足时 `cell_w = max(cell_w, CJK_advance/2)`，宁可字距松也不重叠。
4. `cell_h` 取 `round(line_spacing * font_size)`，不用 FreeType 的 `metrics.height`（CJK 字体的 height 普遍偏紧，会造成行贴行）。

**本机实测（CachyOS，14px）**：`Noto Sans CJK`（TTC 内 Sans 索引 0–4）ASCII 不等宽（`M`=11.375px / `a`=7.875px），**不能用于终端**；`Noto Sans Mono CJK`（索引 5–9）ASCII 恒为 7.0px、CJK 14.0px，比例正好 1:2，可用。

**坑 1**：`fc-match "monospace:lang=zh-cn"` 在本机解析到 `Noto Sans Mono CJK KR`（韩式汉字字形），中文用户会觉得部分字形不对。配置里必须显式写 `font = "Noto Sans Mono CJK SC"`，或匹配后断言 `FAMILY` 含 `SC`。

**坑 2**：同一个 `.ttc` 文件里 Sans 与 Mono 是不同 face index，FreeType 必须按 face index 加载，不能只给路径。

### 13.4 `cols`/`rows` 与 `font_size` 的过约束处理

配置里 `display.font_size` 和 `terminal.cols/rows` 同时存在时必然冲突，必须定方向。规则：

```toml
[terminal]
sizing = "fill"      # fill（默认）：按 font_size 算格数，多余空间全部吃掉
                     # fixed：严格 cols×rows，字太小时自动降 font_size 直到装得下
cols = 80            # fill 模式下作为「最小列数」下限，不足则自动缩字号
rows = 24
```

`sizing = "fixed"` 的求解：从 `font_size` 往下试到 8，取第一个满足 `cols×cell_w ≤ term_w - 2*pad_x` 且 `rows×cell_h ≤ term_h - pads + 1` 的值。找不到则用 8 并告警。

### 13.5 常见分辨率算例（font_size=14，line_spacing=1.5）

| 分辨率 | s | 面板宽 | cell | cols×rows（含状态栏） | 终端占比 |
|---|---|---|---|---|---|
| 1280×720（Switch 底座/掌机） | 1.00 | 216 | 7×21 | 115×31 → PTY 115×30 | 66% |
| 1280×720（无标题栏，fbdev） | 1.00 | 216 | 7×21 | 115×33 → PTY 115×32 | 66% |
| 1920×1080 | 1.50 | 324 | 7×21 | 173×46 → PTY 173×45 | 66% |
| 1024×600（树莓派屏） | 0.80 | 173 | 7×21 | 92×26 → PTY 92×25 | 66% |
| 854×480 | 0.67 | 144 | 7×21 | 77×22 → PTY 77×21 | 66% |

**854×480 上 77 列 < 80 列**：这正是 13.4 规则必须存在的理由——默认 80×24 在该分辨率装不下，需自动降到 font 13 或接受 77 列。

反过来，要 80×24：1280×720 可用 font 17（89 列 × 24 行），1024×600 可用 font 14。

### 13.6 侧面板内部布局（基准坐标 × s）

| 元素 | 基准尺寸 | 位置 |
|---|---|---|
| 肩键 L/R | 宽 92%×面板、高 16 | 顶部，左对齐/右对齐 |
| ZL/ZR | 宽 62%、高 14 | 肩键下方 |
| 摇杆盘 | 190×190 圆 | 面板水平居中 |
| 摇杆内格 | 3×3，每格 42，gap 3 | 盘心 |
| 摇杆外圈键 | 26×26 | 盘四边（N/E/S/W） |
| 十字键 | 3×3，每格 26，gap 3 | 摇杆下 |
| ABXY | 3×3，每格 30，gap 3 | 右面板摇杆下 |
| 面板内边距 | 9 上下 / 12 左右 | — |

面板宽 216 恰好容纳 190 的盘 + 12×2 内边距 + 少量余量，改面板宽必须同时改盘径，否则溢出。**半径 < s 时外圈键会与内格重叠**，因此 `s < 0.72` 时外圈键改为半透明叠加绘制，hit region 优先外圈。

### 13.7 候选弹窗几何

弹窗不占网格行，是终端区内的**浮层**，锚在 PTY 光标格：

```text
popup_x = term_x + pad_x + cursor_col * cell_w
popup_y = term_y + pad_top + (cursor_row + 1) * cell_h   // 光标下一行
popup_max_w = term_w - 2*pad_x
height = 2 行（候选行 + meta 行）+ 2*padding(4*s)
```

每页实际容量 = 在 `popup_max_w` 内能放下的候选数，**不是固定的 `page_size`**：

```text
w_i = label_w + text_w(candidate.text) + gap + annotation_w(截断到 annotation_max_chars)
```

`page_size` 作为上限，超出则换行到第 2 行候选或截断分页。光标在最后一行时弹窗翻到上方（`popup_y` 减去 1 行高度）。

---

## 14. 摇杆字母布局

### 14.1 输入语义（原文缺失，此处定义）

**回中式提交（commit-on-return）**：摇杆推过死区后，按角度高亮一个格；**回到死区才提交**该字母。推出去后可以滑到相邻格改主意，只有在中心松手才算输入。

- 不允许「进入扇区立即提交」：连续两字母会连发，且推的路径上会扫过中间格。
- 不连发、不重复加速：拼音每个字母都要精确，重复由下一推产生。
- 高亮态在 `stick_pressed_highlight` 层，渲染器只读 Action 状态，不读原始轴值。

判定参数（对应 `[input] deadzone/sectors`）：

```toml
deadzone = 0.35          # r/R < 0.35 视为回中
sectors  = 8             # 每摇杆分区数
hysteresis_deg = 8       # 已进入扇区后，需再偏 8° 才切到相邻扇区
```

### 14.2 字母表（26 字母全覆盖，与 `ui-mockup.html` 一致）

**左摇杆（13）** — 元音全在左，与右摇杆的辅音交替，拼 `nihao` 天然左右手轮换：

| 位置 | 角度（0°=右，逆时针） | 字母 |
|---|---|---|
| 内 上 | 90° | `i` |
| 内 右上 | 45° | `h` |
| 内 右 | 0° | `n` |
| 内 右下 | 315° | `z` |
| 内 下 | 270° | `g` |
| 内 左下 | 225° | `u` |
| 内 左 | 180° | `a` |
| 内 左上 | 135° | `e` |
| 中心 | r < deadzone + 按下 | `o` |
| 外 上 | 90° | `m` |
| 外 右 | 0° | `t` |
| 外 下 | 270° | `l` |
| 外 左 | 180° | `r` |

**右摇杆（13）**：

| 位置 | 角度 | 字母 |
|---|---|---|
| 内 上 | 90° | `s` |
| 内 右上 | 45° | `x` |
| 内 右 | 0° | `y` |
| 内 右下 | 315° | `v` |
| 内 下 | 270° | `j` |
| 内 左下 | 225° | `c` |
| 内 左 | 180° | `d` |
| 内 左上 | 135° | `q` |
| 中心 | 按下 | `b` |
| 外 上 | 90° | `f` |
| 外 右 | 0° | `w` |
| 外 下 | 270° | `p` |
| 外 左 | 180° | `k` |

并集验证：`a b c d e f g h i j k l m n o p q r s t u v w x y z` = 26，无重复无遗漏。

**`ü`**：不在布局内，按拼音惯例用 `v` 代替（`lv` → 绿/律/旅）。

### 14.3 符号层（L 键切换）与数字层

L 键把**两个面板同时**切到符号/数字层（当前 mockup 只切左面板 punct + 右面板 num，等于一次切换同时换标点和数字）。

左面板符号层（13），位置与字母层一一对应：

| 位置 | 字符 | 位置 | 字符 |
|---|---|---|---|
| 内 左上 (NW) | `；` | 内 上 (N) | `，` |
| 内 右上 (NE) | `：` | 内 左 (W) | `？` |
| 中心 (C) | `！` | 内 右 (E) | `。` |
| 内 左下 (SW) | `、` | 内 下 (S) | `“` |
| 内 右下 (SE) | `”` | 外 上 (U) | `（` |
| 外 右 (R) | `）` | 外 下 (D) | `—` |
| 外 左 (L) | `…` | | |

右面板数字层（13 格放 10 个数字）：

| 位置 | 字符 | 位置 | 字符 |
|---|---|---|---|
| 内 左上 (NW) | `7` | 内 上 (N) | `8` |
| 内 右上 (NE) | `9` | 内 左 (W) | `4` |
| 中心 (C) | `5` / 按住变 `0` | 内 右 (E) | `6` |
| 内 左下 (SW) | `1` | 内 下 (S) | `2` |
| 内 右下 (SE) | `3` | 外 上 (U) | `+` |
| 外 右 (R) | `−` | 外 下 (D) | `×` |
| 外 左 (L) | `÷` | | |

**该层与 14.2 的「中心=按下」冲突（见 15.2 D2）**：数字层的中心格要区分 5 和 0，而字母层的中心格本身就是「按下」，两套语义叠在同一个物理动作上。

### 14.4 keymap 写法

外圈 4 键需要与内 8 扇区区分。Action 枚举增加半径维度，keymap 里用带后缀的动作名：

```toml
[[map]] when = { mode="terminal", layer="alpha", action="LStickInnerUp" }   do = { type="ime_letter", value="i" }
[[map]] when = { mode="terminal", layer="alpha", action="LStickOuterUp" }   do = { type="ime_letter", value="m" }
[[map]] when = { mode="terminal", layer="alpha", action="LStickPress" }     do = { type="ime_letter", value="o" }
[[map]] when = { mode="terminal", layer="alpha", action="RStickInnerUp" }   do = { type="ime_letter", value="s" }
[[map]] when = { mode="terminal", layer="alpha", action="RStickOuterUp" }   do = { type="ime_letter", value="f" }
[[map]] when = { mode="terminal", layer="alpha", action="RStickPress" }     do = { type="ime_letter", value="b" }
```

即 `Stick` 动作拆成 `{Inner,Outer} × {Up,Right,Down,Left,UpRight,DownRight,DownLeft,UpLeft}`（外圈只有 4 个正交方向）+ `StickPress`。

---

## 15. 待决项（实现前必须拍板）

按阻塞程度排序。每条给了推荐方案，未拍板前对应模块不要动工。

### 15.1 D1 · 外圈 4 字母怎么到达（阻塞 M4）

**冲突**：`ui-mockup.html` 写「R3/L3 点击摇杆圆环 · 切换第二层」，即外圈字母要靠一个 **latch 状态**进入；但 `keymap.toml` 里 `LStickPress`/`RStickPress` 已被「中心字母」占用，同一个物理按下键出现两种语义。

**候选**：

| 方案 | 说明 | 代价 |
|---|---|---|
| A 半径双环（推荐） | 内环 `0.35 ≤ r < 0.78`，外环 `r ≥ 0.82` 回差 0.04；外圈在 4 个正交角上，与内环角度不重叠 | 需要可靠的外推到底，`r` 只能靠手感；8-BIT 轴够用 |
| B L3 按住 | 按住 L3 时内 8 格失效、外 4 格生效 | m/t/l/r/f/w/p/k 共 8/26 字母要额外按，且「按下+推方向」手感差 |
| C 12 扇区单环 | `sectors = 12`，12×2 内环 + 2 个中心按下 = 26，取消外圈 | 每扇区 30°，格子要重画成 12 格环形，面板视觉要改 |

### 15.2 D2 · 中心格由谁触发（阻塞 M4）

**冲突**：mockup 标注「RB 居中触发中心字母（数字层 5/0）」，但 `ACTION` 枚举里的 `LB/RB` 在 Switch 手柄上**不存在**（Switch 只有 L/R/ZL/ZR），且 keymap 里写的是 `LStickPress`。

**推荐**：统一为 `LStickPress`/`RStickPress`（即 L3/R3），删掉 mockup 的 `RB` 提示。代价：数字层的 5/0 需要另一种区分（见 15.3）。

### 15.3 D3 · 数字层 5 与 0 的区分（阻塞 M4 数字输入）

mockup 用「摇杆按下」切 5↔0，但这与 D2 的按下=中心字母重复。

**推荐**：数字 `0` 放到外圈某格（外圈 4 格只用了 `+ − × ÷`，即 4 格里挤 3 个运算符 + 1 个 0），或长按中心格 = 0。

### 15.4 D4 · 候选词如何选中第 2–7 个（阻塞 M4，硬缺口）

**缺口**：keymap 里**没有任何 `cand_select` 映射**。候选弹窗显示 7 个候选并带 `1`–`7` 序号，但手柄没有数字键。现有映射只有 `Tab = 下一页` + `A = 确认`，即**只能选第一个候选**。`PLAN.md` G2 的验收（摇杆拼 `nihao` 选「你好」）刚好是第一个，掩盖了这个问题。

**推荐**：候选态下**劫持十字键**——上/下/左/右移动候选高亮，`A` 确认，`Tab` 翻页。非候选态十字键恢复 Tab/Ctrl/Shift/Alt 修饰功能。这是模态劫持，`ModeFSM` 里需要一个 `composing` 状态。

### 15.5 D5 · ZL/ZR 的「光标移动」指哪个光标（阻塞 M4）

**缺口**：keymap 只有 `cursor_left/right`，没区分两种光标：

- compose 串内光标（内置 IME 的组字串，还没提交给 shell）
- 终端光标（shell 的 readline，需要发 `ESC[D`/`ESC[C`）

**推荐**：`cursor_left/right` 在 `composing` 状态下移动 compose 光标；否则发 `ESC[D`/`ESC[C` 给 PTY。keymap 的 `when` 增加 `composing = true|false` 条件。

### 15.6 D6 · compose 串画在哪（阻塞 M2/M4）

`ScreenBuffer` 里没有 compose 串的位置（它不属于 PTY 输出）。

**推荐**：compose 串作为浮层画在 **PTY 光标格起始处**，向右占用 `wcwidth` 个格，超出右边界则从下一行行首折行（不修改 `ScreenBuffer`，只改渲染时的光标映射）。底部状态栏那 1 行只放 `层 / 语言 / 页 x/y / Caps`，不放组字串。这样 compose 串和 shell 提示符自然接续，符合 mockup 的 `input-line` 效果。

### 15.7 D7 · 状态栏位置（阻塞 M2）

**冲突**：`IMPLEMENTATION.md §4.3` 说底部保留 1 行做 IME 状态栏；`ui-mockup.html` 把 layer/lang/caps 三个 pill 放在终端**右上角**。

**推荐**：两种都保留，`[ui] statusbar = "row" | "float"`，默认 `row`（fbdev 更省事、不遮内容），`float` 供大屏用。

### 15.8 D8 · `terminal.cols/rows` 与 `font_size` 的过约束（阻塞 M1）

见 13.4。需在配置里加 `[terminal] sizing = "fill" | "fixed"`，默认 `fill`。

### 15.9 D9 · 标题栏在 fbdev 上是否有意义（阻塞 M1，低）

mockup 有 38px 标题栏（红黄绿三点 + `tty1 — switch-input@arch`）。fbdev 上白占 38px 且没有窗口管理器语义。

**推荐**：`[ui] titlebar = true|false`，fbdev/drm 上默认 `false`。

### 15.10 D10 · 侧面板占用 34% 屏宽（产品决策）

两块面板各 216/1280 = 16.875%，合计 33.75%。1280×720 下终端只剩 848px（115 列），仍够用；但 854×480 上只剩 77 列，装不下 80 列 TUI。

**推荐**：`[input.gamepad] virtual = "never"` 时面板完全隐藏给终端全宽（已有配置项，需在渲染层落实）；另加 `[ui] panel_scale = 1.0`，允许面板按 0.5–1.0 缩小。

### 15.11 D11 · 屏幕比例不是 16:9 时（阻塞 M8，低）

等比缩放 + 居中留黑是推荐方案，但要在 `DisplayBackend` 之上加一层 letterbox 变换，或者在 `Renderer` 里全局限定坐标。**推荐后者**：`Renderer` 持有 `s` 和偏移，所有绘制走 `scale(Rect)`，后端不感知。

---

## 17. 输入法行为与自检（当前实现）

### 17.1 IM 按键代数（全拼 / 键盘）
路由在 `src/app/ime_route.cpp`（可脱离 UI 单测）：

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

### 17.2 九键面板（已删除）

旧的手柄九键面板/弹窗状态机已于 2026-11 整体删除，现状见 `docs/LAYOUT-v2.md` 与 `docs/ARCH.md` §4。

### 17.3 修饰键（防误操作）
*   Ctrl/Alt + 字母**只在英文态**当组合键（`0x03` / `ESC+字母`）；中文态不抢字母。
*   中文态大写按小写组字；组字中不认识的控制字节被吞。
*   组合键走"明确字母"路径：Ctrl（sticky）→ LB 弹窗选字母 → LB 上屏。

### 17.4 虚拟鼠标（RB/RT）
`src/input/uinput_mouse.*`：懒创建 `/dev/uinput` 设备（EV_KEY + BTN_LEFT/BTN_RIGHT），
只发点击不发 REL，指针位置不动。`--probe-mouse` 会建设备、查 `/sys/class/input` 是否注册、点一下。
右摇杆移动指针仍未实现。

### 17.5 自检命令
```bash
./build/switch-ime-term --audit-ime   # IM 按键代数 + 修饰键
./build/switch-ime-term --audit-input  # 手柄按键/摇杆映射 + keymap 装配
./build/switch-ime-term --probe-mouse # 虚拟鼠标设备注册 + 点击
```

## 16. 依赖现状（CachyOS 实测）

| 依赖 | 状态 |
|---|---|
| freetype2 2.15.0 / fontconfig 2.18.3 / libevdev 1.14.0 / libdrm 2.4.134 / libpng 1.6.59 / dbus 1.16.2 / libglvnd 1.7.0 / xorg-xwayland 24.1.14 | 已装 |
| libvterm | **缺**（`vterm` AUR 或源码构建） |
| xkbcommon | **缺**（`libxkbcommon`） |
| toml++ | **缺**（`tomlplusplus` AUR 或 fetch） |

**GLX 后端风险**：本机是纯 Wayland（无 X11 会话）。`glx` 后端依赖 X11 + libGL，只能跑在 XWayland 里，且 `xorg-xwayland` 已装但**没有 Xorg 服务**——意味着 `--backend glx` 在裸 TTY 下不可用，只能从 Wayland 会话的终端里启动。开发期建议 **`glx` 优先级低于 `egl_gles`**，或者直接给 CachyOS 加一个 **Wayland 原生后端**（`wl` + EGL/GBM，走 `wayland-client`），否则 G3「桌面同一份代码可运行」在纯 Wayland 机上要靠 XWayland 兜底，属于额外依赖。
