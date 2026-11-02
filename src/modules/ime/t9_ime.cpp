#include "modules/ime/t9_ime.h"

#include <algorithm>

namespace ime {
namespace {

// 8 扇区（0=E，逆时针 45° 一格）→ 九宫格位移：row/col 各 -1..1
void sectorDelta(int sector, int& dRow, int& dCol) {
    switch (sector) {
        case 0: dCol = 1; break;                     // E
        case 1: dRow = -1; dCol = 1; break;          // NE
        case 2: dRow = -1; break;                    // N
        case 3: dRow = -1; dCol = -1; break;         // NW
        case 4: dCol = -1; break;                    // W
        case 5: dRow = 1; dCol = -1; break;          // SW
        case 6: dRow = 1; break;                     // S
        case 7: dRow = 1; dCol = 1; break;           // SE
        default: break;
    }
}

}  // namespace

void T9Ime::reset() {
    popup_ = false;
    focus_ = 4;
    letterIdx_ = 0;
}

int T9Ime::letterCount() const {
    if (!keys_ || focus_ < 0 || focus_ > 8) return 0;
    return int(keys_[focus_].letters.size());
}

char T9Ime::currentLetter() const {
    if (!keys_ || focus_ < 0 || focus_ > 8) return 0;
    const std::string& s = keys_[focus_].letters;
    if (s.empty()) return 0;
    return s[size_t(std::clamp(letterIdx_, 0, int(s.size()) - 1))];
}

char T9Ime::focusDigit() const {
    if (!keys_ || focus_ < 0 || focus_ > 8) return 0;
    const std::string& n = keys_[focus_].num;
    return n.empty() ? 0 : n[0];
}

std::string T9Ime::magic() {
    if (!keys_) return {};
    if (!popup_) {  // Grid → Popup：锁定当前格子
        popup_ = true;
        letterIdx_ = 0;
        return {};
    }
    // Popup → 输入当前字母，保持弹窗（连击）
    char ch = currentLetter();
    if (!ch) return {};
    return std::string(1, ch);
}

void T9Ime::stick(int sector) {
    if (!keys_ || sector < 0 || sector > 7) return;
    if (popup_) {  // Popup：左/右换字母（三格字母时也够用）
        int n = letterCount();
        if (n <= 1) return;
        bool left = (sector >= 3 && sector <= 5);   // NW/W/SW 算左
        bool right = (sector >= 0 && sector <= 1) || sector == 7;
        if (left) letterIdx_ = (letterIdx_ + n - 1) % n;
        else if (right) letterIdx_ = (letterIdx_ + 1) % n;
        return;
    }
    // Grid：焦点在九宫格里移动，越界不动（不循环，避免从边上一把跳到对面）
    int dRow = 0, dCol = 0;
    sectorDelta(sector, dRow, dCol);
    int row = std::clamp(focus_ / 3 + dRow, 0, 2);
    int col = std::clamp(focus_ % 3 + dCol, 0, 2);
    focus_ = row * 3 + col;
}

void T9Ime::setFocus(int idx) {
    if (idx >= 0 && idx < 9) focus_ = idx;
}

void T9Ime::setLetter(int idx) {
    if (idx >= 0 && idx < letterCount()) letterIdx_ = idx;
}

}  // namespace ime
