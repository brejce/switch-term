# MANUAL · 进行手册（接手/换会话先读这一份）

> 最后更新：2026-11-04。分支 `feat/keys-108`（HEAD `5b06da9`）。
> 这份只写「现在是什么、怎么上机、接下来做什么」。原理与历史见 `ARCH.md` / `PHYSICS.md` /
> `DESIGN.md` / `LAYOUT-v2.md` / `KEYMAP-108.md` / `DEVICE.md`（DEVICE 是机器实测事实）。

## 1. 现在的产品形态

Switch 2017 上跑的**纯字符终端 + 手柄键盘**，不开 Xorg，直接写 `/dev/fb0`。
屏幕三分区：左键区（ALPHA/NUM 两层，20 键）｜中间终端（libvterm + PTY）｜右键区（20 键），
顶栏 tab/时钟/电量，底栏层/语言/输入法。

- **键区**：每手 5 列 × 4 行 = 20 键，真实 108 键盘的行列。
  ALPHA：`1-5 / qwert / asdfg / zxcvb`（左）+ `6-0 / yuiop / hjkl; / nm,./`（右）。
  NUM：右手 `- = [ ] \ '`、左手 `、【】「」`，两行居中在 home 行附近。
- **上档**：Shift 单击 = 下一键上档一次；双击 = 锁定持续上档（字母即大写）。
  字母标签显示大写，副字只画上档真的有字符的键（数字行、`;:` `,<` `.>` `/?`）。
- **中文态**：拼音组字，候选条在终端底部；ASCII 标点自动转全角。
- **摄像机**：白块（选中键）屏幕坐标恒定，网格从下面滑过；欠阻尼弹簧 ω=34、ζ=0.45，
  视差 0.30，换键时选中键 ×1.14 再落回 ×1.09。
- **遮罩**：以准心（吸附键键心）为圆心的径向遮罩，强度 `1−exp(−d/τ)`、`τ=1.5` 格（一格 = 键距，横 58 / 纵 62），
  颜色=背景色再压 10%；`exp(−d/τ) ≤ 0.04`（≈4.8 格）的键整块不画。
  外围键按独立曲线缩小（`0.6` 格开始缩、`2.3` 格到最小，最多 −30%）。
- **震动**：左右 Joy-Con 分侧（`event6`=Left、`event7`=Right）。底噪按摇杆速度、吸附"咚"。
  **只在摇杆真被推着时震**（松手/自动回中静音）。增益 `gain_left=0.4`、`gain_right=1.0`。
  两半装在滑轨上时内核只给一个 `Combined Joy-Cons` 节点，此时走 `strong=左 / weak=右` 的双通道：
  仍然分侧，只是共用一条 FF 设备。

- **电源档 / 风扇 / 充电限制**：`sudo nvpmodel -m N`（7 档，见 `docs/DEVICE.md` §14）、
  `sudo nvpmodel -d Console|Handheld|Cool`、`nvpmodel_helper.sh 7 55`（充电上限）。
  插拔底座会自动改档（`auto_profiles=1`）；充电上限在裸机跑时会因重启丢失。

## 2. 操作表（手柄）

| 键 | 动作 |
|---|---|
| 左摇杆 / 右摇杆 | 左/右键区的引力指针（选中键 = 白块） |
| LB (L) | 上屏白块键（按 Shift 状态取上档） |
| LT (ZL) | 空格；组字中 = 上屏选中候选 |
| RB (R) / RT (ZR) | 左/右。组字中 = 候选选中项左/右。Tab 窗口内 = 切 tab。带 Shift = 上/下 |
| L3 | 层切换 ALPHA ⇄ NUM（`kLayerCount = 2`） |
| A / B / X / Y | 回车 / 退格 / Delete / Esc（面板按真机：X 上、Y 左、A 右、B 下） |
| DPAD 上 | Tab（并开 1.2s Tab 窗口） |
| DPAD 左 | Shift（按住持续 / 单击一次 / 双击锁） |
| DPAD 右 | Alt（按住持续 / 单击一次 / 双击锁） |
| DPAD 下 | Ctrl（按住持续 / 单击一次 / 双击锁）；**Ctrl+空格 = 下 + ZL = 切中英** |
| `+` | onboard 屏幕键盘（兜底） |

键盘路径（实体）：Ctrl+Space 切中英；其余标准终端输入。

## 3. 配置（全部在 `config/`，改完 `systemctl restart switch-ime-term`）

| 文件 | 管什么 |
|---|---|
| `ui.lua` | 键位字符（`layers` 下档 / `shifts` 上档）、键距/尺寸、`key_window`、`key_area_dy`、标题 |
| `keymap.lua` | 按键 → 动作 |
| `engine.lua` | `input`（死区/滞回）、`ime`、`physics`（吸附/引力/速度）、`rumble`（震动） |

热重载 `kill -USR1 $(pgrep -x switch-ime-term)` 只重读 ui/keymap/engine 的大部分；
**震动 FF 槽与 `key_area_dy` 之外的几何在启动时定死**，改了要 restart。

## 4. 上机与排障

```fish
# 本机 → Switch（Switch 的 git 仓库和本机是同一个 remote 关系）
ssh brejce@192.168.43.17          # 用户名就是 brejce，不是 ubuntu
cd ~/Project/switch-ime-term && git log --oneline -1

# 同步（本机改完）
rsync -a --exclude=.git --exclude=build --exclude='*.png' -e ssh ./ brejce@192.168.43.17:~/Project/switch-ime-term/
ssh brejce@192.168.43.17 'cd ~/Project/switch-ime-term && cmake --build build -j$(nproc) && sudo systemctl restart switch-ime-term'

# 看屏（抓 /dev/fb0 → PNG，然后 read 图片路径）
ssh brejce@192.168.43.17 'sudo dd if=/dev/fb0 bs=1M count=4 status=none | head -c 3686400 > /tmp/f.raw && convert -size 720x1280 -depth 8 bgra:/tmp/f.raw -alpha off -rotate 90 /tmp/f.png'
rsync -e ssh brejce@192.168.43.17:/tmp/f.png /tmp/f.png

# 自检（不需要屏幕）
ssh brejce@192.168.43.17 'cd ~/Project/switch-ime-term && ./build/switch-ime-term --audit-ime && ./build/switch-ime-term --audit-physics'

# 日志
ssh brejce@192.168.43.17 'sudo journalctl -u switch-ime-term -b --no-pager | grep -vE "^\[font\]" | tail -40'
```

**排障速查**
- 手柄没反应：`sudo journalctl ... | grep gamepad` 看有没有 `Combined Joy-Cons`；Joy-Con 开机后几十秒才出来，程序每 2s 重扫。
- **换手柄不用重启服务**：设备掉线（`read` 返回 `ENODEV`）后 2s 内整组重开，日志会重打一遍 `gamepad:` / `rumble:`。
- 震动不出：先看 `rumble:` 那两行挑的是哪台设备；`SIT_DEBUG=1` 时启动会主动打一发自检震动，日志里
  有 `effect id=…` 就说明驱动收了（摸不到就是硬件/马达）。
- 震动没反应：`grep rumble` 应看到两条 `rumble: /dev/input/event6/7`；有 `播放失败` 行才是驱动拒收。
- 白屏/字体怪：`fc-match "Maple Mono NF CN"` 必须返回 Maple（见 `FONTS.md`）；启动日志有 `fontconfig: 要 … 实际拿到 …` = 没装。
- 背光黑屏：`/sys/class/backlight/backlight/brightness` 应为 255（unit 的 ExecStartPre 每次拉满）。
- 电量：`/sys/class/power_supply/battery/capacity`（Switch 上 2% 以下会掉电关机，先充电）。

## 5. 分支与 tag

| 名字 | 位置 | 含义 |
|---|---|---|
| `main` | `d94d605` | 旧 30 键版（圆角定稿前） |
| `t9-checkpoint` | — | 旧 T9 时代 |
| `t10-ui` | — | 物理手感定稿 |
| `t11-ui` | `d94d605` | 字母区视觉定稿（圆角键帽、中心遮罩、外围缩小、视差摄像机、震动） |
| **`feat/keys-108`** | **`5b06da9`（当前）** | 108 键盘布局 + 两层 + 分侧震动 + 新调色板 + 外壳去彩色 |

`feat/keys-108` 两边（本机、Switch）同步一致，工作区干净。

**提交规矩**：本机是主开发区，改完 commit → `git push switch feat/keys-108`。Switch 上不要直接改代码
（它的 `git stash` 会被 rsync 干扰，推送前要先 `git stash push -u` 再 `git checkout -f`）。

## 6. 还没做 / 已知问题（按优先级）

1. **震动主观上还没确认**：`O_NONBLOCK` 修复（`4eed347`）后用户还没反馈过"有没有底噪"。
   有底噪 → 闭环；完全没有 → 驱动层拒收，改走组合设备 `event8`。
2. **`kkBg`/`line`/`text` 的新调色板**：只做了对比度基线，**未在真机上做过色感确认**（有没有夜里刺眼）。
   可能出 `switch-dim` 变体（全局 ×0.85）。
3. **ABXY 现在全中性灰**：要不要保留彩色（降饱和 40%）未定。
4. **`faint` 承载了真信息**：`1/1` 页码、`LAYER/LANG/IME` 标签仍是 faint（3.3:1）。要挪到 dim。
5. **肩键提示的横向位置写死**（`S(10)` / `S(96)`）：别的语言或字号下可能撞。
6. **标点转换只在 IME 路径**：实体键盘的 `routeKey` 不做全角转换。
7. **`《》【】` 两处都能打出**（层里有 + 转换表有），要不要删层里那份未定。
8. **`main` 分支停在旧版**：`feat/keys-108` 合并回 main 的时机未定（建议等 1、2 确认后）。
9. **摄像机/震动/遮罩参数全部硬编码**（`app.cpp` 的 ω/ζ/视差、`screen.cpp` 的 `kMaskTau` / `kShrink*` / `kPulseAmp` 等），没进配置。等设置页那轮统一做。

## 7. 设计决策（不要再来回）

- **数字行常在 + 白块纵向固定** 是几何上互斥的（720 高屏放不下窗口 ≥4.3 行）。选的是**数字行常在**：窗口 4 行，
  摄像机横向跟选中键、纵向固定在准心偏移上。换这个决策要先重新算窗口高度。
- **零位 d/k**（不是 f/j）：摇杆回中落点，掌心位置。
- **选中键不用颜色、用尺寸和白块**：颜色只留 cyan（左）/ red（右）两个饱和色，给选中/右手。
- **震动只在推着时震**：松手、惯性、自动回中一律静音。这条回归过两次，见 `app.cpp` 注释。
- **Joy-Con 串口设备权限**：`/etc/udev/rules.d/99-sit-joycon-ff.rules` + service 里 `chmod 660` 兜底。
- **字体不进仓库**，只记清单（`FONTS.md`）；Maple Mono 缺装时 fontconfig 会静默降级成比例字体。
