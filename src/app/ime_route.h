#pragma once
// 输入法按键路由：把「按下某个字节」翻译成「往终端送什么」。
// 抽出来是为了能脱离 UI/PTY 单测，行为对齐标准拼音输入法。
#include <string>

#include "ime/builtin_ime.h"

namespace app {

struct RouteResult {
    std::string bytes;   // 要写进 PTY 的内容（可能为空）
    bool consumed = false;  // 是否被输入法吃掉（true 时不要再往 PTY 发原始字符）
    bool langChanged = false;  // 是否切换了中/英（调用方要刷新状态栏）
};

// mods 位图：1=Ctrl 2=Shift 4=Alt（来自 sticky / 锁定）。
// Ctrl/Alt + 字母 优先于输入法：这样才能发出 Ctrl+C 这类组合键。
RouteResult routeKey(ime::BuiltinIme& ime, char ch, int mods = 0);

}  // namespace app
