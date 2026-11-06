# STATUS · 交接（现在能跑什么 / 已知 bug / 下一步）

> 最后更新：2026-11-06。定位：**一眼看清现在在哪、下一步做什么**。历史过程已删。
> 模块边界与配置 → `docs/ARCH.md`；机器实测事实 → `docs/DEVICE.md`；上机流程 → `docs/SWITCHROOT.md`；
> 布局权威规格 → `docs/LAYOUT-v2.md`；按键映射 → `config/keymap.lua`（`--print-config` 可打印）；
> 字体 → `docs/FONTS.md`。

环境：**本机就是目标机**——Switch 2017 / switchroot Ubuntu 24.04.4 (noble) / kernel 4.9.140-l4t /
arm64 / 3.9Gi，自然会话已关（`multi-user.target`，无 KDE/Xorg），桌面未卸载；`/dev/dri` 不存在。
实测细节全在 `docs/DEVICE.md`。

## 1. 当前状态（五句话）

- **能跑**：双摇杆引力选键 + 内置拼音 IME（空格上屏选中候选 / R·ZR 选词）+ 多 tab + 三分区绘制 + 配置热重载。
- **开机就是它，不启 Xorg**：`switch-ime-term.service`（`multi-user.target` 默认 + `/dev/tty1` + 背光强制
  255 + 等 Joy-Con 出现再起）。重启即进界面，`systemctl restart switch-ime-term` 换版本。
- **布局 v2 已接管主循环**：`config/ui.lua` 的 40 键 × 2 层，摇杆物理场决定吸附，LB 上屏。
- **旧九键面板已整体删除**（2026-11）：状态机/弹窗/九键配置全删；`--legacy` 的输入层也已并到
  `keymap.lua` + `routeKey`（2026-11-04），C++ 里不再有第二张键位表。
- **这一轮（2026-11-06）**：脏矩形局部上屏（帧拷贝 3.69MB → 英文输出 78–156KB / 中文组字 234KB /
  摇杆动画 932KB，两块面板）、相机钉准心（不夹取）、指数径向遮罩、面板底部提示表、
  手柄设备过滤（跳过 IMU/Serial 辅助节点）。

## 2. 现在能跑什么

### 2.1 构建与自检

```fish
cd ~/Project/switch-ime-term
cmake -B build && cmake --build build -j$(nproc)

./build/switch-ime-term --selftest            # pty / 摇杆 / pinyin / ngram / tabs / osc / fbdev打包
./build/switch-ime-term --audit-ime           # IM 按键代数 + 修饰键
./build/switch-ime-term --audit-input         # 手柄按键/摇杆映射 + keymap 装配
./build/switch-ime-term --probe-mouse         # 虚拟鼠标注册 + 点击
./build/switch-ime-term --print-config        # 装配结果（键位绑定/摇杆轴）
./build/switch-ime-term --theme switch --list-themes
./build/switch-ime-term                       # 开窗口（有 DISPLAY 就选 sdl 后端）
./build/switch-ime-term --backend headless --out frame.png --run "ls"
./build/switch-ime-term --preview-keys --preview-layer 1 --backend fbdev   # v2 键帽直接上屏
```

Switch 上第一次构建要装的包（漏装 `libsdl2-dev` 的表现是 `backend 'sdl' not found` 直接退出，不回落）：

```
sudo apt install cmake build-essential pkg-config libfreetype-dev libfontconfig1-dev \
                 libpng-dev libvterm-dev liblua5.4-dev libsdl2-dev
```

### 2.2 能力清单

| 能力 | 状态 | 备注 |
|---|---|---|
| PTY + libvterm | 能跑 | bash/fish 提示符、命令输出、nano 全屏（含中文与底部快捷键栏） |
| 键盘输入 | 能跑 | 修饰键、F1–F12、方向/Home/End/PgUp/Delete、Shift+Tab、Esc |
| 多 tab | 能跑（未在真 shell 验过） | `term 名字` / `quit` / `exit`，点 tab 条切换，点 `+` 新建，上限 16 |
| 内置拼音 | 能跑 | `Ctrl+Space` 切中英（手柄 = DPAD_DOWN + ZL）；**空格上屏选中候选**、R/ZR 在候选条里移动；候选自绘在终端底部固定 2 行；注音带声调 |
| n-gram 重排 | 能跑 | 30 万条 bigram；`知` 之后打 `shi` → 首候选「识」 |
| 鼠标 demo | 能跑 | 左键选格、右键 = 魔法键、悬停即选中；只改 UI 状态，不写 PTY |
| 光标键 | 能跑 | R=左 / ZR=右；带 Shift（单击一次 / 双击锁定）变上/下；右摇杆移指针**不做** |
| 输出口 | 能跑 | `sink=pty`（默认）/ `uinput`（打给前台程序，打不开自动回落 pty） |
| 配置热重载 | 能跑 | `kill -USR1`（`-HUP` 同）或窗口内 `Ctrl+Shift+R`，全量重读重建 |
| 缩放 | 能跑 | 窗口尺寸变化自动重排 + 重建字体 + PTY resize |
| 主题 | 能跑 | `src/theme/palette.cpp` 3 套（switch/light/gruvbox），UI 与 ANSI 0-15 同一来源 |
| fbdev 后端 | 能跑（本机跑通） | 软件旋转 + 居中贴图；键盘走 stdin raw，Ctrl+Q 退出 |
| v2 分体键盘 | 能跑（接管主循环） | 数据在 `config/ui.lua`；`--preview-keys` 仍可单画键帽 |
| 剪贴板 / 选区 / 滚轮回看 | 只有 `--legacy` | 新路径空实现，见 §3.5 |
| fcitx5 | 进不来 | `display_sdl.cpp:99` 无条件 `SDL_StopTextInput()` |
| 词级候选 | 缺 | `nihao` 第 2 候选是「你号」（单字交叉组合，非词表） |
| DRM/EGL、交叉编译、Overlay | 零代码 | 本机也没有 `/dev/dri`，见 `docs/DEVICE.md` |

### 2.3 数据（`data/`，可重生成）

| 文件 | 内容 | 生成命令 |
|---|---|---|
| `pinyin.idx` 57KB | 410 音节 / 20976 汉字 / 含声调 | `FONT_INDEX=2 uv run --with fonttools --with pypinyin tools/make_pinyin_index.py /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc /tmp/x.h data/pinyin.idx` |
| `ngram.bin` 2.4MB | 30 万条 bigram（语料 = m5ui 三本小说） | `uv run tools/build_ngram.py data/ngram.bin ~/Project/m5ui/books/*.txt` |

## 3. 已知 bug

### 3.1 修饰键四处缺口（2026-11-03 真机核对）

0. ~~十字键键码读错~~ —— **已修**（commit `7bf998b`）：`evdev_gamepad.cpp` 两条路径都补了
   `BTN_DPAD_*`（legacy `:73-83`、`PadState` `:148-151`）。**真机复验还没做**。
1. **uinput 静默丢控制码**：`src/input/uinput_kbd.cpp:23-66` 的 `asciiToKey()` 不认 `0x01~0x1f`，
   `tap()` 首行 `!keycode` 直接返回（`:120`）→ **桌面模式（sink=uinput）下 Ctrl+A/C/V/D 一个键都不发**。
   PTY 模式本来就对。修法约 15 行（`KeyStroke` 加 `ctrl`，`tap()` 夹 `KEY_LEFTCTRL`）。
2. ~~sticky 从不清零~~ —— **已修**（`7bdc6f7` 前一笔）：`doAction` 之后 `clearSticky()`，键盘路径每键清；
   锁定的修饰键现在还会在左面板点亮（`st.modCtrl/modShift/modAlt` 之前从没被赋值）。
3. **中文态 Ctrl 被吞**：`src/app/ime_route.cpp:11` 让中文态 Ctrl+c 走到「字母起组字」（`:109`），
   Ctrl 静默失效、底栏却还亮着 CTRL。桌面模式不受影响。

### 3.2 手柄

- 十字键键码**已在真机复验**（`--audit-input` 全 OK，`--list-gamepad` 认出 `Combined Joy-Cons` =
  `event8`）。注意服务启动时 Joy-Con 还在串口握手，所以 unit 里 `ExecStartPre` 会等设备出现再起。
- 「手柄不可用」的真相是**左滑轨只连上了一边**（不是驱动问题）：必须有 `event6 + event7 + event8`，
  排查命令见 `docs/DEVICE.md` §1.3。
- `config/keymap.lua:7` 的 `LT` 实际映射 `BTN_TL2` = 物理 **ZL**（命名错位，不影响功能）。
- 设备权限已不是问题：`event8` 有 `user:brejce:rw-` ACL，不需要 udev 规则。
- **分层里的空槽**：NUM 层左右各只剩 5~8 颗真键，指针仍可能停在这些键围成的矩形包围盒里的空槽上——
  只是不再被吸附、也不再被相机对准（物理层朝最近真键收 25%/帧，见 `docs/PHYSICS.md` §2.7）。

### 3.3 显示

- **fbdev 仍是单缓冲（真双缓冲未做）**：carveout 4MB 只够一帧（3.69MB = 3,686,400 B），硬件扫同一块
  内存 → 撕裂理论上还在。现在做的是**软件双缓冲 + 脏矩形**（`src/platform/damage.h` + `SoftCanvas`
  base clip + `display_fbdev.cpp` 的 `blit(damage)`）：一帧只搬几十~几百 KB，撕裂窗口 <1ms。
  `tegra_dc_ext` + nvmap 的真双缓冲/flip/vblank **仍未实施**（必须停 X，是本机唯一能验证真双缓冲的路径），
  方案与实测数字见 `docs/Q-double-buffer-dirty-rect.md`。
- **`/dev/dri` 不存在、没有 KMS**：Wayland 不可能，只能 X11 或 fbdev 裸机。
- **有合成器时不要对 `/dev/fb0` 做 `FBIOPUT_VSCREENINFO`** —— 2026-11-03 15:46 硬复位，
  详见 `docs/DEVICE.md` §5。
- ~~字体回退~~ —— **已修**：`MapleMono-NF-CN-{Regular,Bold}.ttf` 已装进 `~/.local/share/fonts`，
  `FontManager::get()` 在「请求族名 ≠ 实际族名」时会打 `fontconfig: 要 'X' 实际拿到 'Y'`。
  背景与装法 → `docs/FONTS.md`、`docs/DEVICE.md` §12。
- **背光会被压到 23/255（9%）**：Xorg 退出、或写 `/sys/class/graphics/fb0/blank` 都会留下这个值，
  屏看起来是纯黑。unit 的 `ExecStartPre` 每次启动强制 255，systemd-backlight 存的值也改成 255 了。
- `--backend fbdev` 下 `fbcon=map:9` —— 切 TTY 没有文字，程序崩了只能靠 SSH。

### 3.4 渲染坑

- **顶栏/底栏的内容签名是手写的**：脏矩形只在 `ui::barSignature` 变了时才重画这两条，往
  `drawTopBar` / `drawBottomBar` 里加新字段必须同时加进签名（两个函数旁边有注释），否则那一格永不刷新。
- 宽字符必须先铺完整行底色再逐格画字，否则后一格底色会盖掉前一字右半边（fzf 选中行、反显必现）。
- 小字号（<16px）要 `FT_LOAD_TARGET_LIGHT` 且字号取整，否则笔画粗细不均、横画断裂。
- fish 启动会 `tcsetattr(TCSAFLUSH)` 丢掉已写入的输入，`--run` 要等它先吐字节。

### 3.5 剪贴板 / fcitx5 / 构建

- 剪贴板：新路径 `app.cpp:423-424` 是空实现，唯一实现是 legacy 的 `popen("wl-copy"/"wl-paste")`，
  而本机这四个工具全缺 → X11 下 `Ctrl+Shift+C/V` 完全静默。
- fcitx5：`display_sdl.cpp:99` 无条件 `SDL_StopTextInput()`，X11 下不挂 XIM → 永远进不来；
  `ime.enabled=false` 只做到「程序内不抢」。`/dev/uinput` 是 `0600 root:root`（不在 input 组），
  所以 `sink=uinput` 也开不起来，会静默回落 pty。
- SDL 没有软件渲染回落（`display_sdl.cpp:87`），tegra GL 开窗失败即退出。

### 3.6 多 tab 的未验项

- 只用假 shell 验过 OSC 路径；**真实 bash/fish 里敲 `term`/`quit` 能不能建/关 tab 还没验**。
- tab 组合键没分配（`Ctrl+N` = `0x0e` 撞 readline「历史下一条」，`Ctrl+Shift+N` 也被折成 `0x0e`）。

### 3.7 文档

- `docs/*` 之外还有 `IMPLEMENTATION.md` 未对齐（仍写旧布局/旧实现），以本文件 + `docs/LAYOUT-v2.md`
  + `docs/PHYSICS.md` 为准。

## 4. 下一步（按顺序）

### A. 真机上机验证

1. 装 §2.1 那批依赖 → `cmake -B build` 跑通。
2. `--list-gamepad` 确认 joycond 设备被枚举、十字键键码修好后动作能出（`jc_rail_disable=0` 默认已开）。
3. **验多 tab 的真交互**：一个 tab 里敲 `command -v term`（应为 `/tmp/sit-bin-<pid>/term`，被 shell
   缓存就 `hash -r`）→ `term 前端` 看是否多一个 tab → `quit` 看是否关 → `exit` 看是否原地重启。
4. 同一轮顺手验：Ctrl（单击一次）→ 键区选 `c`，shell 里应是中断（`0x03`）；
   桌面模式（`sink=uinput`）下**预期失效**（§3.1 第 1 条）。
5. **SSH 通了之后**才动桌面：`set-default multi-user.target` + 停 sddm（先跑 `SWITCHROOT.md` §0.3 的模拟检查）。
   镜像 `~/Downloads/switchroot/theofficialgman-kubuntu-noble-5.1.2-2026-05-13.7z`
   （SHA1 `66e6d4990df4d671e64615b7ec6276299fdebc21`），流程见 `docs/SWITCHROOT.md`；
   没路由器时走 `sudo ./tools/pc-host-switch.sh` 网线直连。

### B. 摇杆统一指针（设计已定，未实现）

结论：**hover 的本质是「指针落在哪个字符键上」**，鼠标/触摸/摇杆共用同一套命中逻辑。

- 唯一指针 = `bd->mouseX/mouseY`（-1 = 虚无），三个输入源都往这里写。
- 触摸：SDL 用 `SDL_FINGER*`（约 20 行）；fbdev 用 `/dev/input/event*` 的 `ABS_MT_POSITION_X/Y`。
- 可调参数（未加）：`engine.input.stick = { deadzone, hysteresis_deg, repeat_ms, repeat_delay_ms }`；
  纯映射层保持无时间，重复逻辑放 app 层。

### C. 布局 v2 接管（已完成）

摇杆物理场选键 → LB 上屏 → L3 层循环 → DPAD 修饰都已接上；数据与静态键帽说明看 `docs/LAYOUT-v2.md`，
手感旋钮在 `config/engine.lua` 的 `engine.physics`（现值表见 `docs/PHYSICS.md` §4）。

### D. 排队项

1. **中文词表 + 组合搜索**：`data/` 只有单字表 + bigram，没有词表，`nihao` 第 2 候选是「你号」。
2. **删 legacy UI**：`src/ui/panels.*` / `layout_lua.*` / 根 `layout.lua` / `main.cpp` 的 `--legacy`。
   删前先把 legacy 独有能力（点击 HitTest、选区、剪贴板、滚轮回看、SDL 写 `sticky`）搬进新路径。
3. 修 §3.1 剩下的两条（uinput 控制码 / 中文态 Ctrl）——sticky 那条已修。
4. `engine.log` / `engine.ime.ngram` 仍只解析不生效（死配置清单见 `docs/ARCH.md` §6.7）。
5. tab 组合键（先把 `ime_route.cpp` 的 ctrl/shift 判断顺序倒过来，或只用 shell 打不出来的组合）。

### D2. UI 收尾（设计已定，未实现）

规格全在 `UI_DESIGN.md`（设计稿：`design/v3-terminal.html`），这里只排优先级：

| 编号 | 任务 | 现状 | 规格 |
|---|---|---|---|
| U1 | 历史回看：右侧 3px 指示条 + 触摸上下滑 + `SCROLL n/total` | `engine.terminal.scrollback` 有数据，但活路径（`app.cpp` 的 `drawGrid`）不画滚动、也不收触摸 | `UI_DESIGN.md` §七 |
| U2 | 候选跟光标：像桌面输入法那样贴着正在打的那行 | 高度预留已做了（`ui.ime_rows=2` 从 PTY 行数里扣），但候选固定钉在终端区底部 | `UI_DESIGN.md` §六 |
| U3 | 底栏 `LANG` + `IME` 合并成一个字段 | `screen.cpp:drawBottomBar` 还在同时打两个（`LANG ZH` + `IME PINYIN`） | `UI_DESIGN.md` §八 |
| U4 | 默认 cwd = 登录用户 home，提示符缩写 `~` | 现在是进程启动时的 cwd，`src/` 里没有任何 HOME/getpwuid 处理 | `UI_DESIGN.md` §九 |

U4 最便宜（开工就做），U3 次之（一个函数），U2 与 U1 都在终端区渲染层，
建议同一次改（都要动 `drawGrid` 的行映射）。

### E. 已定路线（不用再讨论）

- 图形软件塞进 tab：**只有路线 1（库级 offscreen → RGBA → `drawImage`）现实**；cage/weston 换 VT、
  wlroots 当合成器都要 DRM master，本机没有（`docs/DEVICE.md` §7）。代价：给 `render::Renderer` 加一个
  `drawImage`（约 10 行）+ 修 BGRA 假实现。
- 显示层升级顺序：fbdev + 脏矩形先跑通 → 以后可换 `tegra_dc_ext` + nvmap 真双缓冲（同屏，上层不动）。
- 桌面内存：不卸载 KDE，只不启动（裸 X + 我们），省 1.1–1.3GB（`docs/DEVICE.md` §8）。
- 右摇杆不模拟鼠标；`uinput` 打不出中文（要中文走 fcitx5）。

## 5. 里程碑（一句话）

M0 骨架 / M1 终端 / M2 中文渲染 / M3 手柄 / M4 内置 IME / M5 n-gram **已完成**；
M3.5 只差指针源接线（§4.B）；M7 打字模式只有 uinput 骨架；
M6 fcitx5 / M8 DRM·EGL / M9 交叉编译 / M10 Overlay **零代码**。
