# DEVICE · 目标机实测事实

> 最后更新：2026-11-03。定位：这台机器上**测出来的**东西——能做什么、不能做什么、代价多少。
> 上机流程看 `SWITCHROOT.md`，代码现状看 `../STATUS.md`，模块边界看 `ARCH.md`。
> 来源：合并自 2026-11-03 的六份实测记录（X11+KDE 环境审计、fbdev 探针、双缓冲/脏矩形、
> 保 Xorg 去 KDE、mini-DRM 边界、托盘程序），原文已删，逐条内容都在本文，git 历史可查原文。

## 0. 机器与环境

| 项 | 值 |
|---|---|
| 机器 | Switch 2017 |
| 系统 | switchroot Ubuntu 24.04.4 (noble) |
| 内核 | 4.9.140-l4t（`theofficialgman@buildbot` 编，#139，2026-05-04） |
| 架构 / 内存 | arm64 / 3.9 GiB |
| 会话 | X11（`XDG_SESSION_TYPE=x11`、`kwin_x11` + KDE Plasma），`xrandr` 1280x720 |
| 显卡 | NVIDIA tegra 32.3.1，GL 4.6 direct rendering，**无 `/dev/dri`** |
| 桌面未 purge | plasmashell 360MB + kded5 274MB + fcitx5 110MB |
| 手柄设备 | 只有 joycond 合成设备 `event8` = `Nintendo Switch Combined Joy-Cons`（Bus 0006, 057e:2008） |

**本机就是目标机**。以前「Wayland 开发机 + 无桌面真机」的两套假设全部作废。

### 0.1 与旧文档不符的事实（已更正）

| 旧记录 | 实测 |
|---|---|
| `/dev/fb0` 2560×1600 32bpp | **720×1280 32bpp**（那条是别的机器测的） |
| 用户不在 input 组，需加 `70-sit-input.rules` | `event8` 已有 `user:brejce:rw-` 的 ACL（logind uaccess），**不需要** |
| Wayland 下自动选 sdl | 现在是 X11，自动判定仍命中 sdl，但 SDL 开发包未装 |
| 剪贴板走 wl-clipboard | X11，工具全缺，且新路径没实现（§9.1） |
| 「目标机没有桌面环境，我们独占 framebuffer」 | 现在有 KDE + OpenGL，**但无 `/dev/dri`**（§3） |
| 十字键 `kPadUp/Down/Left/Right` 永不产生 | 已修，commit `7bf998b`（§1.2） |

## 1. 手柄（joycond / 键码）

### 1.1 设备与权限

- user **不在** input 组，但 `/dev/input/event8` 有 `user:brejce:rw-` 的 ACL（logind uaccess），能直接读，
  **不需要** `70-sit-input.rules`。
- 设备筛选：`src/input/evdev_gamepad.cpp:184` 只要求 `(ABS_X && ABS_Y) || BTN_SOUTH`，
  `event8` 会被正确打开，不会误收 touchscreen / gpio-keys。
- 触屏由 `input::EvdevTouch` 统一读（`/dev/input/event*` 里报 `ABS_MT_POSITION_X/Y` 的那台），
  拖动翻页/轻点在 fbdev 和 X(SDL) 下是同一份实现；X 那边仍要忽略触摸屏设备，否则事件被 X 抢走。
- 写虚拟键鼠仍要权限：`/dev/uinput` 是 `0600 root:root`，brejce 不在 input 组 →
  `src/app/app.cpp:176-180` 打不开就静默回落 pty（打一行警告）。

### 1.2 十字键键码（阻断过，已修）

**原症状**：`src/input/evdev_gamepad.cpp` 的两条路径（legacy `GamepadMapper` / 新 `PadState`）
只读 `ABS_HAT0X/ABS_HAT0Y`，而本机 `event8` 的 ABS 只有 `X Y RX RY`（位 0,1,3,4），**没有任何 HAT 轴**；
十字键走按键 `544-547 = BTN_DPAD_UP/DOWN/LEFT/RIGHT`。

- 后果：`kPadUp/Down/Left/Right` 永不产生 → `tab`、`mod_ctrl/shift/alt`、三个 `lock_*` 全废，
  连带 `Ctrl+Space` 切中英（唯一入口）也按不出来。
- **修法**（≈6 行，已落）：`EV_KEY` 分支加 4 个 `case BTN_DPAD_*` → `kPadUp/Down/Left/Right`，
  HAT 分支保留以兼容原生 hid-nintendo。
- 现状：legacy 路径 `:73-83`、新路径 `:148-151` 都已认 `BTN_DPAD_*`；**真机复验尚未做**。

### 1.2b 全部按键的实测清单（2026-11-07，`evdump` 逐键取证）

`/dev/input/event*` 能力位图 + 实际按键，逐个确认（`Combined Joy-Cons` 是并集）：

| 物理键 | evdev 码 | 名 | 我们的映射 |
|---|---|---|---|
| A / B / X / Y | 305 / 304 / 307 / 308 | `BTN_EAST/SOUTH/NORTH/WEST` | kPadA/B/X/Y ✓ |
| L / ZL / R / ZR | 310 / 312 / 311 / 313 | `BTN_TL/TL2/TR/TR2` | kPadLB/LT/RB/RT ✓ |
| DPAD 四向 | 544–547 | `BTN_DPAD_UP/DOWN/LEFT/RIGHT` | kPadUp/Down/Left/Right ✓ |
| 左摇杆按下 | 317 | `BTN_THUMBL` | kPadL3（切层）✓ |
| 右摇杆按下 | **318** | `BTN_THUMBR` | **kPadR3 已接上、暂未绑功能**（旧注释说"设备上不存在"是错的） |
| `+` | 315 | `BTN_START` | kPadPlus → `onboard` ✓ |
| `−` | 314 | `BTN_SELECT` | **没映射**（switch 里没有这个 case，按了无反应） |
| Home | 316 | `BTN_MODE` | **没映射** |
| Capture（截图） | 309 | `BTN_Z` | **没映射** |
| 音量 + / − | 115 / 114 | `KEY_VOLUMEUP/DOWN` | 在**独立设备 `gpio-keys`** 上：已接入（`input::EvdevSysKeys` 另开只读 fd），`volup`/`voldown`，暂未绑功能 |
| 电源 | 142 | `KEY_SLEEP` | 在**独立设备 `max77620_onoff`** 上（不是 `KEY_POWER`，logind 已 `HandleSuspendKey=ignore`）：已接入，`power`，暂未绑功能 |
| SL / SR（滑轨） | — | 无独立键码 | 所有 Joy-Con 设备的能力位图里都没有 `BTN_TRIGGER_HAPPY`；`Left/Right Joy-Con` 把滑轨键揉进 `BTN_TL/BTN_TR`。实测按下**零事件**，别当额外按键用 |

`−` / Home / Capture 要启用时各是 3 处改动：`input::PadButton` + `cfg::InputKey` + `kKeys[]`
（顺序必须两边一致，都追加在末尾，否则会顶掉现有索引），然后 `config/keymap.lua` 里 `bind` 一行。

### 1.3 开机「手柄不可用」的真相（两次 boot 对比）

**15:47 那次 —— 全好**：两只 Joy-Con 开机 10 秒内握手成功：

```
/dev/input/event6  Nintendo Switch Left Joy-Con Serial     (js0)
/dev/input/event7  Nintendo Switch Right Joy-Con Serial    (js1)
/dev/input/event8  Nintendo Switch Combined Joy-Cons        (js2)  ← joycond 合成，程序读这个
```

**14:23 那次 —— 只手柄**：启动时**只有右手柄**（`serial0-0`）握手成功，左手柄直到 **15:02:01**
（开机 38 分钟后）才连上；之后 15:09、15:30 又反复重新握手。→ 不是驱动/joycond 的问题，是**左滑轨只连上了一边**。

排查顺序（下次先看这个）：

1. `for d in /dev/input/event*; do echo "$d $(cat /sys/class/input/$(basename $d)/device/name)"; done`
   必须有 **event6 + event7 + event8 三个**；只剩 event8 说明 joycond 活着但某只手柄没上来
2. `dmesg | grep joycon | tail -20` —— 看 `Detected left/right joy-con` 和 `completed handshake` 两边是否都有
3. 两边都不报 → 滑轨接触（推紧 / 擦金手指），与软件无关；`Received pkt is malformed` 大量刷屏也是同一个接触问题

### 1.4 keymap 在真机的可用度

12 条绑定：键码修好后应全通（**待上机复验**）。修之前只有 8 条可用
（LB=magic、LT=space、A/B/X/Y、RB/RT=鼠标左右；不可用 = DPAD_UP=tab、DPAD_LEFT=ctrl、DPAD_RIGHT=shift、DPAD_DOWN=alt、3 个 `lock_*`）。

- `config/keymap.lua:7` 的 `LT` 实际映射 `BTN_TL2` = 物理 **ZL**（命名错位，不影响功能）
- `config/keymap.lua:27` 的 `STICK_R mouse_move` 只在 `src/config/lua_config.cpp:86` 解析成 bool，
  **无消费点** → 右摇杆移鼠标未实现（用户已决定不做）

## 2. 构建链（曾在真机上完全缺失）

`cmake -B build` 第一条就 ENOENT。实测缺：cmake、ninja、`freetype2/fontconfig/libpng/sdl2/vterm/lua5.4`
六个 `.pc` 全无，`/usr/include/{vterm.h,lua5.4,freetype2,libpng16}` 全无，libvterm 连运行时都没有。

```
sudo apt install cmake build-essential pkg-config libfreetype-dev libfontconfig1-dev \
                 libpng-dev libvterm-dev liblua5.4-dev libsdl2-dev
```

ports.ubuntu.com 上都有：cmake 3.28.3 / libvterm-dev 0.3.3 / liblua5.4-dev 5.4.6 / libsdl2-dev 2.30。磁盘 68G 空闲。

**联动陷阱**：SDL2 在 `CMakeLists.txt:13` 是可选的，但 `src/main.cpp:861` 只要 `DISPLAY`/`WAYLAND_DISPLAY`
存在就选 `"sdl"`，`:993` 遇未注册后端**直接 return 1，不回落 fbdev/headless** → 漏装 `libsdl2-dev` 的表现是
`backend 'sdl' not found`，极易误判。

## 3. 显示栈：这台机的真实路径

### 3.1 现代 Linux 标准栈（本机不适用）

```
app 渲染 → buffer (shm / GEM dmabuf)
   ↓ 提交 buffer（不是画到屏幕）
合成器合成所有窗口 → 一个 buffer
   ↓ atomic commit
内核 DRM/KMS 驱动写显示控制器寄存器 → vblank 时 page flip
   ↓
显示控制器 → DSI/HDMI/eDP → 面板
```

核心：应用从不「往屏幕上画」，只把 buffer 交给合成器；flip 是硬件做的，所以不撕裂。

### 3.2 本机实际：无 DRM

- `/dev/dri` **不存在**；`lsmod` 无任何 drm 模块
- 内核只有 `gpu/drm/tegra_udrm/tegra-udrm.ko`（NVIDIA 用户态驱动辅助，**不是 KMS 驱动**）；
  `modprobe tegra-drm` → not found
- 显示控制器以**字符设备**暴露：`/dev/tegra_dc_0`、`/dev/tegra_dc_1`、`/dev/tegra_dc_ctrl`
- 渲染走 `/dev/nvhost-gpu`、`/dev/nvhost-ctrl-gpu`（nvgpu）
- 面板：`tegradc.0: disp0 connected to head0->/host1x/dsi`，DSI 直连

| | X11（当前） | fbdev（裸 TTY） |
|---|---|---|
| 像素产生 | `SoftCanvas` → `SDL_UpdateTexture` | `mmap(/dev/fb0)` 直接 memcpy |
| 传递 | X11 协议 → Xorg + nvidia_drv 32.3.1 | 内核 `tegra_fb` |
| 合成 | kwin_x11 用 GL 合成（有 `Present` 扩展、NV-GLX） | 无 |
| 上屏 | nvidia 驱动 ioctl `/dev/tegra_dc_*` + 前后缓冲 flip（**有 vsync，不撕裂**） | 硬件持续扫描同一块内存（**撕裂**） |
| GPU | 可用（Tegra X1, GL 4.6 direct rendering） | 不可用 |

### 3.3 Wayland 不可能

Wayland 合成器需要 DRM master + GBM/atomic。本机连 `/dev/dri` 都没有，
`weston/sway/cage/wayfire/labwc/river` 全未安装。「离开 X11」的唯一出路是 fbdev 裸机，**不是换 Wayland**。

### 3.4 SDL 渲染器没有软件回落（高风险）

`src/platform/display_sdl.cpp:87` 只请求 `SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC`，
失败即 `return false` → `src/main.cpp:997` 打 "backend init failed" 退出。tegra 专有驱动下必须先实测；
失败就给 `SDL_RENDERER_SOFTWARE` 二次尝试（3 行）。

### 3.5 字体

`src/ui/theme.h:35` 写死 `"Maple Mono NF CN"`，本机 **fc-list 0 命中**，落到 `Noto Sans CJK SC`（非等宽）。
不致命（代码里用到的非 ASCII 全是全角/CJK/箭头 U+2192，Noto 都覆盖），但 1:2 网格与 Nerd 图标没了。
要么装字体，要么把等宽回退串写进 `theme.h`。

## 4. fbdev 硬约束（`tools/fbtest.cpp` 实测）

### 4.1 硬件事实

```
fix.id     tegra_fb
smem_len   4194304  (4.00 MB)
line_len   2880
xres/yres  720x1280
virtual    720x1280
bpp        32
rotate     0
rgba       16/8 8/8 0/8 24/8      → 像素是 0x00RRGGBB（红在 bit16）
一帧字节   3686400  剩余 0.48 MB
双缓冲     不可能（yres_virtual == yres）
```

`/sys/class/graphics/fb0/modes` = `U:720x1280p-60`。内核 cmdline `tegra_fbmem=0x400000@0xf5a00000` = carveout 共 **4 MB**；
720×1280×4 = 3.51 MB，`smem_len` = 4194304 → 剩不到 0.5 MB。`virtual_size` 等于 mode，`FBIOPAN_DISPLAY` 无意义。
**结论**：撕裂只能靠「少画」来压。

### 4.2 四条硬约束

1. **竖屏且不旋转**：Switch LCD 物理 720×1280，`rotate` = **0**。Xorg 的 nvidia 驱动替你转了 90°
   （`xrandr` 显示 `DSI-0 1280x720 normal`），裸机没有。
2. **画布对不上（已修）**：`src/ui/layout.h:11` 设计尺寸 1280×720，`src/platform/display_fbdev.cpp` 早期是
   「1:1 居中，不转不缩」→ `ox_ = (720-1280)/2 = -280`，一帧都画不对。现在走**软件旋转**
   （`engine.display.rotate`，本机 `config/engine.lua` 设 270；实现 `display_fbdev.cpp:103-111`，
   `setRotation` 在 `:145`，字段 `:182`）。
3. **单缓冲，双缓冲物理上不可能**：见 §4.1。
4. **没有控制台兜底**：内核 cmdline `fbcon=map:9`，`/sys/class/vtconsole/*/name` = `dummy device` →
   切 TTY 看不到 shell 提示符，程序崩了屏幕上什么都没有。**必须先配好 SSH 再 purge 桌面。**

### 4.3 写入通路实测

mmap 后画四角 24×24 白块 + 正中红十字，再读回校验：

```
左上角  (0,0)        want=ffffff got=ffffff OK
右上角  (719,0)      want=ffffff got=ffffff OK
左下角  (0,1279)     want=ffffff got=ffffff OK
右下角  (719,1279)   want=ffffff got=ffffff OK
正中    (360,640)    want=ff0000 got=ff0000 OK
角外底色 (690,100)   want=202020 got=202020 OK
```

坐标系是 `720 宽 × 1280 高`（竖屏），stride 2880 必须按 `line_length` 算，不能假设 `xres*4`
（本机恰好相等，但没有保证）。

### 4.4 旋转测试：驱动不帮你转

```
rotate=1 写入成功；回读 rotate=1  xres/yres=720x1280  virtual=720x1280
已还原 rotate=0
```

`FBIOPUT_VSCREENINFO` 接受 `rotate=1`、不报错，但 `xres/yres/virtual` 一个都没变（真实的 fb 旋转驱动会把宽高
交换成 `1280x720`）→ `tegra_fb` **只把这个字段存下来，并不做变换**，**必须软件旋转**。

未验证：X 正占用显示控制器，无法确认「设了 rotate 之后屏幕上到底有没有变化」。要真验必须切到没有合成器的 VT，
但 `fbcon=map:9` 让 TTY 没有文字，只能画个上下不对称的图案靠人眼确认。

## 5. 事故记录：`FBIOPUT_VSCREENINFO` 导致硬复位（2026-11-03 15:46）

**现象**：机器自己重启了。

**证据**：

- `/var/log/kern.log` 上次 boot 最后一行 15:46:20（`joycon_serdev: Received pkt is malformed`），
  下一行直接是 15:47:02 `Booting Linux on physical CPU 0x0`
- **没有** shutdown / reboot 序列，没有 panic/Oops/thermal trip 记录 → 是**硬复位**，不是有序关机
- 电池可排除：15:41 SOC 65%、15:45 SOC 63%；重启后 `CAPACITY` = 51% → 现在是 56% Charging（低电会在 3–5%）
- 温度可排除：事后台风区 36–55°C，无 thermal 阈值触碰记录

**时间相关性（头号嫌疑）**：15:40:47 跑了 `fbtest --rotate 1 --restore`，kern.log 里留下：

```
tegradc tegradc.0: [tegra-fb] check_var from 'fbtest': xres=720 yres=1280 bpp=32
 tegradc tegradc.0: [tegra-fb] set_par from 'fbtest': xres=720 yres=1280 bpp=32   ×2
```

`set_par` = **在 X 正占用显示控制器（nvidia 驱动）时重配了 tegradc**。6 分钟后机器硬复位。
相关不等于因果，但这是当天唯一对显示控制器动手的操作。

> **规矩：有合成器（X11/KWin）占用显示时，不要对 `/dev/fb0` 做 `FBIOPUT_VSCREENINFO`。**
> 读 `FBIOGET_*`、mmap 写像素都安全（已验证），改 var **不安全**。

要验证旋转的真实视觉效果，只有两条路：切到没有合成器的 VT（只能画图案靠人眼确认），
或先停 X（`systemctl stop sddm` / `set-default multi-user.target`）。

另：`/tmp` 重启后被清（`/tmp/fbtest`、`/tmp/lvgl-probe` 全没了）→ 上机验证不能依赖 `/tmp` 留东西，
二进制和脚本放 `tools/` 或 `build/` 下。
LVGL 可行性探针被这次断电中断，**待重跑**（原计划：用 LVGL 画一帧看 CPU 开销）。

## 6. 双缓冲 / 脏矩形（两条路线）

### 6.1 两个东西是正交的

| | 解决什么 | 手段 |
|---|---|---|
| **脏矩形** | 少写多少像素（带宽） | 只重绘/只搬运变化的矩形 |
| **双缓冲** | 什么时候写上去（时序） | 前/后缓冲，vblank 时交换 |

- 只有脏矩形、没有双缓冲 → 写入量小，但硬件扫描同一块内存，仍有撕裂（窗口小到看不见）
- 只有双缓冲、没有脏矩形 → 不撕裂，但每帧仍要拷 3.5 MB / 重绘整屏
- 两个都要 = 每帧只改小块 + 改完在 vblank 交换 → 终端 UI 的正确终局

### 6.2 fb0 路线（现在能跑）

- `/dev/fb0` 只够一帧（`tegra_fbmem=0x400000`，一帧 3,686,400 B），`yres_virtual == yres`
- 但 `/sys/class/graphics/fb0/pan` 存在 → `tegra_fb` 实现了 `fb_pan_display`
- 想用 pan 翻页必须让 `yres_virtual` 翻倍 → 要改 boot.scr 的 `tegra_fbmem` 到 8 MB + 重打包 uImage（`mkimage`）
- **结论**：fb0 上做「真」双缓冲的成本 = 改引导 + 4 MB 保留内存 + 仍然要停 X；收益只是消掉已经看不见的撕裂 →
  **不值**。fb0 上做的是「软件双缓冲 + 脏矩形」：RAM 里一块 back canvas，front 是 fb。

### 6.3 `tegra_dc_ext` 路线（能做，要停 X）

- `/dev/nvmap` + `/dev/tegra_dc_0` 都能 `O_RDWR` 打开（brejce 在 video 组）
- nvmap 分配的扫描缓冲**不占 fb0 那 4 MB**，走系统内存 → 双缓冲、三缓冲都没有内存问题
- `TEGRA_DC_EXT_FLIP` 翻页 + `TEGRA_DC_EXT_EVENT_VBLANK` 事件 → 真 vsync、零撕裂
- 前提：dc 是排他占用的，X 在跑时拿不到 → 必须停合成器

### 6.4 统一抽象：`present(damage)`

现有后端接口（`src/platform/display_backend.h`）是 `beginFrame()` → `fillRect()/drawTexture()` → `endFrame()`
（`:66` / `:69`）；`Rect` 在 `:12`。加三样就够：

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
空了就早退。上层代码照旧「整屏重画」，但只有脏区的像素被真正写（`soft_canvas.cpp` 的 blendPixel 层做一次
边界判断即可）。这样 `ui::screen.cpp` / `terminal_view.cpp` 一行都不用改。

### 6.5 脏区从哪来（不要做通用 damage 追踪）

在 `fillRect` 里逐个累加矩形会产生几百个碎片，合并开销比省下的拷贝还大。TUI 天然有结构，直接取：

| 脏源 | 位置 | 粒度 |
|---|---|---|
| 终端内容变化 | `TerminalParser::dirty()/clearDirty()`（`src/modules/terminal/terminal_parser.h:39-41`）已有标记 | 目前整个视图 → **建议加 `dirtyRows()` 行位图**（vterm 每行本来就有回调） |
| 终端光标 | `cursor(row,col,visible)` | 2 格（旧位置 + 新位置） |
| 输入法候选行 | `ui::layout::kImeRows` = 2 行 | 1 个矩形 |
| 顶/底状态栏 | 每次状态变化 | `1280×32` / `1280×26` |
| 面板键格 | `ui::screen` 自己知道画了哪格 | 单格（`dpad_cell=34`） |
| 弹窗 | 弹窗 rect | 1 个矩形 |
| 滚动 / 换 tab / 窗口 resize | 整屏 | 直接整屏（不要强行优化） |

**最小可用版本**：先只做「终端行级 + 顶栏 + 底栏 + 整屏兜底」，四类矩形足够覆盖 95% 的帧。

### 6.6 fbdev（软件双缓冲 + 脏矩形）

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

撕裂仍在（硬件扫同一块），但一次只写几十 KB → 撕裂窗口 <1 ms。

每帧收益（真实尺寸：cell 8×21、终端 112×28）：

| 场景 | 写入量 | 占整帧 |
|---|---|---|
| 整屏 | 3,686,400 B | 100% |
| 敲一个键（1 行终端） | 928×21×4 = **77,952 B** | 2.1% |
| 光标移动（2 格） | 2×8×21×4 = 1,344 B | 0.04% |
| 底栏变化 | 1280×26×4 = 133,120 B | 3.6% |
| 一个面板键格 | 34×34×4 = 4,624 B | 0.13% |

### 6.7 `tegra_dc_ext`（真双缓冲 + 脏矩形 + vsync）

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

**双缓冲 + 脏矩形的唯一坑**：翻转之后，另一块 buffer 是**两帧前**的内容。只把本帧脏区重绘进当前 buffer，
另一块在那些区域还是旧像素，下次翻上去会闪一下旧内容。所以每个 buffer 各自记账：

```
dirty[i] = 脏区累计（针对 buffer i）
画进 buffer cur 时：只重绘 dirty[cur]，画完 dirty[cur].clear()
翻转后：dirty[other] |= 这一帧发生的全部变化
```

标准的 "damage per buffer"，25 行左右，纯逻辑，可单测。

### 6.8 改动清单（每步独立可验）

1. `src/platform/damage.h`（新）：`DamageList`（push/merge/clip/area）+ `--selftest` 里 3~4 条断言
2. `soft_canvas.*`：加 `setClip(Rect)`；`fillRect/drawTexture/blendPixel` 求交早退
3. `display_backend.h`：`beginFrame/endFrame` 加 damage 参数，**给默认实现 = 忽略 damage 走旧路径**（SDL/headless 不用改）
4. `src/platform/display_fbdev.cpp:102` 的 `endFrame()`：改成按 damage 逐矩形逐行 memcpy
5. `VTermParser`：`dirtyRows()` 行位图（vterm 行回调里 mark）；`terminal_view` 把脏行 → 像素矩形
6. `ui::screen`：面板键格 / 弹窗 / 顶底栏各自 `push` 矩形
7. `app.cpp` 主循环：攒 damage → `beginFrame(damage)` → 渲染 → `endFrame(damage)`；**没有 damage 就整帧跳过**
8. （可选，之后）`display_dc_ext.cpp`：nvmap 双缓冲 + FLIP + VBLANK，per-buffer damage 记账

第 7 步顺带解决「空闲帧还在重绘」的问题——现在空转也在 full memcpy，改完空闲时是 0 拷贝。

### 6.9 注意事项（都是本项目踩过的坑）

- **宽字符/反色**：damage 必须按**格子边界向外扩**，否则跨两格的中文右半边会被裁掉（见 `../STATUS.md`「已知坑」）
- **滚动**：`VTermBridge` 滚一行等于整屏位移 → 滚动时直接整屏，别试图拆矩形
- **`beginFrame` 现在整屏 `canvas_.clear()`**（`display_fbdev.cpp:95`）→ 改脏矩形后**不能**整屏 clear，
  要么只清脏区，要么把每个绘制调用都改成「填自己的矩形」
- **`line_length` ≠ `w*4`** 在本机恰好相等（2880 = 720×4），但代码不能依赖
- fb0 路线**不要**碰 `FBIOPUT_VSCREENINFO`（§5）
- `tegra_dc_ext` 路线**必须停 X**，且这是唯一能验证「真双缓冲」的路径

### 6.10 一句话结论

- **fb0 + 软件双缓冲 + 脏矩形**：现在就能做，收益 3.5 MB → 几十 KB，撕裂小到看不见，不碰引导不改硬件
- **`tegra_dc_ext` + nvmap 真双缓冲 + vblank**：也能做，零撕裂，但必须停 X，工作量是一个 mini-DRM（几天）
- 两者共用同一个 `present(damage)` 接口，先做前者，显示层实现换掉即可升级到后者

## 7. 能力边界：mini-DRM 不能让别的桌面画

### 7.1 三种「画」必须分开谈

| # | 谁在画 | 需要什么 | 本机 |
|---|---|---|---|
| A | **我们自己的程序**独占屏幕 | `tegra_dc_ext` + nvmap，或 fb0 | ✅ 可行（mini-DRM 指的就是这个） |
| B | 别的程序画进**我们的画布** | 我们当合成器 + 一套协议（Wayland `wl_shm` 或自定 IPC） | ⚠️ 有限可行，见 §7.3 |
| C | 别的桌面（KDE/GNOME/sway/weston）**自己启动** | 内核 KMS + DRM master + EGL/GBM + dmabuf | ❌ 不可能，见 §7.2 |

### 7.2 为什么 C 不可能：桌面要的是一整套内核接口

Wayland 合成器（kwin_wayland / sway / weston / mutter）启动时做的第一件事：

```
open("/dev/dri/card0") → drmGetCap/DRM_IOCTL_MODE_GETRESOURCES
  → DRM_IOCTL_MODE_ADDFB2 / dumb buffer / GEM   （拿 framebuffer 对象）
  → drmModeAtomicCommit                         （atomic modeset）
  → DRM_IOCTL_MODE_PAGE_FLIP + vblank           （翻页）
  → gbm_create_device / EGL_PLATFORM_GBM        （EGL 客户端缓冲）
```

这些**全是内核 drm.ko 侧的 ioctl**。本机现状：

- `/dev/dri` **根本不存在**
- 内核配置里 `drm/tegra` 只有 `udrm.h`（用户态 DRM 桥），**没有 KMS 实现**
- `tegra-udrm.ko` 的 description 是 "Kernel DRM support for **user mode DRM driver**"，
  参数 `modeset=0/1` 默认 0 —— 它是给 NVIDIA 自己的用户态驱动做桥的，**不是**给第三方合成器的 KMS
- `libdrm_nvdc.so` 只存在于 `nvidia-bsp-32-3` 里（且被 diverted 成 `.orig` → `tegra/libdrm.so.2`），
  没有头文件、没有 KMS 能力

`tegra_dc_ext` 提供的是「一个客户端独占显示控制器」（enable/disable 排他），KMS 提供的是
「多客户端共享显示控制器 + 资源对象 + 每客户端 master/flip」。写多少遍 mini-DRM 都补不出 KMS 语义。
要真让 KDE/sway 跑起来，得给这个 4.9 内核 port 一个 `drm/tegra` KMS 驱动（T210 display 控制器），
那是**内核工程**，且上游 drm/tegra 从来没为这套 L4T 显示栈提供过支持。

### 7.3 B 这条路可行到什么程度

| 客户端类型 | 可行性 | 卡在哪 |
|---|---|---|
| `wl_shm` 客户端（纯 CPU 画，如 foot、部分 TUI） | ✅ 可行 | 我们要实现 Wayland 协议（或用一个不依赖 DRM 的合成器骨架） |
| `libEGL`/GBM 客户端（KDE 应用、浏览器、SDL GL） | ❌ 难 | 需要 dmabuf/GBM，而这要 DRM 节点；`tegra_dc_ext` 的 nvmap fd 不等于 GBM |
| X11 客户端 | ✅ 间接 | 必须留着现在的 Xorg + nvidia（X 独占 dc），我们只能当它的一个窗口 |

现实分工：要跑别人的图形程序 → 继续用 X11 + nvidia，我们当其中一个窗口；要极简设备 → 我们独占屏幕自绘；
想两者兼得 → 要么实现 `wl_shm` 合成器（只覆盖 CPU 客户端），要么承认做不到。

### 7.4 `tegra_dc_ext` 不是私有接口（更正）

早期记录写「显示控制器只有 NVIDIA 私有 ioctl，未公开」「零拷贝封死」，**是错的**。

本地证据：

- `/proc/kallsyms` 里有 **50 个 `tegra_dc_ext_*` 符号**：`tegra_dc_ext_control_ioctl`、
  `tegra_dc_ext_control_open`、`tegra_dc_ext_enable/disable`、`tegra_dc_ext_cursor_*`、
  `tegra_dc_ext_event_devops` …
- 三个设备都是 `root:video 0660`，**brejce 属于 video 组**，实测全部能 `O_RDWR` 打开：

```
/dev/tegra_dc_0      O_RDWR OK      （head0 = 内屏 DSI）
/dev/tegra_dc_ctrl   O_RDWR OK      （列连接的显示）
/dev/nvmap           O_RDWR OK      （扫描缓冲分配器）
```

源码是公开的：

| 东西 | 地址 | 开放性 |
|---|---|---|
| switchroot 内核（含 tegra dc 扩展、Joy-Con 驱动、max17042 电池） | `gitlab.com/switchroot/kernel/l4t-kernel-4.9`、`github.com/CTCaer/switch-l4t-kernel-4.9`（分支 `linux-dev`） | 开源 |
| `tegra_dc_ext.h`（ioctl ABI） | `include/video/tegra_dc_ext.h` / `include/uapi/video/tegra_dc_ext.h` | 开源，含 `TEGRA_DC_EXT_FLIP*`、`TEGRA_DC_EXT_SET_NVMAP_FD`、vblank 事件 |
| nvmap（扫描缓冲分配器） | 同仓库 `drivers/video/tegra/nvmap/` | 开源的**内核**部分 |
| `nvidia_drv.so` / `libGLX_nvidia.so` / EGL 用户态 | — | **闭源 blob**（真正封闭的那块） |

不需要反推：自绘 + 双缓冲 + vsync + 多窗口 overlay 用的就是 `tegra_dc_ext`，头文件可直接下载；
nvidia 的 X 驱动用的也是这套 ioctl，只是没公开用法示例。

### 7.5 自己画 + 双缓冲的真实代价（按依赖顺序）

1. 从 L4T 源码取 `tegra_dc_ext.h`（+ nvmap 的 ioctl 结构体）
2. `open("/dev/tegra_dc_ctrl")` → `TEGRA_DC_EXT_CONTROL_GET_OUTPUT_PROPERTIES` 找到内屏
3. `open("/dev/tegra_dc_0")` → `TEGRA_DC_EXT_GET_CAPS` / `TEGRA_DC_EXT_GET_WINDOWS` 看可用窗口数
   （Tegra X1 是 A/B/C + cursor）
4. `open("/dev/nvmap")` + `nvmap_ioctl(NVMAP_IOC_CREATE / ALLOC)` 分配两块 1280×720×4 = 3.5 MB 的扫描缓冲
   —— **不占 fb0 那 4 MB carveout**，走系统内存，所以「双缓冲装不下」在这里不存在
5. 每个窗口 `TEGRA_DC_EXT_SET_NVMAP_FD` 绑定 + 设格式/位置/layering
6. `TEGRA_DC_EXT_FLIP`（或 FLIP4）翻页；`tegra_dc_ext_event_devops` 提供 `TEGRA_DC_EXT_EVENT_VBLANK`，
   `poll()` 等它 → 真 vsync
7. 旋转：可以在 dc 窗口上做（L4T 的 T21x 不支持 HW 旋转，通常软件转），或干脆按 720×1280 竖屏排版

前提没变：X 正在占用同一个 dc（`tegra_dc_ext_enable/disable` 排他）→ 只能在 X 停掉、跑裸 TTY 时用，
这也是为什么必须先配好 SSH。

### 7.6 三条路重新评级

| 路线 | 以前的评价 | 现在 |
|---|---|---|
| fbdev + 脏矩形 | 主推 | 仍然最省，**但是「只能单缓冲 + 撕裂」的妥协** |
| `tegra_dc_ext` 自绘 + nvmap 双缓冲 + vblank | 判为不可能 | **可行**，成本几天（一个 mini-DRM，没人给示例）。认真做 UI 时再上 |
| Wayland / KMS | 不可能 | 仍然不可能（缺 KMS 驱动） |

建议顺序不变：先上 fbdev + 脏矩形把功能跑通；终端本身能用了，再把显示层换成 `tegra_dc_ext`
（同一块屏，换掉 `display_fbdev.cpp` 的实现即可，上层不动）。

## 8. 内存与桌面裁剪

### 8.1 现在的账（2026-11-03 实测）

```
Mem: total 3991 MB   used 1963 MB   free 611 MB   buff/cache 1629 MB   available 2028 MB
```

KDE 桌面栈 RSS 合计 **1756 MB**（RSS 会重复计共享库，去重后实际影响约 **1.1–1.3 GB**）：

| 进程 | RSS |
|---|---|
| plasmashell | 360 MB |
| kded5 | 274 MB ← 正常几十 MB，这个是异常的 |
| kwin_x11 | 130 MB |
| kwalletd5 | 79 MB |
| DiscoverNotifier | 75 MB |
| kdeconnectd | 69 MB |
| polkit-kde-auth agent | 65 MB |
| ksmserver | 61 MB |
| kaccess | 58 MB |
| xdg-desktop-portal-kde | 56 MB |
| klauncher | 52 MB |
| onboard（屏幕键盘） | 83 MB |

没必要留着：KDE 桌面全栈（上表）、onboard（我们自己就是键盘 UI）。
要留着：Xorg + `nvidia_drv.so`（属于 `nvidia-l4t-3d-core`，L4T 包，与 KDE 无关）+ libinput。

### 8.2 为什么「卸载 kubuntu」不划算

```
$ apt-get -s purge kubuntu-desktop
下列软件包将被【卸载】：kubuntu-desktop*        ← 只删 1 个包
```

`kubuntu-desktop` 是 **meta 包**，purge 它不会删掉 plasma-desktop / kwin / kded5 那 273 个真包。
真卸载得自己列一长串，风险：

- 依赖链一旦断到 `xserver-xorg-core`（本机是 **focal 时代的 1.20.13**，不是 noble 的版本），修回来很痛苦
- `nvidia_drv.so` 来自 L4T 仓库，不在 Ubuntu 依赖图里，误删只能重灌包
- 收益只有磁盘（几条 GB），**内存收益为零** —— 不启动就不占内存

### 8.3 推荐方案 A：不卸载，只不启动（可随时回滚）

```bash
# 1) 不再自动起图形会话（sddm 不启动）
sudo systemctl set-default multi-user.target
sudo systemctl stop sddm          # 立刻生效；之后重启就不会自动进 KDE

# 2) 需要时手动起一个裸 X（xinit 已装，1.4.1）
startx                            # 读 ~/.xinitrc
```

`~/.xinitrc` 最小版：

```sh
#!/bin/sh
# 单窗口全屏，不要 WM/合成器
exec ~/Project/switch-ime-term/build/switch-ime-term
```

- 想回 KDE：`sudo systemctl set-default graphical.target && sudo systemctl start sddm`
- 想临时进 KDE（不重启）：`sudo systemctl start sddm`

没有 WM 的注意点：

- 窗口没边框、不能拖动 —— 全屏单窗口正好
- X 默认 focus 策略是 `PointerRoot`：指针在窗口上，键盘就给这个窗口，全屏下等于总是有焦点
- 需要程序主动抢焦点时再装 `xdotool`（现未装，很小）；SDL 自己会 `SDL_SetWindowInputFocus`
- 程序崩了屏幕上什么都没有（`fbcon=map:9`）。建议 `~/.xinitrc` 写成 `exec switch-ime-term || exec sh`，
  或在 X 里留一个 `xterm`（需装，几 MB）

### 8.4 预期收益

| 场景 | used | available | 说明 |
|---|---|---|---|
| 现在（KDE 全跑） | 1963 MB | 2028 MB | |
| 只关 sddm、不跑 KDE，裸 X + 我们的程序 | **约 700–900 MB** | **约 3.0 GB** | Xorg 67 MB + nvidia GL + 系统服务 |
| 再把 onedrive/packagekit/discover 之类 mask 掉 | 再省几十 MB | | |

省下来的主要是那 1756 MB 里能真正回收的部分（plasmashell + kded5 + kwin 三项就 764 MB）。

### 8.5 前置条件与风险（不要跳）

1. **先把 SSH 配通**。切到 `multi-user.target` 之后，X 起不来 = 屏幕黑 + 没有控制台文字，只能靠 SSH。
2. **konsole 会一起消失**。切之前确认我们的程序能起、或系统里有 `xterm`/`fbterm` 兜底。
3. **onboard 不会自动起**（靠 KDE autostart）。想用触摸打英文就往 `~/.xinitrc` 加 `onboard &`（包还在）。
   程序里也留了兜底：**右手柄 `+`（BTN_START）→ `keymap.bind("START","onboard")` → 拉起 onboard**
   （`spawnDetached`，setsid 脱离本进程）。桌面模式下 KDE/合成器可能先把手柄抢走（独占 grab 时本程序
   根本收不到事件），那种情况下这颗键就得在系统层再做一份（KDE 全局快捷键 / evsieve），**不在本仓**。
4. **kwallet/polkit/portal 掉了不影响我们**：SDL + evdev + fbdev 都不依赖它们。
5. 想彻底不加载 KDE 开机服务，可 `sudo systemctl mask packagekit.service`（常驻 65 MB 级，还一直刷 get-updates）。

### 8.6 真要卸载（方案 B）

等裸 X 稳定跑几天、SSH 通了、`SWITCHROOT.md` 的备份步骤做过之后再说：

1. 停 sddm，确认裸 X 正常
2. `apt-mark showauto` 里找出 KDE 侧包，先 `apt purge` 验证机（一次删一小组，每次重启验证）
3. **永远不要**一次 purge 包含 `xserver-xorg-core` / `nvidia-l4t-*` / `xserver-xorg-input-libinput` 的依赖闭包
4. 收益：磁盘几 GB；内存收益 0（已经通过「不启动」拿到）

## 9. 其他实测

### 9.1 剪贴板：新路径根本没实现

- 唯一实现是 legacy 的 `src/main.cpp`（`popen("wl-paste"/"wl-copy")`，约 `:377,389`），
  本机 **wl-copy/wl-paste/xclip/xsel 全缺**
- 新路径 `src/app/app.cpp:423-424`：`case cfg::kCopy: case cfg::kPaste:` 落在空 break
- `copyReq/pasteReq`（`src/platform/display_backend.h:103`）只有 legacy 循环消费，`app::run` 从不读
- 结论：X11 下 `Ctrl+Shift+C/V` 完全静默；装 wl-clipboard 也只救 `--legacy`

### 9.2 fcitx5 永远进不来（代码写死）

`src/platform/display_sdl.cpp:99` 无条件 `SDL_StopTextInput()`（注释：用内置 IME，别让 fcitx5 画候选框）。
X11 下等于不挂 XIM → fcitx5 收不到本窗口的键。

- `engine.ime.enabled=false`（`src/app/ime_route.cpp:123` 特意不吞 Ctrl+Space）只做到「程序内不抢」，
  外层开关把路堵死
- 「桌面模式 + fcitx5 打中文」当前不存在；要么按 `ime.enabled` 决定 Start/StopTextInput，要么永远用内置 IME
- `engine.sink = "uinput"` 也开不起来：`/dev/uinput` 是 `0600 root:root`，brejce 不在 input 组 →
  `src/app/app.cpp:176-180` 静默回落 pty

### 9.3 托盘常驻程序（`org.kde.StatusNotifierWatcher`，2026-11-03）

| 托盘项 | 真身 | RSS | 是 Switch 专属吗 |
|---|---|---|---|
| `nvpmodel` + `nvpmodel_apps` | `/usr/share/nvpmodel_indicator/nvpmodel_indicator.py`（python3） | **37 MB** | ✅ **是**（NVIDIA L4T 电源/频率档） |
| `Fcitx` | fcitx5 | 110 MB | ❌ 通用输入法 |
| `Onboard` | onboard 屏幕键盘 | 83 MB | ❌ 通用（KDE autostart 带的） |
| `Discover 通知` | `DiscoverNotifier` | 75 MB | ❌ 通用 |
| `KDE Daemon`（浏览器集成） | kded 的一个模块 | — | ❌ 通用 |

**任务栏里真正属于 Switch/L4T 的只有 nvpmodel 那一个**（它注册了 2 个图标，看着像"几个"）。
joycond（3.7 MB）不占托盘，它是 systemd 服务。

### 9.4 nvpmodel：为什么值得单独看

- 管 **CPU/GPU 频率档 + 风扇档**，`/etc/nvpmodel.conf` 里的档位：

```
ID=0 Console      ← 当前就是这个
ID=1 Handheld
ID=2 OC_CPU   ID=3 OC_GPU   ID=4 OC_All
ID=5 Perf_All ID=6 Perf_OC_All
```

- `nvpmodel.service` 是 **enabled** 的，开机执行 `/usr/sbin/nvpmodel -f /etc/nvpmodel.conf`
  → **开机默认档由 conf 决定，不依赖托盘**
- 托盘那个 python 只是个 **pkexec GUI**（`pm.set_mode(mode_id, ['pkexec'])`），用来点着切档

**实测发现（重要）**：现在是 `NV Power Mode: Console`。Console = 底座档，CPU/GPU 频率更高、风扇更激进。
掌机模式跑 Console 档 = 白烧电、多发热 —— 和「1.5 小时掉 30% 电」以及那次硬复位都对得上。

```bash
sudo nvpmodel -q          # 看当前档
sudo nvpmodel -m 1        # 切 Handheld
sudo nvpmodel -m 0        # 切回 Console
```

（`nvpmodel -q` 会刷几条 `NVPM ERROR: ... permission` 噪声，那是读风扇档/超频节点没权限，无害。）

### 9.5 关不关：三档选择

| 项 | 建议 | 关掉失去什么 | 怎么关 |
|---|---|---|---|
| **nvpmodel 托盘 GUI（37 MB）** | **推荐关**，改用命令行 | 不能点着切档；`nvpmodel -m N` 照旧，开机默认档不受影响 | 删/改 `/etc/xdg/autostart/nvpmodel_indicator.desktop`（`Hidden=true` 或删文件）；`pkill -f nvpmodel_indicator` |
| **Onboard（83 MB）** | 推荐关（我们自己的 UI 就是键盘） | 触摸打英文得另配屏幕键盘；想留就在 `.xinitrc` 里 `onboard &` | 同法改 `/etc/xdg/autostart/onboard*.desktop` |
| **Discover 通知（75 MB）** | 推荐关 | 不再提示系统更新 | 删 autostart + `systemctl mask packagekit.service` |
| **Fcitx（110 MB）** | **看你** | 别的 KDE 程序（kate/konsole）打不了中文 | 见 §9.2：我们根本不用 fcitx，只在别处用中文时留 |
| **浏览器集成提示** | 随手关 | 无 | kded 模块设置里关 |

顺手清的 L4T 垃圾 autostart（`/etc/xdg/autostart/`）：

```
nvbackground.desktop(.orig)      nvbackground.sh(.orig)
nvchrome.desktop(.orig)          nvchrome.sh.orig
nvlxde-compton.desktop           nvlxde-exempt.desktop   nvlxde-exempt.sh
```

`nvlxde-*` 是 **LXDE** 时代残留（我们跑 KDE），`nvchrome` 是 Chrome 的 GPU 参数脚本。不占内存大户
（脚本跑完就退），但拖慢登录。设 `Hidden=true` 即可，不必删包。

### 9.6 合计能省多少

| 动作 | 省下 |
|---|---|
| 关 nvpmodel 托盘 | 37 MB |
| 关 onboard | 83 MB |
| 关 Discover + mask packagekit | 75 MB + packagekitd（几十 MB 级） |
| 关 fcitx5 | 110 MB |
| **小计** | **≈ 300–350 MB** |

对比 §8 的「不启动 KDE」省的 1.1–1.3 GB：这里是小钱，但风险几乎为零，可以立刻做（不涉 X/sddm，不会把屏幕搞黑）。

建议顺序：

1. **先做零风险那步**：`sudo nvpmodel -m 1`（Handheld），立刻降功耗/温度，不动任何服务。
   想固定成开机默认，改 `/etc/nvpmodel.conf` 的默认档或给 `nvpmodel.service` 加 `-m 1`
2. 再把 4 个 autostart 设 `Hidden=true`（nvpmodel 托盘 / onboard / Discover），重新登录生效
3. Fcitx 留着，确认「不在 KDE 里打中文」之后再关
4. KDE 整体不启动留给下一步（需要先有 SSH）

## 10. 复现命令清单

```bash
# fbdev 探针（放 build/ 下，别放 /tmp —— 重启会清）
g++ -O2 -o build/fbtest tools/fbtest.cpp
./build/fbtest --info        # 只读，安全
./build/fbtest --paint       # mmap 写 + 读回，安全
# ./build/fbtest --rotate 1  # 危险：见 §5，别在有 X 的时候跑

# 显示栈能力
ls /dev/dri                                     # → 不存在
ls /proc/kallsyms | grep -c tegra_dc_ext         # → 50 个符号
ls /sys/class/graphics/fb0/pan                  # → 存在（fb 能 pan）
modinfo tegra-udrm | grep parm                  # → modeset=1 enable, 0 disable(default)
dpkg -L nvidia-bsp-32-3                         # → 7 个文件，只有 libdrm_nvdc 符号链接，无头文件
ls /usr/src/linux-headers-4.9.140-l4t/include/config/drm/tegra   # → 只有 udrm.h

# 手柄
for d in /dev/input/event*; do echo "$d $(cat /sys/class/input/$(basename $d)/device/name)"; done
dmesg | grep joycon | tail -20
./build/switch-ime-term --list-gamepad

# 省电 / 内存
sudo nvpmodel -q
free -m
```

---

## 11. 2026-11-03 夜：桌面已关、掉电排查记录

这一夜的改动全部落在这台机（不是计划），记录在此以免下次重复踩。

### 11.1 桌面已停用（未卸载）

```
sudo systemctl set-default multi-user.target   # 已生效
sudo systemctl stop sddm                        # 已生效
```
- 开机不再进 KDE/Xorg；内存占用 `1238 MB → 330 MB`
- 恢复桌面：`sudo systemctl set-default graphical.target && sudo systemctl start sddm`
- **删桌面后没有 logind 会话**，两件事会跟着坏，见 11.2 / 11.3

### 11.2 `/dev/input/*` 与 `/dev/uinput` 权限（必须有 udev 规则）

无会话时 logind 不下发 uaccess ACL，而设备是 `crw------- root input`，**谁都读不了**（`Permission denied`）。
已装 `/etc/udev/rules.d/70-sit-input.rules`：
```
KERNEL=="event*", SUBSYSTEM=="input", MODE="0660", GROUP="input"
KERNEL=="uinput", SUBSYSTEM=="misc", MODE="0660", GROUP="input"
```
并把 `brejce` 加入 `input` 组（新登录生效）。`STATUS`/`ARCH` 里“权限已不是问题、不需要 70-sit-input.rules”的说法**只在有桌面会话时成立**。

### 11.3 WiFi 是 per-user 连接，无桌面就连不上（已修）

`/etc/netplan/90-NM-3fa3aee-*.yaml` 里带 `connection.permissions: "user:brejce:;"` 且
`wifi-security.psk-flags: 1`（密码归 agent 所有、不存盘）→ 没有会话时 NM 不激活，直接“没网”。
已补一条系统级连接 `/etc/NetworkManager/system-connections/98-hotspot.nmconnection`
（`ssid=brejce的Mate 60 Pro`，psk 存盘，无 permissions，`autoconnect-priority=100`），开机即连，不依赖桌面。

### 11.4 会自己挂起（已 mask）

`systemctl stop sddm` 之后整机**自己 suspend**，`/var/log/syslog` 有：
```
18:48:59 NetworkManager: sleep: sleep requested
18:49:00 kernel: PM: suspend entry
```
挂起后 WiFi 被置 unmanaged，从外面看就是“死机”；只能长按电源硬断电再开（硬断电会留脏 fs，`e2fsck -n` 验证过没坏）。
已 mask：`sleep.target / suspend.target / hibernate.target / hybrid-sleep.target / suspend-then-hibernate.target`，
并加 `/etc/systemd/logind.conf.d/99-no-sleep.conf`（`IdleAction=ignore`、`HandleSuspendKey/Hibernate/Lid=ignore`、`HandlePowerKey=poweroff`）。
**注意**：短按电源在 logind 未起来时也不会关机（启动早期卡住时只能长按 12 秒）。

### 11.5 控制台黑屏的原因（`fbconsole=0` 没生效）

`bootloader/ini/L4T-noble.ini` 加了 `fbconsole=0` 想打开内核日志，**实测无效**：
`/proc/cmdline` 仍是 `fbcon=map:9 consoleblank=0`。所以 TTY 有 getty、键盘能打，但**屏幕上没有任何文字**，OTG 键鼠等于盲打。
要修得动 `boot.scr` 里的 `${fbconsole}`（`switchroot/ubuntu-noble/boot.scr`），未做。

### 11.6 直连网线的 PC 侧脚本

`tools/pc-host-switch.sh enp5s0` 起 DHCP + NAT，Switch 拿 `192.168.50.x`；
**收工要带接口名**：`sudo ./tools/pc-host-switch.sh --down enp5s0`（脚本原先会把接口名当成 `--down`，已修）。

## 12. 终端字体必须是「真的」等宽（2026-11-04）

**现象**：终端区文字看起来字距坏了——`hello` 显示成 `hel l o`，`switch` 成 `swi tch`，
`Display` 成 `Di spl ay`。只影响窄字形（`i` `l` `j` `t` `.` `|`），`M` `A` 和汉字正常；
面板（键帽、底栏）也跟着散。截图和缩放都不改变它，**不是渲染错位**。

**根因**：`src/ui/theme.h` 的 `fam::kMono` 要的是 `Maple Mono NF CN`，这台机器**没装**。
fontconfig 对不存在的族名不报错，直接 `FcFontMatch` 给一个默认族——这里是 `Noto Sans`
（**比例字体**）。字形按自身 advance 落位，而网格每格固定 8px，于是窄字形前后各空一截。

判定一行：

```fish
fc-match "Maple Mono NF CN"
# 缺 → NotoSans-Regular.ttf: "Noto Sans" "Regular"      ← 就是它
# 有 → MapleMono-NF-CN-Regular.ttf: "Maple Mono NF CN" "Regular"
```

**修**（不需要 root，装在用户目录）：

```fish
mkdir -p ~/.local/share/fonts
rsync -av ~/本地路径/MapleMono-NF-CN-{Regular,Bold}.ttf brejce@<switch>:~/.local/share/fonts/
ssh brejce@<switch> 'fc-cache -f && fc-match "Maple Mono NF CN"'
```

两个 ttf 各 ~20MB（CJK + Nerd 图标）。**别用 `Noto Sans Mono` 顶替**：advance 也是 0.6em，
网格能对齐，但丢了 1:2 中英宽度比和 Nerd 图标，属于临时凑合。

**代码里的防御**：`render::FontManager::get()` 在「实际匹配到的族名 ≠ 请求的族名」时打
`fontconfig: 要 'X' 实际拿到 'Y'（没装？）`。以后换字体/上新机器，看启动日志这一行就知道，
不要再靠肉眼发现"字距有点怪"。

**排查教训**：先在无头后端复现（`--backend headless --out f.png --run "bash /tmp/t.sh"`），
再用像素量一遍——所有字符起点步距都是 8px，就能立刻排除"渲染错位"，把方向锁到字体本身。

## 13. 能不能自己构建镜像 + 塞 DRM/DRI？（2026-11-04 实测结论）

**问题**：switchroot 支持自定义镜像吗？能不能顺手把 DRM/DRI 装进去，换来 `/dev/dri`？

**结论分两半**：

### 13.1 自定义镜像：支持

`theofficialgman/l4t-image-buildscripts`（社区维护者，就是镜像作者）两步：

```bash
# ① 造 rootfs（Kubuntu / Ubuntu-Unity 要从零造；GNOME 用官方 RPi 预装镜像）
sudo ./scripts/nv_build_samplefs.sh --abi aarch64 --distro ubuntu --flavor kubuntu-desktop --version noble
# ② 把 switchroot 包 + 你的定制打进 rootfs，产出 hekate 可装的 7z
./scripts/apply.sh kde-noble        # 可选 gnome-jammy / gnome-noble / kde-noble / unity-noble
```

输出在 `output/`。中间那步 rootfs 就是个普通 Ubuntu arm64，**想装/删什么包都行**（这也是它真正的价值：
裁掉 KDE、预装我们的依赖、固化 systemd unit 与字体、开机直接进 switch-ime-term —— 省 1.1–1.3GB 内存）。
另有 `theofficialgman/l4t-debs` 是 switchroot 专用 deb 的 APT 源。

### 13.2 但加 DRM/DRI 包没用：这台机器的内核根本没有 KMS

`/proc/config.gz` 实测：

```
CONFIG_DRM=y
# CONFIG_DRM_TEGRA is not set     ← 主线 tegra KMS 显示驱动被关掉
CONFIG_DRM_TEGRA_UDRM=m           ← 只有「用户态 DRM」shim（nvdc 要配 libdrm_nvdc 干活）
CONFIG_FB_TEGRA=y                 ← 显示实际走 fbdev
```

所以 `/dev/dri/card*` 永远不会出现，装多少 `libdrm-*` / `mesa` 都没用（它们已经装了：`libdrm-tegra0、
libgl1-mesa-dri 25.2.8`，没有 KMS 设备就是摆设）。`libdrm_nvdc.so.orig` 的存在说明发行版把 nvdc 驱动
换成了 l4t 版——Xorg 能跑就是靠它 + nvidia 的 EGL，与 KMS 无关。

要真 KMS 只能自己编内核并补下游显示补丁（4.9 l4t 树里显示走 nvdc，不是主线 tegra-drm），代价远大于收益。

### 13.3 好消息：不靠 DRM 也能拿到硬件 GL

`nvidia-l4t-3d-core 32.3.1` 提供 `libEGL.so` / `libGLESv2.so`，走 `/dev/nvhost-*`（与 KMS 无关）。
裸 TTY（Xorg 关着、我们占着 fb0 的时候）实测离屏可用：

```
EGL 1.5  vendor=NVIDIA
GL_RENDERER = NVIDIA Tegra X1 (nvgpu)/integrated
GL_VERSION  = OpenGL ES 3.2 NVIDIA 32.3.1
离屏 pbuffer + glClear + glReadPixels → 像素正确
```

探针源码：`/tmp/eglprobe.c`（`gcc eglprobe.c -o eglprobe -lEGL -lGLESv2`）。要复现随时重编。

### 13.4 对渲染路线的影响

| 路线 | 可行性 | 说明 |
|---|---|---|
| A 软件渲染 → `fb0` | **现在就是** | 单缓冲 carveout，会撕裂；`DEVICE.md` §6 的脏矩形是第一步优化 |
| B `tegra_dc_ext` + nvmap 双缓冲 + FLIP/VBLANK | 可行，**不需要 KMS** | 真 vsync、零撕裂；`/dev/tegra_dc_*` 已存在 |
| C EGL/GLES 离屏渲染 → 贴到 B 的 buffer（或 readback → `fb0`） | 可行，已实测 | 读回像素约 3.5MB/帧；更省的是直接渲染进 nvmap surface |
| D 标准 KMS / Wayland 合成器 / `--vo=gpu` 类 | **不可行** | 没有 `CONFIG_DRM_TEGRA`；Xorg 是 nvdc 专有路径 |

顺序建议不变：先 B（去撕裂），再考虑 C（把 SoftCanvas 换成 GLES，或只把贴图/缩放交给 GPU）。

## 14. 电源档 / 风扇 / 充电限制（2026-11-07 实测）

三样东西在 KDE 托盘里都能点，但**都来自同一个程序**：`/usr/share/nvpmodel_indicator/nvpmodel_indicator.py`
（`/etc/xdg/autostart/nvpmodel_indicator.desktop` 自启），底层是 L4T 的 `/usr/sbin/nvpmodel`。
托盘菜单里那个「Joy-Con Mapping」只是它弹的一张图 `jc_map.png`（就是 `50-joystick.conf` 映射的可视化，无行为）。

### 14.1 电源档：**7 档**，不是 3 档

定义在 `/etc/nvpmodel.conf`，当前档在 `/var/lib/nvpmodel/status`（形如 `pmode:0000 fmode:Console`）。

| ID | 名字 | CPU min | CPU max | 超频 | GPU max |
|---|---|---|---|---|---|
| 0 | Console（出厂默认） | 0 | 1.785 GHz | 0 | 768 MHz |
| 1 | Handheld | 0 | 1.428 GHz | 0 | 460.8 MHz |
| 2 | OC_CPU | 0 | 3.0 GHz | 1 | 768 MHz |
| 3 | OC_GPU | 0 | 1.785 GHz | 0 | 2 GHz（实际封顶 921.6 MHz） |
| 4 | OC_All | 0 | 3.0 GHz | 1 | 2 GHz（同上） |
| 5 | Perf_All | **1.02 GHz** | 1.785 GHz | 0 | 768 MHz |
| 6 | Perf_OC_All | **1.02 GHz** | 3.0 GHz | 1 | 2 GHz（同上） |

- 切档：`sudo nvpmodel -m N`，立刻生效（写 `scaling_max_freq` / `overclock` / GPU `max_freq`），
  回来 `cat /var/lib/nvpmodel/status` 应看到 `pmode:000N`
- **开机是持久的**：`nvpmodel.service`（`ExecStart=/usr/sbin/nvpmodel -f /etc/nvpmodel.conf`）按 status 文件恢复
- 不超频时硬件真实上限：CPU 最高 `2091000`、GPU 最高 `921600000`；mode 0 只用得到 1.785 GHz
- 超频档能到 3.0 GHz 的原因：`CPU_OVERCLOCK 1` 会写 `/sys/kernel/tegra_cpufreq/overclock`，
  内核才把超频频率表暴露出来（关掉就只有上表那串）
- 实测差异：`-m 5` → `scaling_min_freq` 变 1,020,000（Perf 档锁下限）；`-m 0` → 回到 102,000

**和本项目的关系**：整屏旋转 blit 是单线程纯主频活（§4.4 里那条 14.2ms / 247MB/s 是在 mode 0 下量的）。
出厂默认就是 mode 0；要"打字不偶尔卡一下"用 mode 5（不超频，只是锁住降频下限）；OC 档发热/掉电自负。

### 14.2 风扇

`sudo nvpmodel -d Console|Handheld|Cool`（写 thermal-fan-est / pwm-fan 曲线，官方 help 里写的是
`nvpmodel -d mode_name: set mode_name to fan`）。当前值同样在 status 里（`fmode:`）。

### 14.3 插拔底座会自动改档

`/var/lib/nvpmodel/auto_profiles` = 1（默认）时，`/usr/bin/dock-hotplug`
（由 `nintendo-switch-display.desktop` 以 `pkexec` 启动）在插拔时执行：

- 插底座 → `nvpmodel -m 0 -d Console`
- 拔掉 → `nvpmodel -m 1 -d Handheld`

所以**手设的档位会被插拔事件覆盖**。要固定：`sudo sh -c 'echo 0 > /var/lib/nvpmodel/auto_profiles'`。

### 14.4 充电限制

- 内核节点：`/sys/class/power_supply/usb/charge_control_limit`，`0` = 无限制（当前就是 0）
- 档位定义 `/etc/nvpmodel/nvpmodel_charging.conf`：`LIMIT=0` / `LIMIT=55 Always_Docked` / `LIMIT=85 High_Charge`
- **托盘显示的数字和实际写的值不一样**（`nvpmodel_indicator.py` 里的 `no = eval(mode.id); if no is not 0: no += 5 else: no = 100`，
  纯显示修饰）：

  | 托盘菜单显示 | 实际写进 `charge_control_limit` |
  |---|---|
  | `100%: No Limit` | `0` |
  | `60%: Always Docked` | `55` |
  | `90%: High Charge` | `85` |

  **脚本里要写 55 / 85，照菜单写 60 / 90 会得到另一个限制值。**
- 设置 / 读取走 helper（设置会同时写 `/var/lib/nvpmodel/charging_status` 做持久化）：

```bash
sudo /usr/share/nvpmodel_indicator/nvpmodel_helper.sh 7 55   # 设成 55
sudo /usr/share/nvpmodel_indicator/nvpmodel_helper.sh 6      # 读当前值（退出码就是值）
sudo /usr/share/nvpmodel_indicator/nvpmodel_helper.sh 9      # 读保存的值
```

- **坑：裸机跑本项目时重启会丢。** 只有托盘程序（`nvpmodel_indicator.py`，KDE 登录时启动）会把
  `charging_status` 读回来写进节点；我们的终端裸机跑，没有托盘 → 重启后又是 0。要持久就加一个 oneshot unit：

```ini
[Unit]
Description=Switch charging limit (Always docked)
[Service]
Type=oneshot
ExecStart=/bin/sh -c 'echo 55 > /sys/class/power_supply/usb/charge_control_limit'

[Install]
WantedBy=multi-user.target
```

长期插底座当终端用，建议 55~85，别常年顶在 100%。

### 14.5 托盘菜单里其它几项（同样是那个 python）

| 菜单项 | 实际命令/行为 |
|---|---|
| `Apps: Tegra Stats` | `x-terminal-emulator -e pkexec tegrastats-l4t`：L4T 自带的实时监控（C 程序，读 /sys + /proc），一行形如 `RAM 1263/3992M` / `CPU 8%@1785 11%@1785 …` / `EMC_FREQ 1%@1600 GR3D_FREQ 0%@76`。**验证 powermode 是否生效最快的办法**（括号里的频率就是当前上限档位） |
| `Reboot 2 Config` | `pkexec r2c`（`/usr/bin/r2c`，252 行 bash + yad 图形列表 "Choose an entry to reboot to:"）：选一个启动项，写 `/sys/devices/r2p/action` + `param1/param2` 然后**立刻 reboot**，进 Hekate / Android / 另一个 Linux，不改当前系统的启动文件。`/sys/devices/r2p` 是 switchroot 内核的 "reboot to payload" 驱动 |
| `Joy-Con Mapping Help` | 开一个 GTK 窗口显示 `jc_map.png`（就是手柄映射的那张图） |
| `Settings: Automatic profiles` | 勾选 = `/var/lib/nvpmodel/auto_profiles` 为 1（见 §14.3） |

`Color Mode:` 子菜单在本机**不出现**：源码里 `color_modes = False` 写死，运行时还被 helper 探测覆盖
（`panel_color_mode` 节点存在才显示）。要改面板配色只能直接写 `/sys/devices/50000000.host1x/tegradc.0/panel_color_mode`。

## 15. overlay 形态：openbox + 真终端（2026-11-07）

目标形态：不再自绘终端，改成「窗口管理器管真窗口 + 我们只画覆盖层」。

```
顶栏（我们画，XShape 可见区之一）：前台窗口完整标题，其它窗口只显示标题前两个码位
中间（我们窗口的"洞"）：alacritty / chromium 的真实窗口，触摸点击直接落到它们身上
左右侧栏（我们画）：现有键区、吸附、震动、IME —— 逻辑一行没改
```

要点与踩过的坑：

- **没有合成器**，所以"透明"只能用 `XShape`：`ShapeBounding`（看得见）和 `ShapeInput`（点得到）
  设成同一组矩形 = 顶栏 + 左右侧栏，中间是真空洞。
- 覆盖层必须 `_NET_WM_STATE_ABOVE`（否则普通窗口盖住顶栏和键区）；同时它会成为"活动窗口"，
  所以每 500ms 把 X 焦点还给真正的那个窗口，否则 uinput 打的字全进覆盖层自己。
- **Xlib 默认错误处理是 exit**：`XSetInputFocus` 撞上刚被关掉的窗口会 `BadWindow` 直接带走进程，
  X 写操作都要包 `ErrorGuard`（`src/platform/x11_overlay.cpp`）。
- 窗口几何靠 openbox 的 per-app 规则钉死（`~/.config/openbox/rc.xml` 的 `<applications>` 里，
  **必须在 `<applications>` 段内**，写到 `</openbox_config>` 前是无效的）：
  `decor=no` + `position force=yes` 176,32 + `size` 928x662。chromium 会自己 resize，
  得用它的 `--window-position=176,32 --window-size=928,662`。
- 切窗口：我们合成 `Alt+Up` / `Alt+Down`（`Alt` = DPAD_RIGHT），openbox 绑 `A-Up`=NextWindow、
  `A-Down`=PreviousWindow。为什么不是裸方向键：那样 openbox 会把方向键全吞掉，alacritty 里就用不了。
- 一键启动：`tools/start-overlay.sh`。

### 15.1 overlay 下按键进不去？四个坑按顺序排

1. `ukbd` 只在 `eng.sink=="uinput"` 时打开 —— overlay 没有 PTY，必须强制 `sink=uinput`。
2. 按键路由被 `if (!overlayMode)` 整段跳过 —— overlay 下也要跑，字节由 uinput 打给焦点窗口。
3. **uinput 用老的 `uinput_user_dev` 写法创建 → X 认得设备但事件不生效**。
   `xinput list` 能看到 `switch-ime-term virtual keyboard`，`/proc/bus/input/devices` 也有，
   但发出的 Alt+Up 谁都不响应。换成 `UI_DEV_SETUP` + `uinput_setup` 立刻正常。
   （同样发 Alt+Up：新写法能切窗口，老写法毫无反应，这台 4.9 内核如此。）
4. 覆盖层每 500ms `XSetInputFocus` 抢焦点 → 跟 openbox 的 `Alt+Up` 切换互相打架，表现是"切了又弹回去"。
   正确做法：`WM_HINTS.input=False`（不接受焦点），只在启动瞬间补一次焦点，之后绝不抢。

X 写操作（`XSetInputFocus`/`XReparentWindow`/`XMapWindow`…）都要包 `ErrorGuard`：
Xlib 默认错误处理是 `exit`，一个刚被销毁的窗口 id 就能带走整个覆盖层。
