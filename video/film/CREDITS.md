# SIT-001 — 素材与许可

## 影像
- 终端与 IME 画面：真机录屏（Nintendo Switch / switchroot Ubuntu 24.04，`switch-ime-term --backend fbdev`，fb0 直录 1280×720@60fps），取自 `video/raw/t21-zh.mp4` 与 `t13-en.mp4`。
- 设备外观：Switch 机身渲染图（商品外观示意），仅用于技术演示的合成静帧，构图与打光为自绘；Nintendo Switch 为任天堂株式会社的商标/外观，本片无官方授权，不作为商品宣传使用。
- 版画/半调处理：本项目脚本 `tools/plates.py`、`tools/screenplate.py` 生成；页面渲染用 lemo-opuscar（LemoLab）`halftone-dossier` 风格内核。

## 字体
- Noto Serif CJK SC Black — SIL Open Font License 1.1（子集 `assets/serif900.ttf`）。
- Maple Mono NF CN — SIL Open Font License 1.1（子集 `assets/maple.ttf`、`maple-bold.ttf`）。

## 声音
- 旁白：MOSS-TTS-Nano（本地合成，voice=Junhao），文本自撰。
- 音效：`build_audio.py` 用 numpy 合成（whoosh / rustle / thump / click），无第三方素材。
- 无配乐。

## 事实与数字
- 开机 13.1 秒、常驻内存 330 MB、内核 4.9.140-l4t、0 桌面组件、1 个可执行文件：均来自本机实测（`docs/DEVICE.md`、`systemd-analyze`、`free -m`、`ps`）。
