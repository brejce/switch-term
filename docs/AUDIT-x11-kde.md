# 环境审计：X11 + KDE + joycond（2026-11-03 实测）

> 机器就是目标机：Switch 2017 / switchroot Ubuntu 24.04.4 (noble) / kernel 4.9.140-l4t / arm64 / 3.9Gi。
> 会话 `XDG_SESSION_TYPE=x11`、`kwin_x11`、xrandr **1280x720**、NVIDIA tegra 驱动 32.3.1（GL 4.6 直通，**无 `/dev/dri`**）。
> 桌面未 purge：plasmashell 360MB + kded5 274MB + fcitx5 110MB。
> 手柄只有一个 joycond 合成设备：`event8` = `Nintendo Switch Combined Joy-Cons`（Bus 0006, 057e:2008）。

## 1. 手柄十字键全失效（阻断·功能）

- `src/input/evdev_gamepad.cpp:114-121`（新路 `PadState`）与 `:51-60`（legacy `GamepadMapper`）**只读 `ABS_HAT0X/ABS_HAT0Y`**
- 本机 `event8` 的 ABS 只有 `X Y RX RY`（位 0,1,3,4），**没有 HAT**；十字键走按键 `544-547 = BTN_DPAD_UP/DOWN/LEFT/RIGHT`
- 后果：`kPadUp/Down/Left/Right` 永不产生 → `tab`、`mod_ctrl/shift/alt`、三个 `lock_*` 全废，
  连带 `Ctrl+Space` 切中英（唯一入口）也按不出来
- 修法 ≈6 行：在 `EV_KEY` 分支加 4 个 `case BTN_DPAD_*` → `kPadUp/Down/Left/Right`，HAT 分支保留以兼容原生 hid-nintendo
- 设备筛选没问题：`:158-170` 只要求 `(ABS_X && ABS_Y) || BTN_SOUTH`，event8 会被正确打开，不会误收 touchscreen/gpio-keys

### 1.1 开机时「手柄不可用」的真相（2026-11-03 两次 boot 对比）

**本次 boot（15:47）—— 全好**：两只 Joy-Con 开机 10 秒内握手成功，设备齐：

```
/dev/input/event6  Nintendo Switch Left Joy-Con Serial     (js0)
/dev/input/event7  Nintendo Switch Right Joy-Con Serial    (js1)
/dev/input/event8  Nintendo Switch Combined Joy-Cons        (js2)  ← joycond 合成，程序读这个
```

**上次 boot（14:23）—— 只手柄**：启动时**只有右手柄**（`serial0-0`）握手成功，左手柄直到
**15:02:01**（开机 38 分钟后）才连上；之后 15:09、15:30 又反复重新握手。
→ 「手柄不可用」不是驱动/joycond 的问题，是**左滑轨只连上了一边**。

**排查顺序**（下次先看这个）：

1. `for d in /dev/input/event*; do echo "$d $(cat /sys/class/input/$(basename $d)/device/name)"; done`
   必须有 **event6 + event7 + event8 三个**；只剩 event8 说明 joycond 活着但某只手柄没上来
2. `dmesg | grep joycon | tail -20` —— 看 `Detected left/right joy-con` 和 `completed handshake` 两边是否都有
3. 两边都不报 → 滑轨接触（推紧 / 擦金手指），与软件无关；`Received pkt is malformed` 大量刷屏也是同一个接触问题

### keymap 在真机的可用度：12 条里 8 条

| 可用 | 不可用 |
|---|---|
| LB=magic、LT=space、A/B/X/Y=enter/backspace/delete/escape、RB/RT=鼠标左右 | DPAD_UP=tab、DPAD_LEFT=ctrl、DPAD_RIGHT=shift、DPAD_DOWN=alt、3 个 `lock_*` |

- `config/keymap.lua:7` `LT` 实际映射 `BTN_TL2` = 物理 **ZL**（命名错位，不影响功能）
- `config/keymap.lua:27` `STICK_R mouse_move` 只有 `lua_config.cpp:86` 解析成 bool，**无消费点** → 右摇杆移鼠标未实现

## 2. 构建链完全缺失（阻断·构建）

`cmake -B build` 第一条就 ENOENT。实测缺：cmake、ninja、`freetype2/fontconfig/libpng/sdl2/vterm/lua5.4` 六个 `.pc` 全无，
`/usr/include/{vterm.h,lua5.4,freetype2,libpng16}` 全无，libvterm 连运行时都没有。

```
sudo apt install cmake build-essential pkg-config libfreetype-dev libfontconfig1-dev \
                 libpng-dev libvterm-dev liblua5.4-dev libsdl2-dev
```

ports.ubuntu.com 上都有：cmake 3.28.3 / libvterm-dev 0.3.3 / liblua5.4-dev 5.4.6 / libsdl2-dev 2.30。磁盘 68G 空闲。

**联动陷阱**：SDL2 在 `CMakeLists.txt:13` 是可选的，但 `main.cpp:843` 只要 `DISPLAY` 存在就选 `"sdl"`，
`main.cpp:944` 遇未注册后端**直接 return 1，不回落 fbdev/headless** → 漏装 `libsdl2-dev` 的表现是
`backend 'sdl' not found`，极易误判。

## 3. SDL 渲染器没有软件回落（高风险）

`display_sdl.cpp:87` 只请求 `SDL_RENDERER_ACCELERATED | PRESENTVSYNC`，失败即 `return false` → `main.cpp:949`
"backend init failed" 退出。tegra 专有驱动下必须先实测；失败就给 `SDL_RENDERER_SOFTWARE` 二次尝试（3 行）。

## 4. 剪贴板：新路径根本没实现

- 唯一实现是 legacy 的 `main.cpp:377,389`（`popen("wl-paste"/"wl-copy")`），本机 **wl-copy/wl-paste/xclip/xsel 全缺**
- 新路径 `src/app/app.cpp:421-429`：`case cfg::kCopy: case cfg::kPaste:` 落在空 break
- `copyReq/pasteReq`（`display_backend.h:101`）只有 `main.cpp:1237-1243` legacy 循环消费，`app::run` 从不读
- 结论：X11 下 `Ctrl+Shift+C/V` 完全静默；装 wl-clipboard 也只救 `--legacy`

## 5. fcitx5 永远进不来（代码写死）

`src/platform/display_sdl.cpp:100` 无条件 `SDL_StopTextInput()`（注释：用内置 IME，别让 fcitx5 画候选框）。
X11 下等于不挂 XIM → fcitx5 收不到本窗口的键。

- `engine.ime.enabled=false`（`ime_route.cpp:123` 特意不吞 Ctrl+Space）只做到「程序内不抢」，外层开关把路堵死
- 「桌面模式 + fcitx5 打中文」当前不存在；要么按 `ime.enabled` 决定 Start/StopTextInput，要么永远用内置 IME
- `engine.sink = "uinput"` 也开不起来：`/dev/uinput` 是 `0600 root:root`，brejce 不在 input 组
  → `app.cpp:176-180` 静默回落 pty

## 6. 与 STATUS.md 不符的旧记录

| STATUS 里写的 | 实际 |
|---|---|
| `/dev/fb0` 2560×1600 32bpp | **720×1280 32bpp**（那条是别的机器测的） |
| 用户不在 input 组，需加 `70-sit-input.rules` | `event8` 已有 `user:brejce:rw-` ACL（logind uaccess），**不需要** |
| Wayland 下自动选 sdl | 现在是 X11，自动判定仍然命中 sdl，但 SDL dev 未装 |
| 剪贴板走 wl-clipboard | X11，且工具全缺，且新路径未实现 |
| F 节假设「目标机没有桌面环境，我们独占 framebuffer」 | 现在有 KDE + OpenGL，**但无 `/dev/dri`** |

**F 节结论要改**：路线 2（cage/weston 换 VT）和路线 3（wlroots 当合成器）都要 DRM master，
tegra 专有驱动 + 无 `/dev/dri` 基本不可能；只有路线 1（库级 offscreen → `drawImage`）现实，
且现在有 GL，走 SDL renderer/texture 即可，不必碰 KMSDRM。

## 7. 字体

`src/ui/theme.h:35` 写死 `"Maple Mono NF CN"`，本机 **fc-list 0 命中**，落到 `Noto Sans CJK SC`（非等宽）。
不致命（代码里用到的非 ASCII 全是全角/CJK/箭头 U+2192，Noto 都覆盖），但 1:2 网格与 Nerd 图标没了。
要么装字体，要么把等宽回退串写进 `theme.h`。

## 8. 屏幕绘制流程（这台机的真实路径）

### 8.1 现代 Linux 标准栈（本机不适用）

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

### 8.2 本机实际：无 DRM，混合路线

- `/dev/dri` **不存在**；`lsmod` 无任何 drm 模块
- 内核只有 `gpu/drm/tegra_udrm/tegra-udrm.ko`（NVIDIA 用户态驱动辅助，**不是 KMS 驱动**）；`modprobe tegra-drm` → not found
- 显示控制器以**字符设备**暴露：`/dev/tegra_dc_0`、`/dev/tegra_dc_1`、`/dev/tegra_dc_ctrl`
- 渲染走 `/dev/nvhost-gpu`、`/dev/nvhost-ctrl-gpu`（nvgpu）
- 面板：`tegradc.0: disp0 connected to head0->/host1x/dsi`，DSI 直连

| | X11（当前） | fbdev（计划中） |
|---|---|---|
| 像素产生 | `SoftCanvas` → `SDL_UpdateTexture` | `mmap(/dev/fb0)` 直接 memcpy |
| 传递 | X11 协议 → Xorg + nvidia_drv 32.3.1 | 内核 `tegra_fb` |
| 合成 | kwin_x11 用 GL 合成（有 `Present` 扩展、NV-GLX） | 无 |
| 上屏 | nvidia 驱动 ioctl `/dev/tegra_dc_*` + 前后缓冲 flip（**有 vsync，不撕裂**） | 硬件持续扫描同一块内存（**撕裂**） |
| GPU | 可用（Tegra X1, GL 4.6 direct rendering） | 不可用 |

### 8.3 Wayland 不可能

Wayland 合成器需要 DRM master + GBM/atomic。本机连 `/dev/dri` 都没有，`weston/sway/cage/wayfire/labwc/river` 全未安装。
「离开 X11」的唯一出路是 fbdev 裸机，**不是换 Wayland**。

### 8.4 fbdev 的四个硬约束

1. **竖屏且不旋转**：Switch LCD 物理 720×1280。`/sys/class/graphics/fb0/modes` = `U:720x1280p-60`，`rotate` = **0**。
   Xorg 的 nvidia 驱动替你转了 90°（`xrandr` 显示 `DSI-0 1280x720 normal`），裸机没有。
2. **画布对不上**：`layout.h:10-11` 设计尺寸 1280×720，`display_fbdev.cpp:2,69` 是「1:1 居中，不转不缩」，注释写「Switch 恰好 1280×720」→
   `ox_ = (720-1280)/2 = -280`，一帧都画不对。
3. **单缓冲，双缓冲物理上不可能**：内核 cmdline `tegra_fbmem=0x400000@0xf5a00000` = carveout 共 **4 MB**；
   720×1280×4 = 3.51 MB，`smem_len` = 4194304 → 剩余不足 0.5 MB。`virtual_size` 等于 mode，`FBIOPAN_DISPLAY` 无意义。
4. **没有控制台兜底**：内核 cmdline `fbcon=map:9`，`/sys/class/vtconsole/*/name` = `dummy device` →
   切 TTY 看不到 shell 提示符，程序崩了屏幕上什么都没有。**必须先配好 SSH 再 purge 桌面。**

### 8.5 三条路线

| 路线 | 做法 | 代价 |
|---|---|---|
| **主推：fbdev + 软件旋转** | 画布保持 1280×720，输出时整块转 90° 写进 720×1280 的 fb | 每帧多搬 92 万像素（A57 上约几 ms）；零依赖，不背 plasmashell 360MB + kded5 274MB |
| 备选：留在 X11 | 只在 SDL 全屏窗口里跑，不用改旋转逻辑（nvidia 驱动已转好） | 背整套桌面；`display_sdl.cpp:87` 无软件渲染回落，tegra GL 开窗失败即退出 |
| 不做：等 Wayland/DRM | — | 这版内核 + 专有驱动，短期无解 |

软件旋转之前先花 10 分钟试 `FBIOPUT_VSCREENINFO` 把 `var.rotate` 设成 1；`tegra_fb` 可能不支持，不支持就老实 CPU 转。

## 最小执行顺序

1. 装 7 个依赖包 → `cmake -B build` 跑通
2. `evdev_gamepad.cpp:124` 补 `BTN_DPAD_*`（不修则 E1/E2 上机验证走不通）
3. `display_sdl.cpp:87` 加软件渲染回落（先保证能开窗）
4. 再动剪贴板（`app.cpp` 空分支）与 `StopTextInput`
5. fbdev 上机前先解决旋转 + 画布尺寸（§8.4/8.5），并确认 SSH 可用
