# 字体清单（这里记，不把字体文件塞进仓库）

字体只在一处被点名：`src/ui/theme.h` 的 `namespace fam`。改字体等于改那三行。

| 用途 | 族名 | 文件 | 拿到哪 |
|---|---|---|---|
| 终端 / 键帽 / 面板 / 状态栏（等宽） | `Maple Mono NF CN` | `MapleMono-NF-CN-Regular.ttf`、`MapleMono-NF-CN-Bold.ttf`（各 ~20MB） | `~/.local/share/fonts/` |
| 汉字回退（`FontFace::fallback`） | `Noto Sans CJK SC` | 系统包 `fonts-noto-cjk` | `/usr/share/fonts/` |

- Maple Mono 版本：**8.003**（2022 The Maple Mono Project Authors，
  <https://github.com/subframe7536/maple-font>，OFL-1.1）。
- 为什么选它：ASCII advance 0.6em、汉字恰好 2×，正好对上 8px 格宽 + 1:2 中英宽度比；
  自带 Nerd 图标字形。
- **字体文件不进 git**（体积 + 许可文件分发）。换机器/重装系统按下面装一次。

## 装（不需要 root）

```fish
mkdir -p ~/.local/share/fonts
scp MapleMono-NF-CN-{Regular,Bold}.ttf brejce@<switch>:~/.local/share/fonts/
ssh brejce@<switch> 'fc-cache -f && fc-match "Maple Mono NF CN"'
# 期望：MapleMono-NF-CN-Regular.ttf: "Maple Mono NF CN" "Regular"
```

## 校验

```fish
fc-match "Maple Mono NF CN"
```

返回任何**不是** `MapleMono-NF-CN-*` 的结果（常见是 `NotoSans-Regular.ttf: "Noto Sans"`）
就是没装好：fontconfig 对缺装族名**不报错**，静默换一个默认族，而默认族通常是**比例字体**，
落在固定 8px 的网格里就会让 `i l j t .` 前后空一截，看起来像"字距坏了"。
程序启动时也会打一行 `fontconfig: 要 'X' 实际拿到 'Y'（没装？）`。

事故经过与像素级排查过程见 `docs/DEVICE.md` §12。

## 不要拿它顶替

`Noto Sans Mono`（各发行版自带）advance 同样是 0.6em，网格能对齐，但丢了 1:2 中英宽度比
和 Nerd 图标——只能当临时凑合，别写进 `theme.h`。
