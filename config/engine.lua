-- 引擎装配：这里只放「看不见」的参数（终端/输入法/输入设备/日志/输出口）。
-- 看得见的东西（尺寸/键表/标签/字体/主题）在 ui.lua 和 keymap.lua，两类文件分开改。
-- C++ 只读这张表；缺键 = 用 src/config/lua_config.h 里 Engine 的默认值。

engine = {}

engine.terminal = {
    scrollback = 2000,   -- 终端向上保留多少行历史（--legacy 的滚动回放也读它）
}

engine.ime = {
    cand_per_page = 7,   -- 底部候选条一页显示几个（IME 内部翻页跨度也跟着变）
    ngram         = true, -- 候选要不要用 bigram 上下文重排（目前只解析，未接线）
    t9            = true, -- 九键数字预测输入开关（目前只解析，未接线）
}

engine.input = {
    deadzone       = 0.35, -- 摇杆死区：归一化后小于它的位移当作回中
    hysteresis_deg = 8,    -- 扇区切换滞回角：防止在边界上来回抖出字母
    double_tap_ms  = 400,  -- 同一个键在这个时间内按第二下 = 双击（十字键锁定修饰）
}

engine.log = {
    level = "warn",      -- 日志级别：debug | info | warn | error（目前只打印，未接线）
    path  = "",          -- 日志文件路径，空 = 只写 stderr
}

engine.sink = "pty"       -- 输出口：pty（自带 PTY）| uinput（打到前台程序）
