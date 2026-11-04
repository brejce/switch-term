# 探针实测：fbdev 尺寸/旋转 + LVGL 可行性

> 机器：Switch 2017 / switchroot Ubuntu 24.04.4 arm64 / kernel 4.9.140-l4t / 3.9Gi。
> 会话：X11（kwin_x11）+ NVIDIA tegra 专有驱动，**无 `/dev/dri`、无 DRM/KMS**。
> 日期：2026-11-03。前置结论见 `docs/AUDIT-x11-kde.md` §8。

## 1. fbdev 硬件事实（`/tmp/fbtest --info`）

工具：`tools/fbtest.cpp`（`g++ -O2 -o /tmp/fbtest tools/fbtest.cpp`）

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

**结论**：carveout 4 MB，一帧 3.51 MB，剩 0.48 MB → 双缓冲、`FBIOPAN_DISPLAY`、脏矩形之外的任何双缓冲技巧都不可行。
撕裂只能靠"少画"来压。

## 2. 写入通路（`/tmp/fbtest --paint`）

mmap 后画四角 24×24 白块 + 正中红十字，再读回校验：

```
左上角  (0,0)        want=ffffff got=ffffff OK
右上角  (719,0)      want=ffffff got=ffffff OK
左下角  (0,1279)     want=ffffff got=ffffff OK
右下角  (719,1279)   want=ffffff got=ffffff OK
正中    (360,640)    want=ff0000 got=ff0000 OK
角外底色 (690,100)   want=202020 got=202020 OK
```

**结论**：mmap + 直接 memcpy 通路正常，坐标系是 `720 宽 × 1280 高`（竖屏），stride 2880 必须按 `line_length` 算，不能假设 `xres*4`
（本机恰好相等，但没有保证）。

## 3. 旋转测试（`/tmp/fbtest --rotate 1 --restore`）

```
rotate=1 写入成功；回读 rotate=1  xres/yres=720x1280  virtual=720x1280
已还原 rotate=0
```

**结论**：

- `FBIOPUT_VSCREENINFO` 接受 `rotate=1`，**不报错**
- 但 `xres/yres/virtual` 一个都没变（真实的 fb 旋转驱动会把宽高交换成 `1280x720`）
- → `tegra_fb` **只把这个字段存下来，并不做变换**。别指望驱动帮你转，**必须软件旋转**

**未验证**：X 正占用显示控制器，所以无法确认"设了 rotate 之后屏幕上到底有没有变化"。要真验必须切到没有合成器的 VT 再看，
但 `fbcon=map:9` 让 TTY 没有文字，屏幕上只有你自己画的像素，验起来得靠"画个上下不对称的图案 + 人眼确认"。
`--restore` 能一条命令还原，真出事重启也能恢复。

## 4. LVGL 可行性

（探针被 15:46 的意外断电中断，/tmp/lvgl-probe 已随重启消失，待重跑）

## 5. 事故：2026-11-03 15:46 意外断电（重要）

**现象**：机器自己重启了。

**证据**：

- `/var/log/kern.log` 上次 boot 最后一行 15:46:20（`joycon_serdev: Received pkt is malformed`），
  下一行直接是 15:47:02 `Booting Linux on physical CPU 0x0`
- **没有** shutdown / reboot 序列，没有 panic/Oops/thermal trip 记录 → 是**硬复位**，不是有序关机
- 电池可排除：15:41 SOC 65%、15:45 SOC 63%；重启后 `/sys/class/power_supply/battery/CAPACITY` = 51% → 现在是 56% Charging。
  没有低电关机（低电会在 3-5%）
- 温度可排除：事后台风区 36-55°C，无 thermal 阈值触碰记录

**时间相关性（头号嫌疑）**：15:40:47 我跑了 `fbtest --rotate 1 --restore`，kern.log 里留下：

```
tegradc tegradc.0: [tegra-fb] check_var from 'fbtest': xres=720 yres=1280 bpp=32
 tegradc tegradc.0: [tegra-fb] set_par from 'fbtest': xres=720 yres=1280 bpp=32   ×2
```

`set_par` = **在 X 正占用显示控制器（nvidia 驱动）时重配了 tegradc**。6 分钟后机器硬复位。
相关不等于因果，但这是当天唯一对显示控制器动手的操作。

**结论 / 规矩**：

> **有合成器（X11/KWin）占用显示时，不要对 `/dev/fb0` 做 `FBIOPUT_VSCREENINFO`。**
> 读 `FBIOGET_*`、mmap 写像素都安全（已验证），改 var **不安全**。

要验证旋转的真实视觉效果，只有两条路：

1. 切到一个没有合成器的 VT（但 `fbcon=map:9` → 屏幕上没有 shell 文字，只能画图案靠人眼确认）
2. 先停 X（`systemctl stop sddm` 或 `set-default multi-user.target`），验完再起

另外：`/tmp` 重启后被清（`/tmp/fbtest`、`/tmp/lvgl-probe` 全没了）→ 任何上机验证都不能依赖 `/tmp` 留东西，
二进制和脚本放 `tools/` 里或 `build/` 下。

## 6. 复现命令

```bash
g++ -O2 -o build/fbtest tools/fbtest.cpp   # 别放 /tmp
./build/fbtest --info        # 只读，安全
./build/fbtest --paint       # mmap 写 + 读回，安全
# ./build/fbtest --rotate 1  # 危险：见 §5，别在有 X 的时候跑
```

## 7. 有 GPU 驱动，为什么不能自己画 / 双缓冲能不能改 / Xorg 怎么做的

### 7.1 三层能力是分开的，有 GPU ≠ 有显示控制权

| 能力 | 本机状态 | 用户态能不能用 |
|---|---|---|
| GPU 执行 | `nvgpu` 模块 + `/dev/nvhost-gpu`、`/dev/nvhost-ctrl-gpu` | ✅ 能（GL 4.6 经 Xorg 可用） |
| 显存/缓冲分配 | `nvmap_alloc`、`nvmap_pp` 模块已加载 | ❌ 没有 `/dev/nvmap` 节点给用户态 |
| 显示控制器 | `/dev/tegra_dc_0`、`_1`、`_ctrl` | ⚠️ **改看 §8：ABI 是公开的 `tegra_dc_ext`，三个设备实测都能 O_RDWR 打开** |
| 标准显示通路 | **无 `/dev/dri`、无 DRM/KMS** | ❌ 没有 GEM/dmabuf/atomic commit |

分界线就在这：**CPU 把像素写进 fb 能上屏；GPU 画完的 buffer 要上屏，要么经过 nvidia 用户态驱动，
要么自己调 `tegra_dc_ext`（见 §8 —— 这条路是公开的，不是封死的）**。
没有 KMS 只意味着没有 GEM/dmabuf/atomic commit 这套标准动作。

### 7.2 所以能做 / 不能做

- ✅ CPU 渲染 → `mmap(/dev/fb0)` → 上屏（已实测通过）
- ✅ GPU 离屏渲染（EGL / EGLStream）→ 只能 `glReadPixels` 回 CPU 再 memcpy 进 fb，对终端 UI 是负优化
- ⚠️ GPU 直接扫描输出（零拷贝）：走 nvmap + `tegra_dc_ext` 理论上可行，见 §8
- ❌ Wayland / KMS / GBM / DRM master

### 7.3 双缓冲：不是"不能改"，是三个条件现在只满足一个

要翻页必须同时具备：

1. **足够的内存** —— 现在是 `tegra_fbmem=0x400000@0xf5a00000`（4 MB，一帧 3.51 MB）。缺。
2. **`yres_virtual > yres`** —— 现在 `720x1280 == mode`，得设成 `720x2560`，而设它要 `FBIOPUT_VSCREENINFO`（有合成器时危险，见 §5）。缺。
3. **驱动支持翻页** —— `/sys/class/graphics/fb0/pan` 存在且读得到 `0,0`，说明 `tegra_fb` 实现了 `fb_pan_display`。**这条有。**

改法是把 `tegra_fbmem=0x400000` 改成 `0x800000`（8 MB），但：

- 该字符串在 `/opt/switchroot/bootstack/boot.scr`，是 **uImage 格式**（magic `27051956`，头部有数据 CRC `69ed8d17`）
  → **不能原地改字符串**，CRC 会失效，必须用 `mkimage` 重新打包；本机 **没有 mkimage**（要装 `uboot-tools`）
- `/opt/switchroot/bootstack/` 在 rootfs 里只是副本，真实的 `boot.scr` 在 SD 的 FAT 分区（现未挂载，要 sudo）
- 改错 = 开不了机（hekate 能救：重新灌 bootstack）
- 代价：多占 4 MB 物理保留内存（3.9 GiB 上无所谓）

**而且即使翻页成功也不划算**：撕裂消失的代价是多一次整帧 `memcpy`（3.5 MB，A57 上约 2-4 ms），
对一个静态为主的终端 UI 是净亏。

### 7.4 结论：别做双缓冲，做脏矩形

- UI 以静态为主，一帧真正变化的面积通常 < 5% → 脏矩形只需写 100 KB 级别，撕裂窗口 < 1 ms，肉眼不可见
- 单缓冲反而省掉一次全帧拷贝
- 想要真正的 vsync：可以走 §8 的 `tegra_dc_ext` vblank 事件，但要先能接管 dc

### 7.5 Xorg 是怎么做到的（对照）

- `nvidia_drv.so` 32.3.1 是**用户态驱动**：直接开 `/dev/nvhost-gpu` 做 GPU、`/dev/tegra_dc_*` 做显示、
  用 `nvmap` 分配扫描缓冲
- 它**自己维护 front/back buffer**（不吃 fb0 那 4 MB），在 vblank 中断里换 dc 的窗口地址
  → X 天生双缓冲 + vsync，所以 KDE 不撕裂
- 合成：`kwin_x11` 用 GL 合成到一个 back buffer，Xorg 用 `Present` 扩展 flip
- 旋转：`/etc/X11/xorg.conf.d/10-monitor.conf:17` 的
  `ViewPortIn=1280x720 / ViewPortOut=1280x720 Rotation=0` 让驱动把原生 720×1280 面板转成 1280×720 桌面

一句话：**旋转、双缓冲、vsync 全都封在 nvidia 这个黑盒里**。裸机走 fbdev，等于只拿到它吐出来的一块内存当画布，
其余能力要自己补 —— 而补它们需要的接口都没公开。

## 8. 更正：switchroot 是开源的，`tegra_dc_ext` 不是私有接口

§7 里写「显示控制器只有 NVIDIA 私有 ioctl，未公开」「零拷贝封死」是**错的**。实测和公开源码证明：

### 8.1 本地证据（这台机上就能验）

- `/proc/kallsyms` 里有 **50 个 `tegra_dc_ext_*` 符号**：`tegra_dc_ext_control_ioctl`、`tegra_dc_ext_control_open`、
  `tegra_dc_ext_enable/disable`、`tegra_dc_ext_cursor_*`、`tegra_dc_ext_event_devops` …
  → 内核里编译进去了完整的 display controller extension，就是给用户态用的
- 三个设备都是 `root:video 0660`，**brejce 属于 video 组**，实测全部能 `O_RDWR` 打开：

```
/dev/tegra_dc_0      O_RDWR OK      （head0 = 内屏 DSI）
/dev/tegra_dc_ctrl   O_RDWR OK      （列连接的显示）
/dev/nvmap           O_RDWR OK      （扫描缓冲分配器，§7.1 里我说"没有 /dev/nvmap 节点"也是错的）
```

- 内核是 `theofficialgman@buildbot` 编译的 4.9.140-l4t #139（2026-05-04）→ switchroot 自己的内核
- `tegra-udrm.ko` 的 description 是 *"Kernel DRM support for user mode DRM driver on NVIDIA Tegra Soc"*
  —— 是给**用户态 DRM 驱动**用的辅助模块，不是 KMS

### 8.2 源码是公开的

| 东西 | 地址 | 开放性 |
|---|---|---|
| switchroot 内核（含 tegra dc 扩展、Joy-Con 驱动、max17042 电池） | `gitlab.com/switchroot/kernel/l4t-kernel-4.9`、`github.com/CTCaer/switch-l4t-kernel-4.9`（分支 `linux-dev`） | 开源 |
| `tegra_dc_ext.h`（ioctl ABI 定义） | `include/video/tegra_dc_ext.h` / `include/uapi/video/tegra_dc_ext.h` | 开源，含 `TEGRA_DC_EXT_FLIP*`、`TEGRA_DC_EXT_SET_NVMAP_FD`、vblank 事件 |
| nvmap（扫描缓冲分配器） | 同仓库 `drivers/video/tegra/nvmap/` | 开源的**内核**部分 |
| `nvidia_drv.so` / `libGLX_nvidia.so` / EGL 用户态 | — | **闭源 blob**（这才是真正封闭的那块） |

**结论：不需要反推。** 想做的事（自绘 + 双缓冲 + vsync + 多窗口 overlay）用的就是 `tegra_dc_ext`，
它的头文件可以直接从上面仓库下载；nvidia 的 X 驱动用的也是这一套 ioctl，只是它没公开用法示例。

### 8.3 自己画 + 双缓冲的真实代价

要做的事（按依赖顺序）：

1. 从 L4T 源码取 `tegra_dc_ext.h`（+ nvmap 的 ioctl 结构体）
2. `open("/dev/tegra_dc_ctrl")` → `TEGRA_DC_EXT_CONTROL_GET_OUTPUT_PROPERTIES` 找到内屏
3. `open("/dev/tegra_dc_0")` → `TEGRA_DC_EXT_GET_CAPS` / `TEGRA_DC_EXT_GET_WINDOWS` 看可用窗口数（Tegra X1 是 A/B/C + cursor）
4. `open("/dev/nvmap")` + `nvmap_ioctl(NVMAP_IOC_CREATE / ALLOC)` 分配两块 1280×720×4 = 3.5 MB 的扫描缓冲
   —— **不占 fb0 那 4 MB carveout**，走的是系统内存，所以"双缓冲装不下"这个问题在这里不存在
5. 每个窗口 `TEGRA_DC_EXT_SET_NVMAP_FD` 绑定 + 设格式/位置/layering
6. `TEGRA_DC_EXT_FLIP`（或 FLIP4）翻页；`tegra_dc_ext_event_devops` 提供 `TEGRA_DC_EXT_EVENT_VBLANK`，`poll()` 等它 → **真 vsync**
7. 旋转：可以在 dc 窗口上做（L4T 的 T21x 不支持 HW 旋转，通常软件转），或者干脆按 720×1280 竖屏排版

**没变的前提**：X 正在占用同一个 dc（`tegra_dc_ext_enable/disable` 是排他的）。所以这套只能在
X 停掉、跑裸 TTY 的时候用。要验证就得先 `systemctl stop sddm` —— 这也是为什么必须先配好 SSH。

### 8.4 三条路重新评级

| 路线 | 以前评的 | 现在 |
|---|---|---|
| fbdev + 脏矩形 | 主推 | 仍然是最省的路，**但它是"只能单缓冲 + 撕裂"的妥协** |
| `tegra_dc_ext` 自绘 + nvmap 双缓冲 + vblank | 判为"不可能" | **可行**，成本是几天（一个 mini-DRM，没人给示例）。适合以后要认真做 UI 的时候 |
| Wayland / KMS | 不可能 | 仍然不可能（缺 KMS 驱动） |

建议顺序不变：先上 fbdev + 脏矩形把功能跑通；等终端本身能用了，再把显示层换成 `tegra_dc_ext`
（接口是同一块屏幕，换掉 `display_fbdev.cpp` 的实现即可，上层不用动）。
