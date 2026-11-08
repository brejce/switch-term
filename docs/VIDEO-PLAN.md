# 宣传视频拍摄计划（30s 版）

> 成片目标：**30 秒**，1920×1080@60fps，**合成 Switch 外壳**，按键有按动动效，**无配乐**。
> 采集对象：真机 `brejce@192.168.3.193`（Switch，`switch-ime-term --backend fbdev`），屏幕 720×1280 → 转置 1280×720。
> 观众：GitHub/酷安/B 站上「还能在 Switch 上玩终端？」的人。不承诺未实现功能（触摸回看、剪贴板、fcitx5、DRM）。

## 1. 成片规格

| 项 | 值 |
|---|---|
| 时长 / 画幅 | 30.0s / 1920×1080 |
| 帧率 | 60fps（素材原生 60fps） |
| 编码 | H.264 yuv420p, CRF 18, `+faststart` |
| 声音 | 无（后续可由用户加乐） |
| 字幕 | 中文硬字幕（字幕条），另出同名 `.srt` |
| 封面 | 1920×1080 PNG，标题卡版式 |
| 交付 | `video/out/promo-30s.mp4`、`.srt`、`cover.png` |

## 2. 视觉方案

真机 1280×720 画面**透视贴进一张整机渲染图**的屏幕里（早期矢量外壳已废弃，太假）。

- 外壳源图：`video/assets/stage-src.png`（800×480，荧光黄绿 Joy-Con，白底）。
  `video/assets/compose_photo.py` 做：白底抠图（边缘 flood fill + 1px 蚀边 + 反预乘）→
  设备放大到 1720px 宽贴到 1920×1080 画布 `(100,196)` → 录屏按 `PERSPECTIVE` 贴进屏幕四角
  `(467,285) (1447,285) (1447,836) (467,836)`。
- 背景：深蓝灰竖向渐变 `#1E2534 → #06080C` + 设备环境光 + 地面阴影 + 暗角。
- 按键动效：evdev 抓真实按键时间轴（`video/assets/evlog.py`），`t_video = t_event − first_frame_wall`，
  每个按下在外壳**对应键位**叠 190ms 白环 + 辉光；肩键（LB/RB/ZL/ZR）正面看不见，
  改在机身上沿打一个发光键名胶囊。
- 字幕条：底部圆角半透明黑条，Noto Sans CJK SC Medium 44px，单行。
- 已知局限：源图只有 800×480，屏幕内文字放大后偏软；要更锐得等更高分辨率的外壳图。

## 3. 分镜（30s）

| # | 时间 | 画面 | 拍摄 take | 字幕 |
|---|---|---|---|---|
| 1 | 0.0–3.0 | 标题卡：黑底 + `sit` 字标 + 副标题，缩放像呼吸 | 无（静帧） | 「用手柄打字的 Linux 终端」 |
| 2 | 3.0–6.0 | 摇杆扫键：白块在字母区滑动、相机跟动 | **缺**（t13-en 前段是静止画面，待补拍） | 「左右摇杆 = 引力指针」 |
| 3 | 6.0–12.0 | 英文 `echo hi`：逐个选键 + LB 上屏 + ZL 空格（1.6x 加速） | `t13-en` 63.6–77.4 | 「LB 上屏 · ZL 空格 · A 回车」 |
| 4 | 12.0–14.0 | 回车后 `hi` 输出 | `t13-en` 77.6–79.2 | 「真的在跑 shell」 |
| 5 | 14.0–24.0 | 中文：`echo ` + 切中文 + `nihao` + 候选 + ZL 上屏「你好」+ 回车（1.4x） | `t21-zh` 32.6–42.2 | 「内置拼音 IME · 不装 fcitx5」 |
| 6 | 24.0–27.0 | 特性卡：三行数字（常驻 ~330MB / 无桌面 / 13s 开机） | 无（静态图文 + 外壳小图） | — |
| 7 | 27.0–30.0 | 结尾卡：`sit` + 一行说明 + `Made on a Nintendo Switch` | 无（静帧） | — |

转场：2/3/4/5 段之间用 0.25s 交叉淡入（xfade）；标题卡与结尾卡用 0.5s。

## 4. 拍摄流程

1. **复位**：`sudo systemctl restart switch-ime-term`（清屏、回到默认状态）。
2. **起录**（设备上）：
   ```sh
   cd /home/brejce/video/cap && sh cap.sh t13-en 150
   ```
   产出 `/home/brejce/video/raw/t10-en.{mp4,events,log,first}`：
   - `-use_wallclock_as_timestamps 1` + `-debug_ts` → 日志里首帧 `pkt_pts` 即 `first`（epoch 秒），
     后期用它把 evdev 事件精确对到帧。
   - 45s 窗口是给「用户看到口令才开始」留的余量；成片只用其中 5–10s。
3. **口令**：我在聊天里给编号动作，用户看到即开始，动作慢一点无所谓。
4. **停机**：take 结束 `pkill -f cap[.]sh`（**不要**用 `pkill -f fbdev`：会误杀主程序）。
5. **验片**：抽 3 帧看方向/亮度/有无卡顿；`ffprobe` 查帧数。
6. **不合格重拍**，编号 `t11-...`。

### 已踩过的坑（务必遵守）
- 主程序被杀后服务不会自动重启，屏幕冻在最后一帧，手柄表现为「无反应」→ 用
  `sudo systemctl start switch-ime-term` 恢复。**清理进程只按 PID 或 `cap[.]sh`。**
- ffmpeg 启动有 ~0.26s 延迟，`-t N` 实际只录到 `N−0.26`s；拍 45s 用 6–10s 完全够。
- 60fps + nvv4l2 硬件编码时，时间轴是均匀 CFR，浏览无抖动；若某条动作抖动，降 45/30fps 重拍。

## 5. 后期流水线（全脚本，可重跑）

```sh
# 0) 取回素材
rsync -e "ssh -F /dev/null -o BatchMode=yes" -av \
  brejce@192.168.3.193:/home/brejce/video/raw/ video/raw/

# 1) 静态合成图（确认风格用）
python3 video/assets/compose_photo.py          # → video/work/still-*.png

# 2) 一段实拍 → 合成视频（透视贴屏 + 按键高亮 + 字幕）
python3 video/assets/render_photo.py --take video/raw/t13-en.mp4 \
  --events video/raw/t13-en.events --first 1794042508.527788 \
  --in 66.6 --out 70.6 --speed 1.0 --caption "LB 上屏：手柄就是键盘" \
  --outfile video/work/test_photo.mp4

# 3) 剪辑：按分镜切点 trim / setpts 变速 / xfade
# 4) 出片：libx264 CRF18 +faststart，另用同一切点生成 .srt
```

产物：
- `video/work/` 中间文件（每条 take 的合成版）；`video/out/stills/` 静态合成图
- `video/raw/`、`video/work/`、`video/out/` 已在 `.gitignore`（只入库脚本与素材图）
- `video/out/promo-30s.mp4` / `.srt` / `cover.png`

## 6. 风险与对策

| 风险 | 对策 |
|---|---|
| 手柄按键慢、节奏拖沓 | 后期 1.4–1.6x 提速；只留 4–6 个键的真实速度，其余快放 |
| 中文候选条一闪而过 | 让用户在候选出现后停 1.5s 再按 ZL |
| 屏幕撕裂/残影（fbdev 单缓冲） | 选动作小的片段；必要时后期加轻微运动模糊掩盖 |
| 660 行终端字太小 | 外壳画面 1200px 宽已够；必要时对终端区做 1.3x 局部推镜 |
| 拍摄中误杀主程序 | 只用 `pkill -f cap[.]sh` 或精确 PID；拍完 `systemctl is-active` 自检 |

## 7. 时间预算

- 拍摄：3 条 take + 1 条重拍 ≈ 15 分钟
- 合成与动效：脚本跑 ≈ 10 分钟（人盯 1 次）
- 剪辑/字幕/出片：≈ 30 分钟

## 8. 当前进度（2026-11-07）

- ✅ 素材：`t13-en`（英文）、`t21-zh`（中文候选条 → 上屏「你好」）已实拍并核验。
- ✅ 合成管线：`compose_photo.py`（静帧）+ `render_photo.py`（逐帧视频），已出 5 张静态合成图
  `video/out/stills/` 与 4s 试片 `video/out/preview-4s.mp4`。
- ⬜ 待补拍：摇杆扫键镜头（t13-en 该段是静止画面，t22-stick 那次用户没操作）。
- ⬜ 待做：三张卡（标题/数字/结尾）、按分镜拼 30s、`.srt`、封面。

## 9. 最终交付（2026-11-08，已改为 halftone-dossier 风格）

早期「真实录屏 + 3D 外壳 + 按键发光」方案已废弃，最终成片改为**复古半调案卷**风格：
真机画面印成半调版画、纸面排版、红章判决，30.0s / 1920×1080 / 30fps / 中文旁白 + 印刷音效（无配乐）。

| 产物 | 路径 |
|---|---|
| 成片（含旁白） | `video/film/out/sit-001-final.mp4` |
| 无声母版 | `video/film/out/sit-001.mp4` |
| 封面 | `video/film/out/poster.jpg` |
| 字幕 | `video/film/out/sit-001.srt` |
| 分镜与声音设计 | `video/film/TREATMENT.md` |
| 素材与许可 | `video/film/CREDITS.md` |
| 页面源码（可重渲） | `video/film/index.html` + `tools/plates.py` + `build_audio.py` |

一键重做：`node core/render/video.mjs video/film --fps 30 --workers 4 --out video/film/out/sit-001.mp4`
→ `python build_audio.py` → `ffmpeg` 混流（loudnorm I=-14）。
