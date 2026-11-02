-- UI 装配：只在这里描述长什么样，运行期全部由 C++ 用原生结构跑
-- 尺寸单位＝设计像素（1280×720 基准），实际会按窗口乘缩放

ui = {}

ui.title      = "SYS.TERM"
ui.panel_w    = 220
ui.top_bar_h  = 32
ui.bot_bar_h  = 26

ui.stick_box  = 180      -- T9 九宫格外框边长
ui.dpad_cell  = 28
ui.abxy_cell  = 30
ui.shoulder_h = 20

ui.term_pad_x   = 14
ui.term_pad_top = 10
ui.term_pad_bot = 8
ui.cell_w       = 8
ui.cell_h       = 21
ui.ime_rows     = 2      -- 底部固定留给组字串/候选的行数

-- 左手区：T9 九键（数字是物理标准布局，letter 用多击取字母）
-- 顺序＝九宫格 row-major：1..9
ui.t9_alpha = {
    { "1", ".,?"  },
    { "2", "ABC"  },
    { "3", "DEF"  },
    { "4", "GHI"  },
    { "5", "JKL"  },
    { "6", "MNO"  },
    { "7", "PQRS" },
    { "8", "TUV"  },
    { "9", "WXYZ" },
}

-- 符号层（由 Shift/切层进入）
ui.t9_symbol = {
    { "+", "-*"  },
    { ".", ",?"  },
    { ":", ";!"  },
    { "(", ")["  },
    { "]", "]}{" },
    { "/", "=%"  },
    { "<", ">|"  },
    { "\\", "_&" },
    { "~", "^`"  },
}

-- 状态栏文字
ui.labels = {
    t9_title    = "// T9_KEYPAD",
    dpad_title  = "// DPAD (MODS)",
    mouse_title = "// MOUSE_CONTROL",
    abxy_title  = "// ABXY",
}
