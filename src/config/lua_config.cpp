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

// engine.<sect>.<k>：缺段/缺键/类型不符都返回 def（引擎配置全部可选）
void pushSection(lua_State* L, const char* sect) {  // 成功时栈顶 = 段表
    lua_getglobal(L, "engine");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, sect);
    lua_remove(L, -2);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
    }
}

int einum(lua_State* L, const char* sect, const char* k, int def) {
    pushSection(L, sect);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return def;
    }
    lua_getfield(L, -1, k);
    int v = lua_isnumber(L, -1) ? int(lua_tointeger(L, -1)) : def;
    lua_pop(L, 2);
    return v;
}

bool ebool(lua_State* L, const char* sect, const char* k, bool def) {
    pushSection(L, sect);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return def;
    }
    lua_getfield(L, -1, k);
    bool v = lua_isboolean(L, -1) ? lua_toboolean(L, -1) != 0 : def;
    lua_pop(L, 2);
    return v;
}

float efloat(lua_State* L, const char* sect, const char* k, float def) {
    pushSection(L, sect);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return def;
    }
    lua_getfield(L, -1, k);
    float v = lua_isnumber(L, -1) ? float(lua_tonumber(L, -1)) : def;
    lua_pop(L, 2);
    return v;
}

std::string estr(lua_State* L, const char* sect, const char* k, const char* def) {
    pushSection(L, sect);
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

bool loadEngine(const char* path, Engine& out) {
    // 文件不存在 = 没写引擎配置，静默用默认值（只有真写错了才报）
    if (FILE* f = std::fopen(path, "r")) {
        std::fclose(f);
    } else {
        return false;
    }
    lua_State* L = luaL_newstate();
    if (!L) return false;
    luaL_openlibs(L);
    if (luaL_dofile(L, path) != LUA_OK) {
        std::fprintf(stderr, "%s: %s\n", path, lua_tostring(L, -1));
        lua_close(L);
        return false;
    }
    out.scrollback = einum(L, "terminal", "scrollback", out.scrollback);
    out.candPerPage = einum(L, "ime", "cand_per_page", out.candPerPage);
    out.ngram = ebool(L, "ime", "ngram", out.ngram);
    out.t9 = ebool(L, "ime", "t9", out.t9);
    out.imeEnabled = ebool(L, "ime", "enabled", out.imeEnabled);
    out.deadzone = efloat(L, "input", "deadzone", out.deadzone);
    out.hysteresisDeg = efloat(L, "input", "hysteresis_deg", out.hysteresisDeg);
    out.doubleTapMs = einum(L, "input", "double_tap_ms", out.doubleTapMs);
    out.logLevel = estr(L, "log", "level", out.logLevel.c_str());
    out.logPath = estr(L, "log", "path", out.logPath.c_str());
    lua_getglobal(L, "engine");  // engine.sink 是顶层字段，不是表
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "sink");
        if (lua_isstring(L, -1)) out.sink = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_close(L);
    return true;
}

bool loadProfile(const char* dir, Profile& out) {
    const std::string d = dir && *dir ? dir : "config";
    bool any = loadUi((d + "/ui.lua").c_str(), out.ui);
    if (loadKeymap((d + "/keymap.lua").c_str(), out.keymap)) any = true;
    if (loadEngine((d + "/engine.lua").c_str(), out.engine)) any = true;
    return any;
}

}  // namespace cfg
