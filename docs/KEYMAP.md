# KEYMAP · 按键映射

> 最后更新：2026-11（旧九键布局的映射表已删除）。

当前按键分工的唯一权威是 **`LAYOUT-v2.md` §5**，实际绑定在 **`config/keymap.lua`**（键名 → 动作名，
启动时装配成 `cfg::KeyMap`）。本文件不再复制一份，避免两处描述打架。

看当前绑定的办法：

```bash
./build/switch-ime-term --print-config      # 打好表的动作逐条打印
./build/switch-ime-term --audit-input       # 手柄按键/摇杆映射自检
```
