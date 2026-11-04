-- 按键装配：把物理输入绑到动作名，C++ 启动时转成原生表，运行期不查 Lua
-- 键名：LB LT RB RT A B X Y DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT L3
-- 动作名见 src/config/lua_config.cpp 的 kActionNames

-- 左手：LB 上屏（物理场吸附的键），LT 空格，摇杆按下 = 层循环（ALPHA→NUM→SYM）
keymap.bind("LB", "magic")
keymap.bind("LT", "space")
keymap.bind("L3", "layer_next")

-- 十字键：单按一次性修饰，双击锁定
keymap.bind("DPAD_UP", "tab")
keymap.bind("DPAD_LEFT", "mod_ctrl")
keymap.bind("DPAD_DOWN", "mod_alt")
keymap.bind("DPAD_RIGHT", "mod_shift")
keymap.bind_double("DPAD_LEFT", "lock_ctrl")
keymap.bind_double("DPAD_DOWN", "lock_alt")
keymap.bind_double("DPAD_RIGHT", "lock_shift")

-- 右手柄 +：拉起 onboard 屏幕键盘。桌面模式下 KDE 可能先抢走手柄，这颗是兜底
keymap.bind("START", "onboard")

-- 右手：ABXY
keymap.bind("A", "enter")
keymap.bind("B", "backspace")
keymap.bind("X", "delete")
keymap.bind("Y", "escape")

-- 右手肩键：R（RB）= 左，ZR（RT）= 右；锁了 Shift 就变成 上 / 下（手柄没别的键发上下箭头）。
-- 组字出候选时这两颗不移动光标，而是移动候选条里的选中项（选中项由空格上屏）。
keymap.bind("RB", "cursor_left")
keymap.bind("RT", "cursor_right")
keymap.bind_axis("STICK_R", "mouse_move")

-- 多 tab：终端里敲 `term`／`quit` 建/关，键位待定所以先不绑。
-- 考虑过 Ctrl+N，但 0x0e 是 readline 的「历史下一条」，会和 shell 抢键。
-- 方案定了只改这几行（Action 枚举与 doAction 分支都在，放开就能用）：
-- keymap.bind_double("DPAD_UP", "next_tab")
-- keymap.bind_double("DPAD_DOWN", "new_tab")
-- keymap.bind_double("LT", "prev_tab")
-- keymap.bind_double("RB", "close_tab")
