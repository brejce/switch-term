# Switch-Term

**一台握在手里的终端。** Framebuffer 全自绘界面的手柄键盘终端：108 键全部映射到两支手柄上，
手不用离开握把，就能在 `pi-coding-agent`、`nano`、`vim` 等任何终端应用里完成全部操作——
打字、切窗、起 shell，都不需要摸键盘。

## 是什么

- **没有桌面**：不开 Xorg/Wayland，直接写 `/dev/fb0`（Nintendo Switch 2017 上实测跑通）。
- **全键盘**：真实 108 键的行列，压进左右各 20 键、ALPHA / NUM 两层；Shift 单击上档一次、双击锁定。
- **手柄即输入**：左右摇杆做带吸附与震动的引力指针，肩键 / 十字 / ABXY 各有分工，眼睛不必看键区。
- **真终端**：libvterm + PTY，`vim`、`tmux`、`htop`、`pi-coding-agent` 照常跑。
- **中文输入**：拼音组字，候选条贴在终端底部。

## 构建

依赖：CMake ≥ 3.16、C++17、freetype2、fontconfig、libpng、libvterm、lua5.4（完整列表见 `CMakeLists.txt`）。

```sh
cmake -B build
cmake --build build -j4
./build/switch-ime-term
```

## 文档

接手先读 [`docs/MANUAL.md`](docs/MANUAL.md)（产品形态 / 操作表 / 配置 / 上机与排障），
其余索引见 [`docs/README.md`](docs/README.md)。
