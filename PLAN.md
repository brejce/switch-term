# switch-ime-term 项目计划书

> 版本：v1.2
> 定位：手柄驱动的跨平台虚拟终端 + 可插拔中文输入法
> 目标平台：Nintendo Switch（Switchroot Linux）、x86-64 桌面 Linux、其他 fbdev/DRM/EGL Linux
> 配套：`IMPLEMENTATION.md`（实现规范）、`ui-mockup.html`（视觉基准）

---

## 1. 项目概述

`switch-ime-term` 是一个**独立的 Linux 终端程序**，自己实现终端模拟和中文输入，不依赖桌面环境、不依赖系统 IME、不需要内核补丁。核心能力：

- **标准手柄输入**：读 evdev，支持 Switch Pro/Joy-Con、Xbox、PlayStation。
- **中文显示**：fontconfig + FreeType 自己渲染字符网格，支持宽字符。
- **四类可插拔后端**：显示、输入、输出、IME 全部接口抽象。
- **三源并行输入**：手柄、物理键盘、鼠标/触屏同时可用，互不抢夺。
- **两种运行模式**：终端模式（包裹 shell，输出到 PTY）和打字模式（uinput 虚拟键盘）。
- **默认内置 IME**：开箱即用，不需要 D-Bus、不需要 fcitx5，最贴合 Switchroot 纯终端环境。

**与 Paper S3 / ESP-IDF / ECS 框架无任何代码依赖**。Paper S3 的拼音索引仅作为离线数据来源之一，可导出为 `pinyin.idx`，不是运行时依赖。

---

## 2. 设计原则

1. **平台解耦**：四类后端全部接口抽象，上层渲染和逻辑不含 `#ifdef` 平台分支。
2. **默认内置，可选系统**：IME 默认 `builtin`，Switch 开箱即用；桌面用户可显式切 `fcitx5`/`ibus`。
3. **能力探测 + 优雅降级**：手柄按键、显示后端、IME 服务全部运行时探测，探测不到就降级，不崩溃。
4. **并行输入，互不抢夺**：手柄走 IME，物理键盘直写 PTY，指针走 HitTest，三者事件队列独立处理。
5. **事件驱动重绘**：只在 PTY 数据、输入事件、定时器到期时重绘。
6. **单线程事件循环**：epoll 统一管理 PTY、手柄、键盘、指针、D-Bus、定时器。
7. **配置驱动**：按键映射、手柄归一化、IME 选择、显示后端全部 TOML 配置。
8. **接口预留，内容后补**：Overlay 只留接口，registry 为空，不阻塞主目标。

---

## 3. 目标与非目标

### 3.1 目标

| 编号 | 目标 | 验收方式 |
|---|---|---|
| G1 | 在 Switchroot 上运行，fbdev 显示中文终端 | 实机 `ls`、`vim`、`echo 你好` 正常 |
| G2 | 手柄完成全部终端操作，无需物理键盘 | 摇杆拼 `nihao`，选“你好”后 shell 收到 UTF-8 |
| G3 | x86-64 桌面 Linux 上同一份代码可运行 | CachyOS 上 `--backend glx` 开窗口调试 |
| G4 | **默认使用内置 IME**，兼容性最高，最贴合 Switchroot 纯终端状态 | 无 D-Bus 环境下启动，日志 `ime=builtin`，选字正常 |
| G5 | 打字模式下可作为通用手柄键盘输入到桌面应用 | gedit / 浏览器中用手柄输入英文和快捷键 |
| G6 | 显示后端可替换，新增后端不改渲染代码 | 新增后端只需新增 `.cpp` + 注册 |
| G7 | **物理键盘、鼠标、手柄并行输入，互不抢夺** | 三者同时接，各自操作互不干扰 |
| G8 | **UI 元素支持鼠标点击和触屏点按** | 鼠标点摇杆格子触发等价 Action；触屏点候选词可选中 |
| G9 | **无物理手柄时手柄 UI 仍可显示和操作** | `--virtual-gamepad always` 下鼠标可操作全部面板元素 |

### 3.2 非目标

- 不实现完整图形桌面环境。
- 不替代 fcitx5/ibus，只是它们的“前端适配器”之一。
- 不支持 Windows/macOS（架构不排斥，不在当前范围）。
- 不实现 Overlay 具体内容（菜单、设置），只预留接口。
- **不内置 LLM 输入法**。LLM 走 fcitx5 生态，`builtin` 保持传统拼音 + n-gram。
- 不做多线程渲染或 GPU 计算加速。

---

## 4. 系统架构

见 `IMPLEMENTATION.md` 第 1 节架构图。四类可插拔后端 + 输入三源，单线程 epoll 事件循环。

---

## 5. 模块设计

详见 `IMPLEMENTATION.md`。要点：

- **显示**：`DisplayBackend` 接口，`fillRect`/`drawTexture` 两类绘制命令，后端矩阵 fbdev/drm/egl_gles/glx/headless。
- **终端**：Pty + libvterm + ScreenBuffer，底部 1 行输入法状态栏。
- **渲染**：fontconfig + FreeType + LRU atlas，无后端分支。
- **输入**：三源并行，手柄归一化 → Action → ActionMap；键盘按 route；指针走 HitTest。
- **IME**：`builtin`（默认）/`fcitx5`/`ibus`，`Candidate.annotation` 支持译词标注，`fcitx5` 的 `comment` 映射过来。LLM 不内置。
- **Overlay**：接口预留，registry 为空。

---

## 6. 配置文件

> UI 层规范已独立到 `UI_DESIGN.md`（纯字符网格，无图形块）。本文档中的 `[ui]` 配置项只覆盖 IME 候选栏。


见 `IMPLEMENTATION.md` 第 9 节完整结构。关键默认值：

- `ime.backend = "builtin"`
- `input.keyboard.route = "pty"`
- `input.gamepad.virtual = "auto"`
- `mode.default = "terminal"`
- `ui.annotation_max_chars = 12`，`annotation_style = "inline"`
- `terminal.sizing = "fill"`（`cols/rows` 只是下限，实际格数由字号推导）
- `ui.titlebar = false`（fbdev/drm 上），`ui.statusbar = "row"`
- `input.hysteresis_deg = 8`

布局推导规则见 `IMPLEMENTATION.md` 第 13 节，摇杆字母表见第 14 节。**15 节列出的 D1–D11 未拍板前，对应模块不要动工。**

---

## 7. 目录结构

见 `IMPLEMENTATION.md` 第 10 节。

---

## 8. 构建与部署

```bash
# CachyOS 开发
cmake -B build -DBACKEND_GLX=ON -DBACKEND_HEADLESS=ON -DENABLE_DEBUG_OVERLAY=ON

# Switchroot 部署
cmake -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake \
  -DBACKEND_FBDEV=ON -DBACKEND_EGL_GLES=ON
scp build-arm/switch-ime-term switch:/home/user/
scp -r data switch:/home/user/

# CI
cmake -B build-ci -DBACKEND_HEADLESS=ON -DBACKEND_FBDEV=OFF -DBACKEND_EGL_GLES=OFF
```

udev 权限：`SUBSYSTEM=="input", GROUP="input", MODE="0660"`，用户加入 `input` 组。

---

## 9. 数据资产

| 文件 | 来源 | 大小 | 说明 |
|---|---|---|---|
| `data/pinyin.idx` | 公开语料 + 自整理；可选从 Paper S3 导出 | 最小版 < 5MB | `(pinyin, candidate_utf8, freq)` |
| `data/ngram.bin` | 离线生成，bigram/trigram 哈希表 | 视语料 | 提供 `tools/build_ngram.py` |
| `data/keymap.toml` | 随仓库 | 小 | 按键映射 |
| `data/gamepad_profiles.toml` | 随仓库 | 小 | Switch/Xbox/PS 归一化表 |

最小版随仓库，保证 clone 下来就能跑内置 IME。

---

## 10. 里程碑与验收

| 里程碑 | 内容 | 验收标准 |
|---|---|---|
| **M0** 骨架 | CMake、配置、日志、epoll、后端注册表 | `--help`/`--list-backends`/`--list-gamepad`/`--print-config` 正常；Ctrl+C 退出 |
| **M1** 终端模拟 | PTY + libvterm + fbdev ASCII | CachyOS 上 `ls`/`vim` 正常；光标、清屏、滚动正确 |
| **M2** 中文渲染 | fontconfig + FreeType + atlas + 宽字符 | `echo 你好世界` 正确显示，无方块 |
| **M3** 手柄输入 | evdev 归一化 + ActionMap + 终端控制 | Xbox 手柄方向键在 `vim` 里移动；A=回车、B=退格 |
| **M3.5** 三源并行 + 虚拟手柄 UI | 键盘/指针源 + HitTest + `virtual` 开关 | 物理键盘打字不触发手柄映射；鼠标点摇杆格子触发等价 Action；无手柄时面板可点击 |
| **M4** 内置 IME | 拼音状态机 + 候选 + 底部状态栏 + 选字写 PTY | 摇杆拼 `nihao` 选“你好”后 shell 收到 UTF-8；鼠标点候选可选中 |
| **M5** n-gram 重排 | 离线表 + ContextBuffer + 排序 | 相同拼音在不同前文下候选顺序不同 |
| **M6** 系统 IME 接入 | fcitx5 D-Bus 后端 + `auto` 探测 | CachyOS 桌面下有 fcitx5 时日志 `ime=fcitx5`，选字正常 |
| **M7** 打字模式 | uinput 虚拟键盘 + 配置切换 | CachyOS 桌面里手柄输入到 gedit/浏览器 |
| **M8** 显示后端扩展 | DRM + EGL + headless 像素回归 | Switch 上 EGL 可用；CI 对比基准图 |
| **M9** Switch 移植调优 | aarch64 交叉编译 + 摇杆调参 + 触屏 + 电源 | Switchroot 实机全流程通过；触屏点按正常 |
| **M10** 接口预留验证 | OverlayStack + Registry + 扩展虚函数空实现 | Home/Capture 按下日志“未注册”，不影响终端 |

M1–M7 在 CachyOS 完成，M8–M9 才碰 Switch 硬件。

---

## 11. 测试策略

| 层级 | 工具 | 内容 |
|---|---|---|
| 单元测试 | Catch2/GoogleTest | PinyinBuffer、CandidateGen、ActionMap、GamepadNormalize、HitTest |
| 像素回归 | headless + libpng + diff | 固定 fixture 对比 golden，容差 2% |
| 交互测试 | `--script` 脚本模式 | 注入 Action 序列验证状态 |
| 实机测试 | Switch + Xbox + 鼠标 | M3/M3.5/M4/M9 |
| 兼容性 | CachyOS/Switchroot/树莓派 | fbdev/DRM/EGL |

CI 只跑 headless，实机手动触发。

---

## 12. 风险与应对

| 风险 | 影响 | 应对 |
|---|---|---|
| Switch fbdev 刷新慢 | 按键高亮延迟 | 脏矩形 + A8 特化；实测卡顿切 EGL |
| 摇杆 8/12 分区误触 | 输入错字母 | 死区 + 角度滞回 + UI 实时高亮 |
| CJK 字形多，atlas 爆内存 | 渲染卡顿 | LRU 动态 atlas；ASCII 常驻 |
| evdev 权限 | 读不到设备 | udev 规则 + 用户加入 `input` 组 |
| Switch Home/Capture 读不到 | 元按键失效 | 能力探测 + 降级；`--list-gamepad` 自查 |
| 物理键盘 route 配置错 | 用户以为中文坏了 | 文档明确：终端模式中文由手柄 IME 处理，物理键盘默认直写 PTY；纯 fbdev 下物理键盘只能输入 ASCII |
| 鼠标点击 UI 与终端选区冲突 | 误触 | HitTest 只注册面板/候选/肩键；`click_hit_test=false` 可关 |
| 键盘和手柄同时按同一逻辑键 | 重复触发 | 同帧相同 Action 200ms 窗口去重 |
| 触屏多指误触 | 滚动/选区乱跳 | 单指点击走 HitTest，双指才滚动 |
| 无手柄时虚拟 UI 被误认为已连接 | 困惑 | 面板标题显示 `虚拟手柄（无设备）`；日志提示 |
| xkbcommon 布局不一致 | keysym 翻译错 | 读 `XKB_DEFAULT_LAYOUT`，默认 `us` |
| fcitx5 D-Bus 版本差异 | 接不上 | 只依赖 `InputContext1` 稳定接口；失败回退 `builtin` |
| 系统 IME 候选字段缺失 | 弹窗显示不全 | `candidates()` 为空时只显示 preedit，数字键选字 |
| 打字模式中文不生效 | 误以为坏了 | 启动检测并打印提示；README 明确 |
| Overlay 打开时 PTY 输出仍更新 | 视觉干扰 | `modal()` 时暂停重绘终端区域 |
| libvterm 对某些 TUI 兼容 | `htop`/`tmux` 异常 | 跟踪上游；必要时打补丁 |
| 内置 IME 词库体积 | 仓库变大 | 最小版 < 5MB 随仓库；完整版 release 附件 |
| 侧面板占 34% 屏宽（216×2/1280） | 854×480 上终端只剩 77 列，装不下 80 列 TUI | `virtual=never` 时面板隐藏给全宽；`ui.panel_scale` 允许 0.5–1.0 缩面板；字号自动下探 |
| 候选第 2–7 个没有按键可达 | 只能选第一个候选，拼音输入实际不可用 | 候选态劫持十字键选候选（D4） |
| 外圈 8 个字母靠 latch 进入 | 打字效率低、易误触 | 半径双环或 12 扇区单环（D1） |
| 本机纯 Wayland 无 X11 会话 | `glx` 后端无法在裸 TTY 启动 | 补 `wl`+EGL 原生后端；开发期优先 `egl_gles`/`headless` |
| 组字串（compose）无归属容器 | 渲染位置未定，会与 PTY 输出打架 | 作为浮层锚在 PTY 光标格，不改 ScreenBuffer（D6） |

---

## 13. 开发规范

- C++17，`clang-format`（LLVM 风格，4 空格）。
- 命名：类 `PascalCase`，函数 `camelCase`，成员 `trailing_`，常量 `kPascalCase`。
- 分支：`main` 保护，功能分支 `feat/<milestone>-<topic>`，PR 过 CI（编译 + 单元 + headless 像素回归）。
- 提交：Conventional Commits。
- 接口稳定性：四类后端接口合并进 `main` 后，新增字段用可选虚函数默认实现。
- 文档同步：接口变更必须更新 `IMPLEMENTATION.md` 和 `PLAN.md`。

---

## 14. 后续演进（不在当前范围）

- Overlay 内容：菜单、设置、网络、电源。
- Vulkan / WGPU 后端。
- 触摸屏手势：双指滚动、三指切层。
- 多语言 IME：日文假名、韩文谚文。
- 网络终端：SSH 客户端集成。
- 主题系统：配色、字体、面板布局 TOML。
- Windows/macOS 移植：需实现对应后端。

