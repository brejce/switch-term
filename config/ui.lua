-- UI 装配：只在这里描述长什么样，运行期全部由 C++ 用原生结构跑
-- 尺寸单位＝设计像素（1280×720 基准），实际会按窗口乘缩放

ui = {}

ui.title      = "SYS.TERM"
ui.panel_w    = 220
ui.top_bar_h  = 32
ui.bot_bar_h  = 26

ui.stick_box  = 180      -- 旧九宫格时代的外框边长（v2 键区不用它，保留给布局缩放）
ui.dpad_cell  = 28
ui.abxy_cell  = 30
ui.shoulder_h = 20

ui.term_pad_x   = 14
ui.term_pad_top = 10
ui.term_pad_bot = 8
ui.cell_w       = 8
ui.cell_h       = 21
ui.ime_rows     = 2      -- 底部固定留给组字串/候选的行数

-- 状态栏文字
ui.labels = {
    dpad_title  = "// DPAD (MODS)",
    abxy_title  = "// ABXY",
}

-- v2 分体键盘：每手 15 键（5 列×3 行）× 3 层。docs/LAYOUT-v2.md 是字符表/分工的权威规格。
-- 坐标是**局部坐标**：世界比面板大，左右手各是一个独立的 5×3 场，两面共用这一套坐标；
-- 屏上位置由渲染算：offset = 面板中心 - 指针（指针吸附住的键正好落在面板中心）。
-- 键序 = 左手 row-major(3×5) 再接右手 row-major(3×5)
ui.key_size = 64    -- 键帽边长；字符字号见 screen.cpp 的 drawKeyHand（26/12，随它缩放）

-- 面板一屏露几列几行（滑窗可见范围）：3 列 = 2×70+64 = 204，正好塞进 panel_w 220
ui.key_window = { cols = 3, rows = 3 }

local function hand_keys()
    local t = {}
    for r = 0, 2 do
        for c = 0, 4 do
            t[#t + 1] = {x = c * 70, y = r * 76}  -- 键 64，横向间隙 6、纵向间隙 12
        end
    end
    return t
end

ui.keys = {}  -- 左手 15 个再右手 15 个，坐标相同（两场互不干扰）
for _, side in ipairs({hand_keys(), hand_keys()}) do
    for _, k in ipairs(side) do
        ui.keys[#ui.keys + 1] = k
    end
end

-- 每层 30 个单字符，顺序同上；改顺序等于改布局
ui.layers = {
    alpha = {
        'q', 'w', 'e', 'r', 't',
        'a', 's', 'd', 'f', 'g',
        'z', 'x', 'c', 'v', ',',
        'y', 'u', 'i', 'o', 'p',
        'h', 'j', 'k', 'l', ':',
        'b', 'n', 'm', '.', '?',
    },
    num = {
        '1', '2', '3', '4', '5',
        '[', '-', '.', '|', '/',
        ']', '=', '_', ',', '(',
        '6', '7', '8', '9', '0',
        ')', ':', ';', '>', '<',
        '"', "'", '~', '?', '@',
    },
    sym = {
        '!', '@', '#', '$', '%',
        '{', '}', '[', ']', '\\',
        '&', '*', '+', ';', '~',
        '^', '(', ')', '_', '"',
        '`', "'", ',', '.', '?',
        '-', '/', '|', '=', '<',
    },
}
