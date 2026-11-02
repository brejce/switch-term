#pragma once
// 调色板：主题管理的唯一数据源。UI 与终端（ANSI 16 色）都从这里取色。
#include <cstdint>

namespace pal {

// UI 用色
inline uint32_t bg, panel, term, line, text, dim, faint;
inline uint32_t cyan, red, green, purple, yellow, blue;
inline uint32_t shL, shZL, shR, shZR, shZLText, shZRText, rightKey, kkBg;
// 终端 ANSI 0-15（16-255 仍走 vterm 自己的 256 色表）
inline uint32_t ansi[16];

// 载入内置主题；名字不认识返回 false 且不改动当前配色
bool load(const char* name);
const char* const* names();  // 以 nullptr 结尾
const char* current();

void loadDefault();

}  // namespace pal
