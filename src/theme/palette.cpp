#include "theme/palette.h"

#include <cstring>

namespace pal {
namespace {

struct Theme {
    const char* name;
    uint32_t bg, panel, term, line, text, dim, faint;
    uint32_t cyan, red, green, purple, yellow, blue;
    uint32_t shL, shZL, shR, shZR, shZLText, shZRText, rightKey, kkBg;
    uint32_t ansi[16];
};

// 顺序即 --list-themes 的顺序，第一个是默认
const Theme kThemes[] = {
    {"switch",  // 原版：近黑冷调
     0x08080c, 0x0c0c14, 0x06060a, 0x1a1a26, 0xc8c8d4, 0x5a5a72, 0x2e2e40,
     0x22d3ee, 0xff6b5a, 0x4ade80, 0xa855f7, 0xfbbf24, 0x4ab8e6,
     0x1a1226, 0x14101e, 0x0e1e26, 0x0a1620, 0x8a6ab0, 0x3a8aa8, 0xff9a8a, 0x14141e,
     {0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
      0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00, 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff}},

    {"light",  // 浅色：白天/室外
     0xf4f4ee, 0xe8e8e1, 0xffffff, 0xc9c9c0, 0x1f1f28, 0x6e6e7a, 0xa8a8b2,
     0x0e7490, 0xdc2626, 0x15803d, 0x7c3aed, 0xb45309, 0x2563eb,
     0xefe8f7, 0xe6dff2, 0xe2f1f7, 0xd9eaf3, 0x6b4a9e, 0x2b6b86, 0xc2410c, 0xe8e8f0,
     {0x000000, 0x800000, 0x006400, 0x806000, 0x000080, 0x800080, 0x006060, 0xb0b0b0,
      0x707070, 0xc00000, 0x008000, 0x907000, 0x0000c0, 0xa000a0, 0x008080, 0x1f1f28}},

    {"gruvbox",  // 暖色复古
     0x1d2021, 0x282828, 0x1d2021, 0x3c3836, 0xebdbb2, 0xa89984, 0x665c54,
     0x8ec07c, 0xfb4934, 0xb8bb26, 0xd3869b, 0xfabd2f, 0x83a598,
     0x322920, 0x2a2419, 0x1f2a2a, 0x1a2222, 0xb8a06a, 0x7a9e9a, 0xfe8019, 0x32302f,
     {0x282828, 0xcc241d, 0x98971a, 0xd79921, 0x458588, 0xb16286, 0x689d6a, 0xa89984,
      0x928374, 0xfb4934, 0xb8bb26, 0xfabd2f, 0x83a598, 0xd3869b, 0x8ec07c, 0xebdbb2}},
};

const char* gCur = kThemes[0].name;

void apply(const Theme& t) {
    bg = t.bg;
    panel = t.panel;
    term = t.term;
    line = t.line;
    text = t.text;
    dim = t.dim;
    faint = t.faint;
    cyan = t.cyan;
    red = t.red;
    green = t.green;
    purple = t.purple;
    yellow = t.yellow;
    blue = t.blue;
    shL = t.shL;
    shZL = t.shZL;
    shR = t.shR;
    shZR = t.shZR;
    shZLText = t.shZLText;
    shZRText = t.shZRText;
    rightKey = t.rightKey;
    kkBg = t.kkBg;
    for (int i = 0; i < 16; ++i) ansi[i] = t.ansi[i];
    gCur = t.name;
}

}  // namespace

bool load(const char* name) {
    if (!name) return false;
    for (const Theme& t : kThemes) {
        if (std::strcmp(t.name, name) == 0) {
            apply(t);
            return true;
        }
    }
    return false;
}

void loadDefault() { apply(kThemes[0]); }

const char* const* names() {
    static const char* list[sizeof(kThemes) / sizeof(kThemes[0]) + 1] = {};
    for (size_t i = 0; i < sizeof(kThemes) / sizeof(kThemes[0]); ++i) list[i] = kThemes[i].name;
    return list;
}

const char* current() { return gCur; }

}  // namespace pal
