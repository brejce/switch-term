# Switchroot 上机手册（Noble 24.04 实测流程）

> 目标：Switch 到手后从零到「跑起本程序」。本文只写这台机器上验证过的路径，
> 未验证的地方标了 **(待验证)**。

## 0. 镜像（已下载并校验）

| 项 | 值 |
|---|---|
| 文件 | `~/Downloads/switchroot/theofficialgman-kubuntu-noble-5.1.2-2026-05-13.7z` |
| 大小 | 2,374,622,000 B（2.37 GB） |
| SHA1 | `66e6d4990df4d671e64615b7ec6276299fdebc21`（与上游 `hashes.txt` 一致） |
| 已解压 | `~/Downloads/switchroot/noble/`（6.7 GB） |

上游只提供 **SHA1**，没有 sha256。复查：

```fish
cd ~/Downloads/switchroot
sha1sum theofficialgman-kubuntu-noble-5.1.2-2026-05-13.7z
```

> `ubuntu-jammy-5.1.2-...7z.partial` 是没下完的 jammy，**不要用**（改主意时中断的）。

## 1. 准备

- hekate **≥ 6.0.6**（v5.1.2 硬性要求，先升级）
- SD 卡 ≥ 16 GB（推荐 128 GB U3/A2）
- FAT32 分区要留 **≥ 8 GiB**（解压后 6.6 GiB）
- **分区会清空整张卡**：先备份 FAT32 文件 + `Tools → Backup eMMC`

## 2. 关键认知：这不是 `.img`

```
switchroot/install/l4t.00   3.99 GiB   裸 ext4 分区第 1 段
switchroot/install/l4t.01   2.59 GiB   裸 ext4 分区第 2 段
```

**不要 `dd`、不要 balenaEtcher。** 正确做法是解压到 SD 卡 FAT32 根目录，
由 hekate 的 `Flash Linux` 写进 Linux 分区。

## 3. 灌卡

1. hekate → `Tools → USB Tools`，把 SD 卡当 U 盘挂到 PC（等价：`echo ums > /sys/devices/r2p/action`）
2. 解压到 **FAT32 根目录**（不要多套一层文件夹）：

   ```fish
   # device 换成实际的 SD 卡挂载点，先 mount 确认
   set CARD /run/media/(whoami)/(SD卡卷标)
   7z x -y -o$CARD ~/Downloads/switchroot/theofficialgman-kubuntu-noble-5.1.2-2026-05-13.7z
   sync
   ```

3. 解压后卡根应该是：`bootloader/ini/L4T-noble.ini`、`switchroot/install/l4t.0{0,1}`、
   `switchroot/ubuntu-noble/*`
4. **安全弹出**（`sync` + `umount`），否则安装文件会损坏

## 4. hekate 分区 + 写入

1. `Tools → Partition SD Card`：建 GPT，FAT32 留 ≥ 8 GiB，剩下的给 Linux
2. `Tools → Partition SD Card → Flash Linux`：选 FAT32 上的 `l4t.00/01`
   （官方流程里已没有 `gptrestore`，别去找）
3. **首次启动前**：`hekate → Nyx Options → Dump Joy-Con BT`
   （要求两只 Joy-Con 已插在滑轨上、在 HOS 里配对过；Switch Lite 也必须做，顺带 dump 出厂校准）
4. `More Configs` → `[L4T Ubuntu Noble]`（`id=SWR-NOB`，别和 jammy 的 `SWR-UBU` 混）

默认账号 `ubuntu` / `ubuntu`（首次登录会要求改密码）**(待验证)**。

## 5. 启动参数（`bootloader/ini/L4T-noble.ini` 追加，按需）

```ini
[L4T Ubuntu Noble]
l4t=1
boot_prefixes=/switchroot/ubuntu-noble/
id=SWR-NOB
r2p_action=self
icon=switchroot/ubuntu-noble/icon_ubuntu_hue.bmp
logopath=switchroot/ubuntu-noble/bootlogo_ubuntu.bmp
```

| 键 | 默认 | 说明 |
|---|---|---|
| `jc_rail_disable` | **0** | **0 = 滑轨直连 Joy-Con 可用**（我们要的就是这个） |
| `fbconsole` | 9（关） | 想在内核日志里排障改成 `0`（内置屏） |
| `rootdev` | `mmcblk0p2` | 根分区，一般不用改 |
| `rootlabel_retries` | 1 | 改了分区表才动 |
| `uart_port` / `earlycon` | 0 | 有 UART 硬件才用 |

## 6. 精简桌面（Kubuntu Noble → 纯 TTY）

**先做这一步，验证没事再删包。**

```bash
sudo systemctl set-default multi-user.target
sudo systemctl disable --now sddm
sudo reboot
```

确认能进 TTY、能跑我们的程序之后再：

```bash
dpkg -l | grep -E 'l4t|switchroot|tegra'    # 先记下这些，绝对不能删
sudo apt purge -y kubuntu-desktop 'plasma-*' 'kwin*' 'kde-*' sddm \
    xdg-desktop-portal-kde discover snapd flatpak
sudo apt autoremove --purge -y
sudo apt clean
```

绝对不能删：`nvidia-l4t-*`、`linux-nvidia-tegra*`、`switchroot-*`、`bluez`、`systemd`、`udev`、
内核模块 `hid-nintendo`（删了就读不到手柄）。

## 7. 编译本程序（Switch 上原生编译，非交叉）

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config \
    libfreetype-dev libfontconfig1-dev libpng-dev \
    libvterm-dev liblua5.4-dev libsdl2-dev \
    fonts-noto-cjk
```

拷源码 + 数据过去（`data/` 必须一起，相对路径）：

```fish
rsync -av --exclude build ~/Project/switch-ime-term/ switch:~/switch-ime-term/
```

首次编译：

```bash
cd ~/switch-ime-term
cmake -B build && cmake --build build -j$(nproc)
```

字体：程序主字体是 `Maple Mono NF CN`，缺了会回退 `Noto Sans CJK SC`。
想用主字体就把 PC 上的 `~/.local/share/fonts/MapleMono-NF-CN-*.ttf` 拷到 Switch 的同一路径。

## 8. 权限（两个都卡这里）

```bash
# 1) 读手柄：/dev/input/event* 通常是 root:input 660
sudo usermod -aG input $USER        # 重新登录（或 newgrp input）才生效

# 2) 写虚拟键盘鼠标（只有用 --sink uinput 才需要）
sudo tee /etc/udev/rules.d/70-sit-input.rules >/dev/null <<'EOF'
KERNEL=="event*", SUBSYSTEM=="input", TAG+="uaccess"
KERNEL=="uinput", SUBSYSTEM=="misc", MODE="0660", GROUP="input", TAG+="uaccess"
EOF
sudo udevadm control --reload && sudo udevadm trigger
```

## 9. 上机验证顺序（照这个走）

```bash
# ① 手柄到底被认成什么（关键！插着 Joy-Con 跑）
./build/switch-ime-term --list-gamepad
#   期望看到 "Joy-Con (L)" / "Joy-Con (R)"，并确认程序选中了哪一个

# ② 不依赖显示的自检
./build/switch-ime-term --selftest
./build/switch-ime-term --audit-ime
./build/switch-ime-term --audit-t9

# ③ 显示
cat /sys/class/graphics/fb0/virtual_size      # 掌机应为 1280,720 量级
./build/switch-ime-term --backend fbdev
```

**已知待修**：左右 Joy-Con 是两个独立 input 设备，`EvdevGamepad::open()` 会把两个都打开
（`src/input/evdev_gamepad.cpp:152`），摇杆事件可能重复。真机看到设备名后再决定
「按名字选主设备」还是「扇区变化去重」。

## 10. 已知问题（来自官方 wiki / 报告）

- 连不上 WPA3/WPA2 混合模式 Wi-Fi：`nmcli dev wifi connect <SSID>`，或把路由器改 `WPA/WPA2 Personal`
- Firefox 硬解要从 Mozilla PPA 装（官方 snap 不带 L4T 补丁）
- Chromium 定制版 126 之后停更
- 升级走标准 `sudo apt update && sudo apt dist-upgrade`
