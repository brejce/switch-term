// layout.lua 加载器：布局尺寸和键位都在启动时从 Lua 读，改完不用重新编译
#include <lua.hpp>

#include <cstdio>
#include <string>

#include "ui/layout.h"
#include "ui/panels.h"

namespace ui {
namespace layout {
namespace {

int inum(lua_State* L, const char* key, int def) {
    lua_getglobal(L, key);
    int v = lua_isnumber(L, -1) ? int(lua_tointeger(L, -1)) : def;
    lua_pop(L, 1);
    return v;
}

void str(lua_State* L, const char* key, std::string& dst) {
    lua_getglobal(L, key);
    if (lua_isstring(L, -1)) dst = lua_tostring(L, -1);
    lua_pop(L, 1);
}

// 13 项字符串表：1..9 是九宫格，10..13 是外圈（上 右 下 左）
void table13(lua_State* L, const char* key, StickSpec& spec) {
    lua_getglobal(L, key);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    int n = int(lua_rawlen(L, -1));
    for (int i = 1; i <= n && i <= 13; ++i) {
        lua_rawgeti(L, -1, i);
        if (lua_isstring(L, -1)) {
            std::string v = lua_tostring(L, -1);
            if (i <= 9)
                spec.cells[i - 1] = v;
            else
                spec.outers[i - 10] = v;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

}  // namespace

bool loadFile(const char* path) {
    FILE* probe = std::fopen(path, "r");
    if (!probe) return false;  // 没有就用默认值
    std::fclose(probe);

    lua_State* L = luaL_newstate();
    if (!L) return false;
    luaL_openlibs(L);
    keys::resetDefaults();  // 先铺默认，再让 Lua 逐项覆盖
    if (luaL_dofile(L, path) != LUA_OK) {
        std::fprintf(stderr, "layout: %s 有错：%s（用默认布局）\n", path,
                     lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        lua_close(L);
        base = Base{};
        recompute();
        return false;
    }

    base.topBarH = inum(L, "top_bar_h", base.topBarH);
    base.botBarH = inum(L, "bot_bar_h", base.botBarH);
    base.panelW = inum(L, "panel_w", base.panelW);
    base.panelPadTop = inum(L, "panel_pad_top", base.panelPadTop);
    base.panelPadX = inum(L, "panel_pad_x", base.panelPadX);
    base.stickBox = inum(L, "stick_box", base.stickBox);
    base.dpadCell = inum(L, "dpad_cell", base.dpadCell);
    base.abxyCell = inum(L, "abxy_cell", base.abxyCell);
    base.termPadX = inum(L, "term_pad_x", base.termPadX);
    base.termPadTop = inum(L, "term_pad_top", base.termPadTop);
    base.termPadBot = inum(L, "term_pad_bot", base.termPadBot);
    base.cellW = inum(L, "cell_w", base.cellW);
    base.cellH = inum(L, "cell_h", base.cellH);
    base.imeRows = inum(L, "ime_rows", base.imeRows);

    table13(L, "left_alpha", keys::leftAlpha);
    table13(L, "left_symbol", keys::leftSymbol);
    table13(L, "right_alpha", keys::rightAlpha);
    table13(L, "right_num", keys::rightNum);
    str(L, "dpad_tab", keys::dpadTab);
    str(L, "dpad_ctrl", keys::dpadCtrl);
    str(L, "dpad_shift", keys::dpadShift);
    str(L, "dpad_alt", keys::dpadAlt);
    str(L, "abxy_x", keys::abxyX);
    str(L, "abxy_a", keys::abxyA);
    str(L, "abxy_b", keys::abxyB);

    lua_close(L);

    // 面板不能宽到把终端挤没（按设计尺寸判断）
    if (base.panelW < 80) base.panelW = 80;
    if (base.panelW > kDesignW / 2 - 80) base.panelW = kDesignW / 2 - 80;
    recompute();
    return true;
}

}  // namespace layout
}  // namespace ui
