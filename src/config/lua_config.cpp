#include "config/lua_config.h"

#include <lua.hpp>

#include <cstdio>
#include <cstring>

namespace cfg {
namespace {

struct ActionDef {
    const char* name;
    Action act;
};
const ActionDef kActions[] = {
    {"none", kNone},        {"magic", kMagic},       {"space", kSpace},
    {"tab", kTab},          {"enter", kEnter},       {"backspace", kBackspace},
    {"delete", kDelete},    {"escape", kEscape},     {"mod_ctrl", kModCtrl},
    {"mod_alt", kModAlt},   {"mod_shift", kModShift},{"lock_ctrl", kLockCtrl},
    {"lock_alt", kLockAlt}, {"lock_shift", kLockShift},
    {"mouse_left", kMouseLeft}, {"mouse_right", kMouseRight},
};

struct KeyDef {
    const char* name;
    InputKey key;
};
const KeyDef kKeys[] = {
    {"LB", kLB}, {"LT", kLT}, {"RB", kRB}, {"RT", kRT},   {"A", kA}, {"B", kB},
    {"X", kX},   {"Y", kY},   {"DPAD_UP", kDpadUp},      {"DPAD_DOWN", kDpadDown},
    {"DPAD_LEFT", kDpadLeft}, {"DPAD_RIGHT", kDpadRight},
};

// keymap.lua 调用的 C 函数把结果写进这里（单线程启动期，静态够用）
struct Builder {
    KeyMap* km;
};
Builder gBuild{};

bool findKey(const char* n, InputKey& out) {
    for (const KeyDef& d : kKeys) {
        if (std::strcmp(d.name, n) == 0) {
            out = d.key;
            return true;
        }
    }
    return false;
}

int l_bind(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    const char* act = luaL_checkstring(L, 2);
    Action a;
    InputKey k;
    if (!actionFromName(act, a)) {
        std::fprintf(stderr, "keymap: 未知动作 '%s'\n", act);
        return 0;
    }
    if (!findKey(key, k)) {
        std::fprintf(stderr, "keymap: 未知键 '%s'\n", key);
        return 0;
    }
    gBuild.km->single[k] = a;
    return 0;
}

int l_bindDouble(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    const char* act = luaL_checkstring(L, 2);
    Action a;
    InputKey k;
    if (!actionFromName(act, a) || !findKey(key, k)) {
        std::fprintf(stderr, "keymap: bind_double('%s','%s') 无效\n", key, act);
        return 0;
    }
    gBuild.km->dbl[k] = a;
    return 0;
}

int l_bindAxis(lua_State* L) {
    const char* axis = luaL_checkstring(L, 1);
    const char* what = luaL_checkstring(L, 2);
    if (std::strcmp(axis, "STICK_R") == 0 && std::strcmp(what, "mouse_move") == 0)
        gBuild.km->axisMouseMove = true;
    else
        std::fprintf(stderr, "keymap: bind_axis('%s','%s') 目前只支持 STICK_R/mouse_move\n",
                     axis, what);
    return 0;
}

int inum(lua_State* L, const char* k, int def) {
    lua_getglobal(L, "ui");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return def;
    }
    lua_getfield(L, -1, k);
    int v = lua_isnumber(L, -1) ? int(lua_tointeger(L, -1)) : def;
    lua_pop(L, 2);
    return v;
}

std::string istr(lua_State* L, const char* k, const char* def) {
    lua_getglobal(L, "ui");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return def;
    }
    lua_getfield(L, -1, k);
    std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : def;
    lua_pop(L, 2);
    return v;
}

// 读 ui.t9_alpha / t9_symbol：9 项 { "2", "ABC" }
void readT9(lua_State* L, const char* field, T9Key out[9]) {
    lua_getglobal(L, "ui");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, field);
    if (lua_istable(L, -1)) {
        for (int i = 1; i <= 9; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_istable(L, -1)) {
                lua_rawgeti(L, -1, 1);
                if (lua_isstring(L, -1)) out[i - 1].num = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_rawgeti(L, -1, 2);
                if (lua_isstring(L, -1)) out[i - 1].letters = lua_tostring(L, -1);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 2);
}

bool dofile(const char* path) {
    lua_State* L = luaL_newstate();
    if (!L) return false;
    luaL_openlibs(L);
    if (luaL_dofile(L, path) != LUA_OK) {
        std::fprintf(stderr, "%s: %s\n", path, lua_tostring(L, -1));
        lua_close(L);
        return false;
    }
    lua_close(L);
    return true;
}

}  // namespace

bool actionFromName(const char* s, Action& out) {
    for (const ActionDef& d : kActions) {
        if (std::strcmp(d.name, s) == 0) {
            out = d.act;
            return true;
        }
    }
    return false;
}

const char* actionName(Action a) {
    for (const ActionDef& d : kActions)
        if (d.act == a) return d.name;
    return "none";
}

bool loadUi(const char* path, Ui& out) {
    lua_State* L = luaL_newstate();
    if (!L) return false;
    luaL_openlibs(L);
    if (luaL_dofile(L, path) != LUA_OK) {
        std::fprintf(stderr, "%s: %s\n", path, lua_tostring(L, -1));
        lua_close(L);
        return false;
    }
    out.panelW = inum(L, "panel_w", out.panelW);
    out.topBarH = inum(L, "top_bar_h", out.topBarH);
    out.botBarH = inum(L, "bot_bar_h", out.botBarH);
    out.stickBox = inum(L, "stick_box", out.stickBox);
    out.dpadCell = inum(L, "dpad_cell", out.dpadCell);
    out.abxyCell = inum(L, "abxy_cell", out.abxyCell);
    out.shoulderH = inum(L, "shoulder_h", out.shoulderH);
    out.termPadX = inum(L, "term_pad_x", out.termPadX);
    out.termPadTop = inum(L, "term_pad_top", out.termPadTop);
    out.termPadBot = inum(L, "term_pad_bot", out.termPadBot);
    out.cellW = inum(L, "cell_w", out.cellW);
    out.cellH = inum(L, "cell_h", out.cellH);
    out.imeRows = inum(L, "ime_rows", out.imeRows);
    out.title = istr(L, "title", out.title.c_str());
    readT9(L, "t9_alpha", out.t9Alpha);
    readT9(L, "t9_symbol", out.t9Symbol);
    lua_getglobal(L, "ui");
    if (lua_istable(L, -1)) {  // ui.labels.t9_title
        lua_getfield(L, -1, "labels");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "t9_title");
            if (lua_isstring(L, -1)) out.t9Title = lua_tostring(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_close(L);
    return true;
}

bool loadKeymap(const char* path, KeyMap& out) {
    lua_State* L = luaL_newstate();
    if (!L) return false;
    luaL_openlibs(L);

    gBuild.km = &out;
    lua_newtable(L);  // keymap
    lua_pushcfunction(L, l_bind);
    lua_setfield(L, -2, "bind");
    lua_pushcfunction(L, l_bindDouble);
    lua_setfield(L, -2, "bind_double");
    lua_pushcfunction(L, l_bindAxis);
    lua_setfield(L, -2, "bind_axis");
    lua_setglobal(L, "keymap");

    if (luaL_dofile(L, path) != LUA_OK) {
        std::fprintf(stderr, "%s: %s\n", path, lua_tostring(L, -1));
        lua_close(L);
        return false;
    }
    lua_close(L);
    return true;
}

}  // namespace cfg
