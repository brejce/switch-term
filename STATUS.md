# STATUS（v4.2：T9 九键 + 修饰键规则已落地）

设计准则：`docs/DESIGN.md`（v4.2：§3.1 空格双含义 / §3.2 鼠标暂缓 / §3.3 修饰键规则），设计稿 `docs/ui-mockup.html`。

## 已完成（重构）
- **运行期已换成新 UI**：`src/app/app.cpp`（新循环）+ `src/ui/screen.*`（三分区绘制）。
  旧 UI 保留在 `--legacy` 后端，确认无回归后删。
- 新循环：Lua 装配 → `Geometry`（按缩放算）→ `TerminalParser` → PTY → 内置拼音 IME → 渲染。
  `kill -USR1` 重载 ui.lua；窗口尺寸变化自动重排 + 重建字体 + PTY resize。
- `--run "<cmd>"` 现在是 `shell -c`，输出干净（不再被 fish 欢迎信息干扰），用于无头截图核验。
- `src/modules/terminal/terminal_parser.h`：终端解析层接口（feed / cellAt / cursor / history / dirty）。
- `src/modules/terminal/vterm_parser.cpp`：包住已验证的 `term::VTermBridge`；`createParser()` 是唯一入口，
  ghostty 未接时回落到 vterm，接口不变。
- `config/ui.lua`：尺寸 + T9 两层键表（1=.,? … 9=WXYZ）+ 面板标题。
- `config/keymap.lua`：按键→动作名（magic/space/enter/backspace/delete/escape/mod_*/lock_*/mouse_*）。
- `src/config/lua_config.{h,cpp}`：启动装配，把 Lua 转成原生 `Ui`/`KeyMap`；运行期零 Lua。
  验证：`--print-config` 输出 T9 九键 + 12 个键位的单/双击绑定 + `axis STICK_R mouse_move=1`。

## 输入法行为（`--audit-ime` 逐条核对）
- 路由抽到 `src/app/ime_route.cpp`（可单测）。行为：字母组字、空格→首候选、
  **回车→拼音原样上屏**（打出 nihao 本身，不确认候选）、数字 1-7 选词、Tab 翻页、
  Backspace 删字母、Esc 取消组字（字符照发）、标点先上屏候选再发标点、
  组字中 Ctrl+Space 拼音原样落下、英文态直通。
- 已知缺口：**没有词级候选**，`nihao` 的第 2 候选是"你号"（单字交叉组合，非词表）。

## M2 进展（`--audit-t9` 全部通过）
- `src/modules/ime/t9_ime.*`：Grid ↔ Popup 状态机。默认焦点 5(JKL)；摇杆 8 扇区移动焦点
  （越界不循环）；LB 进弹窗 → 再按上屏当前字母且**不退出**（连击）；弹窗内摇杆左右换字母。
- `input::PadState`：evdev 事件 → PadButton/扇区（纯映射，合成事件可测），app 层再按
  `keymap.lua` 的 single/dbl 查动作；同一键 400ms 内二按 = 双击（DPAD 锁修饰）。
- 锁 Shift = 符号层（`t9.setKeys` 换成 `ui.t9_symbol`）。
- 弹窗渲染：焦点格反色 + 单个大字母 + `n/N`，其余格子变暗。
- **右摇杆模拟鼠标按用户要求暂缓**（用鼠标点"摇杆模拟鼠标"没意义）。

## 待做（按 DESIGN 里程碑）
- M0 渲染、M1 终端、M2 输入（左摇杆 3×3 + LB 魔法键 Grid/Popup）**已完成**
  （`ui/screen.cpp` 画三分区，`app/app.cpp` 走 `TerminalParser`，`--audit-t9` 全绿，
  `--ui-preview x.png` 出图正常）。右摇杆 uinput 鼠标仍按用户要求暂缓。
1. 删旧 UI：`src/ui/panels.cpp` / `src/ui/layout_lua.cpp` / 根 `layout.lua` 现在只服务 `--legacy`，
   新架构无回归后连同 `main.cpp` 的 `--legacy` 分支一起删。
2. 词级候选（`nihao` 第 2 候选是「你号」）。
3. TOML 配置（PLAN §6）、evdev `input` 组/udev、Switch 交叉编译（M4）。

---

# 进度快照（2026-10-27 夜）

## 能跑

```fish
cd ~/Project/switch-ime-term
cmake -B build && cmake --build build -j$(nproc)
./build/switch-ime-term --selftest            # pty / 摇杆 / pinyin / ngram 四项自检
./build/switch-ime-term --audit-ime           # 19 项：IM 按键代数 + T9 拼音/英文 + 修饰键
./build/switch-ime-term --audit-t9            # 16 项：T9 状态机 + evdev 合成事件映射
./build/switch-ime-term --probe-mouse         # 虚拟鼠标注册 + 点击
./build/switch-ime-term                       # 开窗口（Wayland 下自动选 sdl 后端）
./build/switch-ime-term --backend headless --out frame.png --run "ls"
```

## 已验证

- PTY + libvterm：bash/fish 提示符、命令输出、nano 全屏（含中文和底部快捷键栏）
- 键盘：修饰键、F1–F12、方向/Home/End/PgUp/Delete、Shift+Tab、Esc 进 shell
- 鼠标/触摸：点左右面板的字母格 = 输入该字
- 内置拼音：`Ctrl+Space` 切中英；候选自绘在终端底部固定 2 行；注音带声调
- n-gram：30 万条 bigram（语料 = m5ui 三本小说），`知` 之后打 `shi` → 首候选「识」
- fbdev：真机 `/dev/fb0`（2560×1600 32bpp）已验证写入，回读像素与预期一致；
  无显示会话时自动选 fbdev（Switch 走同一条路），1:1 居中不缩放
- 按键路径：`sdl`（Wayland 窗口）/ `fbdev`（控制台，stdin raw 模式，Ctrl+Q 退出）

## 数据（都在 data/，可重生成）

| 文件 | 生成命令 |
|---|---|
| `pinyin.idx` 57KB，410 音节 / 20976 汉字 / 含声调 | `FONT_INDEX=2 uv run --with fonttools --with pypinyin tools/make_pinyin_index.py /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc /tmp/x.h data/pinyin.idx` |
| `ngram.bin` 2.4MB，30 万条 | `uv run tools/build_ngram.py data/ngram.bin ~/Project/m5ui/books/*.txt` |

## 没做（按建议顺序）

1. 配置文件 + 日志（M0 收尾；计划 §6 的 TOML 键一个都还没读）
2. 词级候选（当前只有单音节/两音节逐字拼，`nihao` 后面是「你号/你毫」）
3. evdev 手柄（M3，需先 `sudo usermod -aG input brejce` 并重新登录）
4. uinput 打字模式（M7，桌面当通用手柄键盘用）
5. fcitx5 D-Bus 后端（M6）、DRM/EGL 后端、Switch 交叉编译（M8/M9）
   - fbdev 已有：`--backend fbdev`；切到 TTY（Ctrl+Alt+F3）能看到，Ctrl+Q 退出
6. ~~还没 `git init`~~ —— 已建仓（分支 `main`），首个提交含全部源码与 `data/`

## 主题与滚动
- 配色集中在 `src/theme/palette.cpp`（3 套预设），`--theme switch|light|gruvbox`、`--list-themes`；
  UI 与终端 ANSI 0-15 都从这里取色，改一处全变。16-255 色仍走 vterm 自带 256 色表。
- 终端滚动：滚轮 / Shift+PgUp·PgDn 回看，历史 2000 行（`VTermBridge::historyAt`），
  回看时底部提示行显示 `SCROLL n/N`；`sb_popline` 返回 0，往下不还原被吞的行。

## 剪贴板与动作层
- `HitCell.action`（`ui::kActIme/kActCopy/kActPaste`）：面板格子从「只会写 PTY 字节」
  变成能执行动作。鼠标/触摸点击、键盘组合键走同一个 `doAction()`，
  以后 evdev 手柄接上只需要在 ActionMap 里加一行按钮→动作。
- 键盘：`Ctrl+Shift+C` 复制可见区（28 行，含回看位置）、`Ctrl+Shift+V` 粘贴。
  终端里 `Ctrl+C` 必须是 0x03 中断，不能拿来做复制。
- 左面板底部多了两行可点的 `CP 复制 可见区` / `PS 粘贴 剪贴板`；ABXY 的「中」格现在点了也切中英。
- 剪贴板走 `wl-copy` / `wl-paste`（Wayland 本机）。
- 选区：`kActSelLeft/kActSelRight` 动作做扩展（手柄 = Shift+ZL/ZR，键盘 = Shift+←/→），
  起点取终端光标，渲染为反色；`Ctrl+Shift+C`／`CP` 有选区就复制选区，没有才复制整屏。
  Esc 取消选区。`selStep/selClear/selectedText` 在 `ui/terminal_view`。

## 布局用 Lua（改版式不用重编译）
- `layout.lua`：三分区（`panel_w`/`top_bar_h`/`bot_bar_h`）、终端字符格（`cell_w`/`cell_h`）、
  摇杆格尺寸、以及四张键位表（`left_alpha`/`left_symbol`/`right_alpha`/`right_num`，13 项
  = 九宫格 9 + 外圈 4）、DPAD/ABXY 标签。启动时读取，`--layout <path>` 可换文件。
- 派生量（终端列数/行数、摇杆格大小）在 `layout::recompute()` 里算，载入后自动重算；
  PTY 尺寸在建 PTY 之前定下来，所以改面板宽度会真的改变终端列数（220→101 列，260→91 列）。
- 改错语法不会崩：解析失败提示后回落到内置默认布局。

## 手柄（evdev）
- `src/input/evdev_gamepad.*`：`GamepadMapper` 是纯映射（合成事件可自检），
  `EvdevGamepad` 扫 `/dev/input/event*`，只认「有 ABS_X/ABS_Y 或有 BTN_SOUTH」的设备，
  轴范围用 `EVIOCGABS` 读，初值取轴中点。
- 映射：左右摇杆 8 扇区→字母（只在扇区变化时输出）；A=回车 B=退格 X=Delete Y=中英；
  L=切层 R=空格 ZL/ZR=光标左右（锁 Shift 时变扩选）；十字左/右/下=锁 Ctrl/Shift/Alt，上=Tab。
  中英切换 = Ctrl+空格。修饰键两种：**单按 = 一次性 sticky**（只作用于下一个键，
  实体键盘"先按 Ctrl 再按空格"的语义），**400ms 内同方向再按 = 锁定**（一直生效，格子上反色）。
  手柄：十字左按一下 → 再按 R(空格) = 切换中英。键盘/手柄两路输入共用 `routeKeys()`。
- 本机读不了 `/dev/input/event*`（root:input 660，用户不在 input 组）。Switchroot 上需要
  `/etc/udev/rules.d/70-sit-input.rules`：`KERNEL=="event*", SUBSYSTEM=="input", TAG+="uaccess"`
  然后 `sudo udevadm control --reload && sudo udevadm trigger`。

## 缩放与热重载
- 画布不再固定 1280×720：`layout::kScale` = 窗口输出像素 / 设计尺寸，所有尺寸 = `base × kScale`，
  **字体也按同一系数重栅格化**（`FontBook::init` 里 `px * S`）。放大后是原生清晰，不是位图拉伸。
- 窗口尺寸变化（瓦片/全屏）→ `scaleHint()` 重新算 → `applyScale()`：重排布局、重建字体、
  换画布、终端列数变了就 `vt.resize` + `pty.resize`。启动时也按窗口定一次缩放。
- **layout.lua 重载是手动的**（不轮询）：`kill -USR1 $(pgrep -x switch-ime-term)`
  （`-HUP` 也行），或窗口里按 `Ctrl+Shift+R`。重载会重读键位/尺寸 → 重排 → PTY resize。
- 取整导致的副作用：不同缩放下终端行列会差 1~2（1x = 101×28，2x = 101×26），像素取整不可避免。

## 已知坑
- 宽字符（中文）必须「先铺完整行底色、再逐格画字」：以前是边铺边画，宽字符跨两格，
  后一格的底色会把前一个字的右半边盖掉。fzf 选中行、反显（`\033[7m`）必现。
- 本机 niri+SDL 下 `SDL_TEXTINPUT` 根本不触发（`SDL_StartTextInput()` 也没用），
  可打印字符只能由 `keySequence()` 从 keysym 合成；Shift 判定必须用自己按事件顺序
  跟踪的状态（`shiftDown_`），用 `keysym.mod` 会滞后一个字，打出 `_`→`-`、大小写翻转。
- 小字号（<16px）要用 `FT_LOAD_TARGET_LIGHT`，且字号取整数：12.5px 的 ppem 会被取整到 13
  但缩放仍按 12.5 算，笔画粗细不均、横画断裂。

- 字体：`Maple Mono NF CN` 是主字体，缺字回退 `Noto Sans CJK SC`；`⇄`/`＋` 靠回退
- `data/` 是相对路径，换目录启动会警告（不崩）
- SDL 窗口被合成器缩放时（1280×720 → 1024×576），点击坐标已按比例换算
- fish 启动会 `tcsetattr(TCSAFLUSH)` 丢掉已写入的输入，所以 `--run` 要等它先吐字节
