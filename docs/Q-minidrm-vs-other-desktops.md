# Q：mini-DRM 做好了，其他桌面/程序就能画了？

> 命名规则：一个文件回答一个问题。前置见 `docs/PROBE-fbdev-lvgl.md` §8、`docs/Q-double-buffer-dirty-rect.md`。

## 一句话答案

**不能。** mini-DRM 只是让**我们自己的程序**独占屏幕自己画；它不是「显示服务」，
别的桌面/程序没法把像素交给它。要让第三方合成器跑起来，缺的是**内核里的 KMS 驱动**，
不是一个用户态程序。

## 1. 三种「画」必须分开谈

| # | 谁在画 | 需要什么 | 本机 |
|---|---|---|---|
| A | **我们自己的程序**（switch-ime-term）独占屏幕 | `tegra_dc_ext` + nvmap，或 fb0 | ✅ 可行（mini-DRM 指的就是这个） |
| B | 别的程序画进**我们的画布**里 | 我们当合成器 + 实现一套协议（Wayland `wl_shm` 或自定 IPC） | ⚠️ 有限可行，见 §3 |
| C | 别的桌面（KDE/GNOME/sway/weston）**自己启动** | 内核 KMS + DRM master + EGL/GBM + dmabuf | ❌ 本机不可能，见 §2 |

mini-DRM 解决的是 A。A 做完，B 和 C 都还不会自动成立。

## 2. 为什么 C 不可能：桌面要的是一整套内核接口

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

**`tegra_dc_ext` 提供的是「一个客户端独占显示控制器」**（enable/disable 是排他的），
而 KMS 提供的是「多客户端共享显示控制器 + 资源对象 + 每客户端的 master/flip」。
两者不是同一种东西，写多少遍 mini-DRM 都补不出 KMS 语义。

要真让 KDE/sway 跑起来，得给这个 4.9 内核 port 一个 `drm/tegra` KMS 驱动（T210 display 控制器），
那是**内核工程**，不是用户态工程，且上游 drm/tegra 从来没有为这套 L4T 显示栈提供过支持。

## 3. B 这条路可行到什么程度

如果 mini-DRM 顺手做成「我们的程序 = kiosk 合成器」，别的程序**可以**画进来：

| 客户端类型 | 可行性 | 卡在哪 |
|---|---|---|
| `wl_shm` 客户端（纯 CPU 画，如 foot、部分 TUI） | ✅ 可行 | 我们要实现 Wayland 协议（或用一个不依赖 DRM 的合成器骨架） |
| `libEGL`/GBM 客户端（KDE 应用、浏览器、SDL GL） | ❌ 难 | 需要 dmabuf/GBM，而这要 DRM 节点；`tegra_dc_ext` 的 nvmap fd 不等于 GBM |
| X11 客户端 | ✅ 间接 | 必须留着现在的 Xorg + nvidia（X 独占 dc），我们只能当它的一个窗口 |

所以现实的分工是：

- **要跑别人的图形程序** → 继续用现成的 X11 + nvidia（它已经能做这件事），我们当其中一个窗口
- **要一个没有桌面的极简设备** → 我们独占屏幕，mini-DRM 自绘，不接受第三方客户端
- **想两者兼得** → 我们要么实现 wl_shm 合成器（能做但只覆盖 CPU 客户端），要么承认做不到

## 4. 本机证据清单（可复验）

```bash
ls /dev/dri                       # → 不存在
ls /proc/kallsyms | grep -c tegra_dc_ext        # → 50 个符号（有 dc 扩展）
ls /sys/class/graphics/fb0/pan                 # → 存在（fb 能 pan）
modinfo tegra-udrm | grep parm                 # → modeset=1 enable, 0 disable(default)
dpkg -L nvidia-bsp-32-3                        # → 7 个文件，只有 libdrm_nvdc 符号链接，无头文件
ls /usr/src/linux-headers-4.9.140-l4t/include/config/drm/tegra   # → 只有 udrm.h
```

## 5. 结论

- **mini-DRM = A 路线**：自己独占屏幕、自绘、双缓冲、vsync。做完只服务我们自己。
- **它不会让「其他桌面」能画**，因为别的桌面要的是内核 KMS，而本机没有、也没法在不写内核驱动的前提下有。
- 想「别人也能画」的最小可行版本：我们的程序当 kiosk 合成器，只支持 `wl_shm` 客户端；
  GL/GBM 客户端和完整桌面仍然只能走现有 X11。
- 决策顺序建议：先把 A 做扎实（终端本身能用），**不要**为了 B/C 提前引入 Wayland/kiosk 复杂度。
