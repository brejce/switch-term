# Q：任务栏常驻了哪些 Switch / L4T 软件，能不能关

> 命名规则：一个文件回答一个问题。相关：`docs/Q-keep-xorg-drop-kde.md`（省内存总账）。

## 1. 托盘实测清单（`org.kde.StatusNotifierWatcher`，2026-11-03）

| 托盘项 | 真身 | RSS | 是 Switch 专属吗 |
|---|---|---|---|
| `nvpmodel` + `nvpmodel_apps` | `/usr/share/nvpmodel_indicator/nvpmodel_indicator.py`（python3） | **37 MB** | ✅ **是**（NVIDIA L4T 电源/频率档） |
| `Fcitx` | fcitx5 | 110 MB | ❌ 通用输入法 |
| `Onboard` | onboard 屏幕键盘 | 83 MB | ❌ 通用（KDE 会话 autostart 带的） |
| `Discover 通知` | `DiscoverNotifier`（KDE 更新提示） | 75 MB | ❌ 通用 |
| `KDE Daemon`（浏览器集成） | kded 的一个模块 | — | ❌ 通用 |

**结论：任务栏里真正属于 Switch/L4T 的只有 nvpmodel 那一个（但它注册了 2 个图标，所以看着像"几个"）。**
joycond（3.7 MB）不占托盘，它是 systemd 服务。

## 2. nvpmodel 是什么，为什么值得单独看

- 它管 **CPU/GPU 频率档 + 风扇档**，`/etc/nvpmodel.conf` 里的档位：

```
ID=0 Console      ← 当前就是这个
ID=1 Handheld
ID=2 OC_CPU   ID=3 OC_GPU   ID=4 OC_All
ID=5 Perf_All ID=6 Perf_OC_All
```

- `nvpmodel.service` 是 **enabled** 的，开机执行 `/usr/sbin/nvpmodel -f /etc/nvpmodel.conf`
  → 也就是说**开机默认档由 conf 决定，不依赖托盘**
- 托盘那个 python 只是个 **pkexec GUI**（`pm.set_mode(mode_id, ['pkexec'])`），用来点着切档

**实测发现（重要）**：现在是 `NV Power Mode: Console`。Console = 底座档，CPU/GPU 频率更高、风扇策略更激进。
掌机模式跑 Console 档 = 白烧电、多发热 —— 这和之前 1.5 小时掉 30% 电、以及那次硬复位都对得上。

想切掌机档（不需要托盘）：

```bash
sudo nvpmodel -q          # 看当前档
sudo nvpmodel -m 1        # 切 Handheld
sudo nvpmodel -m 0        # 切回 Console
```

（`nvpmodel -q` 会刷几条 `NVPM ERROR: ... permission` 噪声，那是读风扇档/超频节点没权限，无害。）

## 3. 关不关：三档选择

| 项 | 建议 | 关掉失去什么 | 怎么关 |
|---|---|---|---|
| **nvpmodel 托盘 GUI（37 MB）** | **推荐关**，改用命令行 | 不能点着切档；`nvpmodel -m N` 照旧，开机默认档不受影响 | 删/改 `/etc/xdg/autostart/nvpmodel_indicator.desktop`（设 `Hidden=true` 或删文件）；`pkill -f nvpmodel_indicator` |
| **Onboard（83 MB）** | 推荐关（我们自己的 UI 就是键盘） | 触摸打英文得另配屏幕键盘；想留就在 `.xinitrc` 里自己 `onboard &` | 同法改 `/etc/xdg/autostart/onboard*.desktop` |
| **Discover 通知（75 MB）** | 推荐关 | 不再提示系统更新 | 删 autostart + `systemctl mask packagekit.service` |
| **Fcitx（110 MB）** | **看你** | 别的 KDE 程序（kate/konsole）打不了中文 | 见下 |
| **浏览器集成提示** | 随手关 | 无 | kded 模块设置里关 |

关于 Fcitx 有个坑（`docs/AUDIT-x11-kde.md` §5）：我们的程序在 `display_sdl.cpp:100` 无条件
`SDL_StopTextInput()`，**根本不用 fcitx**。所以：

- 只用我们的程序 → 关掉 fcitx5，省 110 MB
- 还要在 konsole/kate 里打中文 → 留着

## 4. 顺手能清的 L4T 垃圾 autostart

`/etc/xdg/autostart/` 里还有一堆 L4T BSP 带来的、跟这台机无关的东西：

```
nvbackground.desktop(.orig)      nvbackground.sh(.orig)
nvchrome.desktop(.orig)          nvchrome.sh.orig
nvlxde-compton.desktop           nvlxde-exempt.desktop   nvlxde-exempt.sh
```

`nvlxde-*` 是 **LXDE** 时代的残留（我们跑 KDE），`nvchrome` 是 Chrome 的 GPU 参数脚本。
这些都不占内存大户（脚本跑完就退），但会拖慢登录。可以设 `Hidden=true`，不必删包。

## 5. 合计能省多少

| 动作 | 省下 |
|---|---|
| 关 nvpmodel 托盘 | 37 MB |
| 关 onboard | 83 MB |
| 关 Discover + mask packagekit | 75 MB + packagekitd（几十 MB 级） |
| 关 fcitx5 | 110 MB |
| **小计** | **≈ 300–350 MB** |

对比 `docs/Q-keep-xorg-drop-kde.md` 里「不启动 KDE」省的 1.1–1.3 GB：这里是小钱，但风险几乎为零，
可以立刻做（不涉及 X/sddm，不会把屏幕搞黑）。

## 6. 建议顺序

1. **先做零风险的那一步**：`sudo nvpmodel -m 1`（Handheld）—— 立刻降功耗/温度，不动任何服务。
   想固定成开机默认，就改 `/etc/nvpmodel.conf` 的默认档或给 `nvpmodel.service` 加 `-m 1`
2. 再把 4 个 autostart 设 `Hidden=true`（nvpmodel 托盘 / onboard / Discover），重新登录生效
3. Fcitx 留着，等你确认「不在 KDE 里打中文」之后再关
4. KDE 整体不启动留给下一步（需要先有 SSH）
