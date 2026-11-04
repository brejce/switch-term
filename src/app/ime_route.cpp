#include "app/ime_route.h"

namespace app {

RouteResult routeKey(ime::BuiltinIme& ime, char ch, int mods) {
    RouteResult r;
    const bool ctrl = (mods & 1) != 0, shift = (mods & 2) != 0, alt = (mods & 4) != 0;

    // Ctrl/Alt + 字母：只在英文态当组合键（Ctrl+C = 0x03 就是这样出来的）。
    // 中文态不抢字母，否则拼音根本打不出来。
    if (!ime.chinese && ch >= 'a' && ch <= 'z') {
        if (ctrl) {
            r.bytes = std::string(1, char(ch - 'a' + 1));
            r.consumed = true;
            return r;
        }
        if (alt) {
            r.bytes = std::string("\x1b") + ch;
            r.consumed = true;
            return r;
        }
        if (shift) {  // 英文态 Shift → 大写
            r.bytes = std::string(1, char(ch - 'a' + 'A'));
            r.consumed = true;
            return r;
        }
    }

    // 中文态下把大写当小写处理：锁了 Shift 也不会把拼音打成大写（否则组字直接失效）
    if (ime.chinese && ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');

    // Ctrl+Space：切中英。组字中的拼音原样落下（跟 fcitx 一样，不吞字）
    if (ch == 0x00) {
        r.bytes = ime.toggle();
        r.consumed = true;
        r.langChanged = true;
        return r;
    }

    if (ime.chinese && ime.composing()) {
        if (ch >= '1' && ch <= '9') {  // 数字选词：只认当前页内的候选（页大小 = engine.ime.cand_per_page）
            int idx = ch - '1';
            if (idx < ime.pageSize()) {
                r.bytes = ime.commit(ime.page() * ime.pageSize() + idx);
                r.consumed = true;
                return r;
            }
            // 8/9 超出本页：不吞掉，当普通字符走下面
        }
        if (ch == ' ') {  // 空格 = 上屏候选条里选中的那个（R/ZR 移动选中项）
            r.bytes = ime.commit(-1);
            r.consumed = true;
            return r;
        }
        if (ch == '\r') {  // 回车 = 拼音原样上屏（打出 nihao 本身）
            r.bytes = ime.commitRaw();
            r.consumed = true;
            return r;
        }
        if (ch == '\t') {  // Tab 翻页
            ime.nextPage();
            r.consumed = true;
            return r;
        }
        if (ch == 0x7f) {  // 退格删字母
            ime.backspace();
            r.consumed = true;
            return r;
        }
        if (ch == 0x1b) {  // Esc 取消组字，字符照常交给 shell
            ime.clear();
            r.bytes = ch;
            r.consumed = true;
            return r;
        }
        if (ch >= 'a' && ch <= 'z') {
            ime.push(ch);
            r.consumed = true;
            return r;
        }
        // 组字中收到不认识的控件字节（例如锁了 Ctrl 打出来的 0x01~0x1a）：
        // 直接吞掉，别让它把 shell 搞乱
        if ((unsigned char)ch < 0x20 && ch != ' ' && ch != '\r' && ch != '\t' && ch != 0x1b &&
            ch != 0x7f) {
            r.consumed = true;
            return r;
        }
        // 标点等：先把候选上屏，再发这个字符，否则标点会跑到中文前面
        r.bytes = ime.commit(-1);
        r.bytes += ch;
        r.consumed = true;
        return r;
    }

    if (ime.chinese && ch >= 'a' && ch <= 'z') {  // 中文态：字母起组字
        ime.push(ch);
        r.consumed = true;
        return r;
    }

    r.bytes = ch;  // 其余原样进 PTY
    return r;
}

RouteResult routeRaw(char ch, int mods) {
    RouteResult r;
    const bool ctrl = (mods & 1) != 0, shift = (mods & 2) != 0, alt = (mods & 4) != 0;

    // Ctrl+Space 原样交出去（空 + consumed=false），绝不切语言：
    // 桌面模式下这是 fcitx5 的切换键，吞了就等于抢用户的热键。
    if (ch == 0x00) return r;

    if (ch >= 'a' && ch <= 'z') {
        if (ctrl) {
            r.bytes = std::string(1, char(ch - 'a' + 1));
            r.consumed = true;
            return r;
        }
        if (alt) {
            r.bytes = std::string("\x1b") + ch;
            r.consumed = true;
            return r;
        }
        if (shift) {
            r.bytes = std::string(1, char(ch - 'a' + 'A'));
            r.consumed = true;
            return r;
        }
    } else if (alt && (unsigned char)ch >= 0x20 && (unsigned char)ch < 0x7f) {
        r.bytes = std::string("\x1b") + ch;  // Alt+标点/数字：ESC 前缀
        r.consumed = true;
        return r;
    }

    r.bytes = ch;  // 其余（含控制码、UTF-8 高位字节）一个字节原样透传
    return r;
}

}  // namespace app
