#include "config/lua_config.h"

#include <lua.hpp>

#include <algorithm>
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
    {"next_tab", kNextTab}, {"prev_tab", kPrevTab},
    {"new_tab", kNewTab},   {"close_tab", kCloseTab},
    {"layer_next", kLayerNext}, {"onboard", kOnboard},
    // 光标左右：原来是面板独有动作，v2 起 R / ZR 也绑这两个（LAYOUT-v2 §5）
    {"cursor_left", kCursorLeft}, {"cursor_right", kCursorRight},
};

struct KeyDef {
    const char* name;
    InputKey key;
};
const KeyDef kKeys[] = {
    {"LB", kLB}, {"LT", kLT}, {"RB", kRB}, {"RT", kRT},   {"A", kA}, {"B", kB},
    {"X", kX},   {"Y", kY},   {"DPAD_UP", kDpadUp},      {"DPAD_DOWN", kDpadDown},
    {"DPAD_LEFT", kDpadLeft}, {"DPAD_RIGHT", kDpadRight}, {"L3", kL3},
    {"START", kStart},
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

// ui.<sect>.<k>：ui.key_window.cols 这种小表
int uiSubInt(lua_State* L, const char* sect, const char* k, int def) {
    lua_getglobal(L, "ui");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return def;
    }
    lua_getfield(L, -1, sect);
    int v = def;
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, k);
        if (lua_isnumber(L, -1)) v = int(lua_tointeger(L, -1));
        lua_pop(L, 1);
    }
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


// UTF-8 串的首字符（ui.layers 的每格是一个字符，可能是 3 字节汉字）
std::string firstUtf8(const char* s) {
    if (!s || !*s) return {};
    const unsigned char b = static_cast<unsigned char>(*s);
    const int n = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
    return std::string(s, size_t(n));
}

// 读 ui.keys：30 项 {x=,y=}
void readKeysV2(lua_State* L, KeyV2 out[kKeyV2Count]) {
    lua_getglobal(L, "ui");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "keys");
    if (lua_istable(L, -1)) {
        for (int i = 1; i <= kKeyV2Count; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_istable(L, -1)) {
                lua_getfield(L, -1, "x");
                if (lua_isnumber(L, -1)) out[i - 1].x = int(lua_tointeger(L, -1));
                lua_pop(L, 1);
                lua_getfield(L, -1, "y");
                if (lua_isnumber(L, -1)) out[i - 1].y = int(lua_tointeger(L, -1));
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 2);
}

// 读 ui.layers.<alpha|num|sym>：每层 30 个单字符
void readLayersAt(lua_State* L, const char* table, std::string out[kLayerCount][kKeyV2Count]) {
    static const char* names[kLayerCount] = {"alpha", "num", "sym"};
    lua_getglobal(L, "ui");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, table);
    if (lua_istable(L, -1)) {
        for (int l = 0; l < kLayerCount; ++l) {
            lua_getfield(L, -1, names[l]);
            if (lua_istable(L, -1)) {
                for (int i = 1; i <= kKeyV2Count; ++i) {
                    lua_rawgeti(L, -1, i);
                    if (lua_isstring(L, -1)) out[l][i - 1] = firstUtf8(lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 2);
}

void readLayers(lua_State* L, std::string out[kLayerCount][kKeyV2Count]) {
    readLayersAt(L, "layers", out);
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
    out.keySize = inum(L, "key_size", out.keySize);
    out.keyWindowCols = uiSubInt(L, "key_window", "cols", out.keyWindowCols);
    out.keyWindowRows = uiSubInt(L, "key_window", "rows", out.keyWindowRows);
    out.keyAreaDy = einum(L, "ui", "key_area_dy", out.keyAreaDy);
    out.title = istr(L, "title", out.title.c_str());
    readKeysV2(L, out.keysV2);
    readLayersAt(L, "layers", out.layers);
    readLayersAt(L, "shifts", out.shifts);
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
    out.imeEnabled = ebool(L, "ime", "enabled", out.imeEnabled);
    out.deadzone = efloat(L, "input", "deadzone", out.deadzone);
    out.hysteresisDeg = efloat(L, "input", "hysteresis_deg", out.hysteresisDeg);
    out.doubleTapMs = einum(L, "input", "double_tap_ms", out.doubleTapMs);
    out.rotate = einum(L, "display", "rotate", out.rotate);
    out.physics.damping = efloat(L, "physics", "damping", out.physics.damping);
    out.physics.attractPower = efloat(L, "physics", "attract_power", out.physics.attractPower);
    out.physics.attractRadius = efloat(L, "physics", "attract_radius", out.physics.attractRadius);
    out.physics.snapRadius = efloat(L, "physics", "snap_radius", out.physics.snapRadius);
    out.physics.releaseRatio = efloat(L, "physics", "release_ratio", out.physics.releaseRatio);
    out.physics.releasePower = efloat(L, "physics", "release_power", out.physics.releasePower);
    out.physics.maxSpeed = efloat(L, "physics", "max_speed", out.physics.maxSpeed);
    out.physics.speedSlowEnd =
        efloat(L, "physics", "speed_slow_end", out.physics.speedSlowEnd);
    out.physics.speedLow = efloat(L, "physics", "speed_low", out.physics.speedLow);
    out.physics.speedPower = efloat(L, "physics", "speed_power", out.physics.speedPower);
    out.physics.hoverRadius = efloat(L, "physics", "hover_radius", out.physics.hoverRadius);
    out.physics.homePull = efloat(L, "physics", "home_pull", out.physics.homePull);  // engine.physics.dt 不读：写死在 physics.cpp
    out.physics.pointerRadius =
        efloat(L, "physics", "pointer_radius", out.physics.pointerRadius);  // 指针球半径，0 = 原点模型

    // engine.rumble：Joy-Con 震动
    out.rumble.enabled = ebool(L, "rumble", "enabled", out.rumble.enabled);
    out.rumble.moveMax = efloat(L, "rumble", "move_max", out.rumble.moveMax);
    out.rumble.moveSpeedRef = efloat(L, "rumble", "move_speed_ref", out.rumble.moveSpeedRef);
    out.rumble.snapStrong = efloat(L, "rumble", "snap_strong", out.rumble.snapStrong);
    out.rumble.snapWeak = efloat(L, "rumble", "snap_weak", out.rumble.snapWeak);
    out.rumble.snapMs = einum(L, "rumble", "snap_ms", out.rumble.snapMs);
    out.rumble.rateHz = einum(L, "rumble", "rate_hz", out.rumble.rateHz);
    out.rumble.moveSmoothMs = efloat(L, "rumble", "move_smooth_ms", out.rumble.moveSmoothMs);
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
