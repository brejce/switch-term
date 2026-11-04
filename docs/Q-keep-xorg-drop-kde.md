# Q：保留 Xorg、去掉 KDE，能不能省内存？要不要卸载 kubuntu

> 命名规则：一个文件回答一个问题。前置：`docs/AUDIT-x11-kde.md`、`docs/Q-minidrm-vs-other-desktops.md`。

## 一句话答案

**能省，而且是这台机上最划算的一步（约 1.2 GB）。但「卸载」和「省内存」是两件事** ——
不启动 KDE 就已经省下了，卸载只多省磁盘、还带来风险。

## 1. 现在的账（2026-11-03 实测）

```
Mem: total 3991 MB   used 1963 MB   free 611 MB   buff/cache 1629 MB   available 2028 MB
```

KDE 桌面栈 RSS 合计 **1756 MB**（RSS 会重复计共享库，去重后实际影响约 1.1–1.3 GB）：

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

**没必要留着的部分**：KDE 桌面全栈（上表）、onboard（我们自己就是键盘 UI）。
**要留着**：Xorg + `nvidia_drv.so`（属于 `nvidia-l4t-3d-core`，L4T 包，跟 KDE 无关）+ libinput。

## 2. 为什么「卸载 kubuntu」不划算

```
$ apt-get -s purge kubuntu-desktop
下列软件包将被【卸载】：kubuntu-desktop*        ← 只删 1 个包
```

`kubuntu-desktop` 是 **meta 包**，purge 它不会删掉 plasma-desktop / kwin / kded5 那 273 个真包。
要真卸载得自己列一长串（`purge plasma-desktop kwin-x11 kded5 ...`），风险：

- 依赖链一旦断到 `xserver-xorg-core`（本机是 **focal 时代的 1.20.13**，不是 noble 的版本），修回来很痛苦
- 我们的 `nvidia_drv.so` 来自 L4T 仓库，不在 Ubuntu 依赖图里，误删后只能重灌包
- 收益只有磁盘（几条 GB），**内存收益为零** —— 因为不启动就不占内存

## 3. 推荐方案 A：不卸载，只不启动（可随时回滚）

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

**没有 WM 的注意点**

- 窗口没边框、不能拖动 —— 对我们全屏单窗口正好
- X 默认 focus 策略是 `PointerRoot`：鼠标指针在窗口上，键盘就给这个窗口。全屏下等于总是有焦点
- 需要程序主动抢焦点时再装 `xdotool`（现在未装，很小）；也可以在 `~/.xinitrc` 里
  `exec switch-ime-term` 之前什么都不做，SDL 自己会 `SDL_SetWindowInputFocus`
- 程序崩了屏幕上什么都没有（`fbcon=map=9` → 控制台无文字）。建议 `~/.xinitrc` 写成
  `exec switch-ime-term || exec sh` 或者在 X 里留一个 `xterm`（需装 `xterm`，几 MB）

## 4. 预期收益

| 场景 | used | available | 说明 |
|---|---|---|---|
| 现在（KDE 全跑） | 1963 MB | 2028 MB | |
| 只关 sddm + 不跑 KDE，裸 X + 我们的程序 | **约 700–900 MB** | **约 3.0 GB** | Xorg 67 MB + nvidia GL + 系统服务 |
| 再把 onedrive/packagekit/discover 之类服务 mask 掉 | 再省几十 MB | | |

省下来的主要是那 1756 MB 的 RSS 里能真正回收的部分（plasmashell + kded5 + kwin 三项就 764 MB）。

## 5. 前置条件与风险（不要跳）

1. **先把 SSH 配通**。切到 `multi-user.target` 之后，X 起不来 = 屏幕黑 + 没有控制台文字
   （`fbcon=map:9`），只能靠 SSH 进来改，或者拔卡/重启进 hekate。
2. **konsole 会一起消失** —— KDE 会话里的终端没了。所以切之前确认：我们的程序能起、或者系统里有
   `xterm`/`fbterm` 之类兜底。
3. **onboard 屏幕键盘不会自动起**（它靠 KDE 会话 autostart）。我们自己的 UI 就是键盘，
   但如果还想用触摸打英文，得自己往 `~/.xinitrc` 里加 `onboard &`（包还在，不卸载就能起）。
4. **kwallet/polkit/portal 掉了不影响我们**：SDL + evdev + fbdev 都不依赖它们。
5. 想彻底不加载 KDE 的开机服务，可以额外 mask：
   `sudo systemctl mask packagekit.service`（它一直在刷 get-updates，还常驻 65 MB 级的 packagekitd）

## 6. 如果以后真要卸载（方案 B）

等系统在裸 X 下稳定跑几天、SSH 通了、`docs/SWITCHROOT.md` 的备份步骤做过之后再说，顺序：

1. 停 sddm，确认裸 X 正常
2. `apt-mark showauto` 里找出 KDE 侧包，先 `apt purge` 验证机（一次删一小组，每次重启验证）
3. **永远不要**一次 purge 包含 `xserver-xorg-core` / `nvidia-l4t-*` / `xserver-xorg-input-libinput` 的依赖闭包
4. 收益：磁盘几 GB；内存收益 0（已经通过"不启动"拿到了）

## 7. 建议

- 现在只做方案 A 的第 1 步里的 `systemctl stop sddm`？**先别**，等 SSH 通。
- 在此之前可以零风险做一件事：**关掉 kded5 那个 274 MB 的异常占用**，单独看它涨不涨（重启 kded5 试试，
  只影响 KDE 功能，不影响数据）。这是"先赚一点"的实验，但收益远小于方案 A。
