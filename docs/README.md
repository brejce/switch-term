# 文档索引

接手先读 **`MANUAL.md`**（一页纸：产品形态/操作/配置/上机/已知问题），其余按需查。

## 现行（跟代码同步维护）

| 文档 | 讲什么 | 什么时候看 |
|---|---|---|
| `MANUAL.md` | 进行手册：形态、操作表、配置、上机与排障、分支 | **先读这个** |
| `ARCH.md` | 架构：显示/输入/渲染分层、模块边界 | 改模块划分、加后端 |
| `DEVICE.md` | 目标机实测事实：手柄/触屏/显示栈/电源/充电/踩过的坑 | 任何"真机上为什么不行" |
| `LAYOUT-v2.md` | 键位布局 v2：面板 176、键 48、滑窗 3×4、tab 条 | 改 UI 几何 |
| `PHYSICS.md` | 摇杆→指针的物理场、吸附、相机、震动参数 | 调手感 |
| `KEYMAP-108.md` | 键位表（108 键映射到 40 键两层） | 改键位/加键 |
| `DESIGN.md` / `DESIGN-BRIEF.md` | 视觉设计：配色、字体、间距、状态色 | 改配色/排版 |
| `FONTS.md` | 字体选择与等宽/回退规则 | 换字体 |
| `../IMPLEMENTATION.md` | 实现总览（部分章节较旧，数字以 `DEVICE.md` 为准） | 找某个功能落在哪个文件 |

## 历史存档（一次性结论，不要再当现状用）

| 文档 | 内容 | 状态 |
|---|---|---|
| `Q-keep-xorg-drop-kde.md` | 保留 Xorg 去掉 KDE 能不能省内存 | 结论仍有效（KDE 已关，X 也关了） |
| `Q-minidrm-vs-other-desktops.md` | DRM 自建 vs 其它桌面栈 | 结论：没有 DRM，别走 |
| `Q-double-buffer-dirty-rect.md` | 双缓冲/脏矩形方案比较 | 脏矩形已落地；双缓冲仍待定 |
| `Q-switch-tray-apps.md` | Switch 托盘里的那几个工具是谁 | 已并入 `DEVICE.md` §14 |
| `PROPOSAL-physics-ui.md` | 引力 UI 的外部提案 | 构想，未采纳 |
| `PROBE-fbdev-lvgl.md` | fbdev 尺寸/旋转 + LVGL 可行性探针 | 探针结论已并入 `DEVICE.md` |
| `AUDIT-x11-kde.md` | 2026-11-03 的 X11+KDE+joycond 环境审计 | 快照，环境已变 |
| `SWITCHROOT.md` | Switchroot Noble 上机流程 | 上机步骤仍可用 |
| `new.md` | 2026-10 的旧设计稿原文 | 纯存档 |
