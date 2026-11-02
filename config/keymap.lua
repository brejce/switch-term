-- 按键装配：把物理输入绑到动作名，C++ 启动时转成原生表，运行期不查 Lua
-- 键名：LB LT RB RT A B X Y DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT
-- 动作名见 src/config/lua_config.cpp 的 kActionNames

-- 左手：LB 魔法键（网格 ↔ 弹窗 ↔ 上屏），LT 空格
keymap.bind("LB", "magic")
keymap.bind("LT", "space")

-- 十字键：单按一次性修饰，双击锁定
keymap.bind("DPAD_UP", "tab")
keymap.bind("DPAD_LEFT", "mod_ctrl")
keymap.bind("DPAD_DOWN", "mod_alt")
keymap.bind("DPAD_RIGHT", "mod_shift")
keymap.bind_double("DPAD_LEFT", "lock_ctrl")
keymap.bind_double("DPAD_DOWN", "lock_alt")
keymap.bind_double("DPAD_RIGHT", "lock_shift")

-- 右手：ABXY
keymap.bind("A", "enter")
keymap.bind("B", "backspace")
keymap.bind("X", "delete")
keymap.bind("Y", "escape")

-- 右手肩键＝鼠标左右键（uinput 点击，不移动指针）；右摇杆移动未实现
keymap.bind("RB", "mouse_left")
keymap.bind("RT", "mouse_right")
keymap.bind_axis("STICK_R", "mouse_move")
