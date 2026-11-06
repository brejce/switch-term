# Switchroot 上机手册（Noble 24.04 实测流程）

> 最后更新：2026-11-03。定位：**从零到跑起本程序的上机步骤**（网络/分区/镜像/构建/启动）。
> 机器实测事实（显示栈、手柄、内存、fbdev）在 `DEVICE.md`；当前进度与待验项在 `../STATUS.md`。
> 本文只写这台机器上验证过的路径，未验证的地方标了 **(待验证)**。

## 0. 部署前置：网络 + SSH（**必须在删桌面之前做完**）

删桌面后没有 GUI 配网，网络配错就只能靠 hekate 挂 SD 卡救。所以顺序是：
**先配网 → 验证 SSH → 再删桌面**。

### 0.1 配多个 WiFi，手机热点排最高优先级

```bash
nmcli dev status                  # 确认网卡名（一般是 wlan0）
nmcli dev wifi list
nmcli dev wifi connect "iPhone" password "xxxx"      # 手机热点（锚点）
nmcli dev wifi connect "家里SSID" password "yyyy"    # 常用 WiFi

# 数值越大越优先：热点永远优先，家里次之
nmcli con mod "iPhone"   connection.autoconnect-priority 100
nmcli con mod "家里SSID" connection.autoconnect-priority  10

# 关 WiFi 省电（省电模式下 SSH 延迟能到几百 ms）
sudo iw dev wlan0 set power_save off
sudo tee /etc/NetworkManager/conf.d/wifi-powersave.conf >/dev/null <<'EOF'
[connection]
wifi.powersave = 2
EOF
sudo systemctl restart NetworkManager
```

切换 WiFi 时不用删旧的：`nmcli con up "家里SSID"`，找不到就自动回落下一个 profile。

### 0.2 SSH（靠 IP 连，mDNS 不可靠）

```bash
sudo apt install -y openssh-server avahi-daemon
sudo systemctl enable --now ssh avahi-daemon
hostname
ip -4 addr show wlan0         # ← 每次换网都在 Switch 上跑这条，记下新 IP
```

PC 上：`ssh brejce@<Switch 屏幕上显示的 IP>`。
**用户名是 `brejce`，不是 `ubuntu`**（镜像文档里写的 `ubuntu@` 是旧示例，照抄会连不上）。
**只靠 IP。** mDNS（`switchroot.local`）实测解析不了，且换子网 / VLAN / 开了 AP 隔离的热点必然失效 —— 当成锦上添花，失败不要排查，直接用 IP。
配密钥：`ssh-copy-id brejce@<IP>`。密钥配一次就永久有效，之后 IP 变了也不用重配。

### 0.3 删桌面前的强制检查

```bash
apt-get -s purge kubuntu-desktop | grep -E '^(Remv|Purg)' \
  | grep -E 'network-manager|openssh|avahi|wpa|bluez'
```

**输出为空才能真删。** 有输出就先 `apt-mark manual network-manager openssh-server avahi-daemon` 保护住。

### 0.4 三条救命通道

| 通道 | 怎么做 | 适用 |
|---|---|---|
| **USB-C 转以太网**（RTL8153 芯片最稳） | 插上出 `eth0`，NM 自动 DHCP | 最可靠，建议备一个 |
| **USB 键盘 + TTY** | Ctrl+Alt+F2 进 TTY，用 `nmcli` 改网 | 不需要任何网络 |
| **hekate UMS** | 重启 → `Tools → USB Tools` 挂 SD 卡，从 PC 改文件 | 最后手段（要重启） |

### 0.5 已知坑：唤醒后可能不重连

深睡眠唤醒后 `brcmfmac`/`cyw43455` 偶发不自动重连，临时办法 `sudo systemctl restart NetworkManager`。
先不加 watchdog（担心网络抖动时被误踢），真遇到再说。

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

确认能进 TTY、**SSH 还能连**（见 §0.3 的模拟 purge），再：

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
rsync -av --exclude build ~/Project/switch-ime-term/ brejce@<Switch IP>:~/switch-ime-term/
```

首次编译：

```bash
cd ~/switch-ime-term
cmake -B build && cmake --build build -j$(nproc)
```

字体：程序主字体是 `Maple Mono NF CN`，缺了会回退 `Noto Sans CJK SC`。
想用主字体就把 PC 上的 `~/.local/share/fonts/MapleMono-NF-CN-*.ttf` 拷到 Switch 的同一路径。

## 8. 权限（两个都卡这里）

> 读手柄实际**不用**加组：`event8` 已有 `user:brejce:rw-` 的 ACL（logind uaccess），实测能直接读。
> 下面第 1 步只在遇到权限错误时再做；写 `/dev/uinput` 才是真需要规则的（`DEVICE.md` §1.1/§9.2）。

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
./build/switch-ime-term --audit-input

# ③ 显示
cat /sys/class/graphics/fb0/virtual_size      # 掌机应为 1280,720 量级
./build/switch-ime-term --backend fbdev
```

**已知待修**：左右 Joy-Con 是两个独立 input 设备，`EvdevGamepad::open()` 会把两个都打开
（`src/input/evdev_gamepad.cpp:152`），摇杆事件可能重复。真机看到设备名后再决定
「按名字选主设备」还是「扇区变化去重」。

## 10. 通过 SSH 把程序跑到 Switch 屏幕上

`--backend fbdev` 直接写 `/dev/fb0`，但 tty1 的 getty 会抢着刷光标。正确的远程启动方式：

```bash
sudo systemctl stop getty@tty1          # 让出 tty1（或 disable 永久让出）
sudo openvt -c 1 -s -w ./build/switch-ime-term --backend fbdev
#   -s = 切到该 VT，-w = 等程序退出；退出后手动 sudo systemctl start getty@tty1
```

纯验证不占屏的玩法：`--backend headless --out /tmp/f.png --run "ls"`，写成 PNG 拉回 PC 看。

## 11. 已知问题（来自官方 wiki / 报告）

- 连不上 WPA3/WPA2 混合模式 Wi-Fi：`nmcli dev wifi connect <SSID>`，或把路由器改 `WPA/WPA2 Personal`
- Firefox 硬解要从 Mozilla PPA 装（官方 snap 不带 L4T 补丁）
- Chromium 定制版 126 之后停更
- 升级走标准 `sudo apt update && sudo apt dist-upgrade`

## 12. 备用通道：PC ↔ Switch 网线直连（不需要路由器）

酒店/宿舍没有可用路由器时，用一根网线把 PC 和 Switch 直接连起来，PC 当 DHCP + NAT。

**已验证的硬件**（2026-11-02 本机实测）：

| 项 | 结果 |
|---|---|
| PC 侧网卡 | RTL8153 USB，`0bda:8153`，原生 `r8152` 驱动，免驱 |
| 接口名 | `enp7s0u1u4u4`，稳定别名 `enx6c1ff76618b3`（MAC 派生） |
| 环回测试 | 两网口直连协商 1000 Mbps，`carrier=1` |
| 酒店墙插 | 两个 RJ45 口均 `carrier=0`，物理层无信号（与 Switch 无关） |

PC 侧一键（需要 sudo）：

```bash
sudo ./tools/pc-host-switch.sh                 # 默认 enx6c1ff76618b3 → wlan0
sudo ./tools/pc-host-switch.sh eth-if wlan0    # 或显式指定
sudo ./tools/pc-host-switch.sh --down          # 收工，交回 NetworkManager
```

它会做四件事：
1. `nmcli device set $IF managed no`（防止 NM 自己在这张网卡上跑 DHCP）
2. 给 `$IF` 配 `192.168.50.1/24`
3. 起 `dnsmasq` 发 `192.168.50.10-50` 的 DHCP，网关/DNS 指向本机
4. 加 `nft` masquerade 到 `wlan0`，让 Switch 能通过 PC 的 WiFi 上网

Switch 侧：接口设 DHCP 即可；或手动静态 `192.168.50.2/24`、网关 `192.168.50.1`。

排查：租约 `/tmp/dnsmasq-switch.leases`，日志 `/tmp/dnsmasq-switch.log`。

> Switch 侧要接网卡需要 USB-C 处于 host 模式：**dock 的 USB 口**最省事，
> 或用 OTG 转接头（注意供电）。掌机模式直插 USB-C 是设备模式，网卡不会枚举。
