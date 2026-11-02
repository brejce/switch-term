#pragma once
// T9 九键状态机：Grid（选格子）↔ Popup（选字母，可连击）。
// 只管「焦点/弹窗/选中的字母」，上屏和候选交给 pinyin IME。
#include <string>

#include "config/lua_config.h"

namespace ime {

class T9Ime {
public:
    void setKeys(const cfg::T9Key* keys) { keys_ = keys; }  // 由装配层给（alpha 或 symbol 层）
    void reset();

    bool popup() const { return popup_; }
    int focus() const { return focus_; }       // 0..8
    int letterIdx() const { return letterIdx_; }
    int letterCount() const;                   // 当前格子有几个字母
    char currentLetter() const;                // Popup 里当前选中的字母
    char focusDigit() const;                   // 焦点格的数字（T9 九键用）

    // 魔法键（LB）：Grid → Popup；Popup → 返回要上屏的字母（不退出弹窗，可连击）
    std::string magic();

    // 摇杆 8 扇区：Grid 里移动焦点；Popup 里左右换字母
    void stick(int sector);

    void leavePopup() { popup_ = false; }

private:
    const cfg::T9Key* keys_ = nullptr;
    bool popup_ = false;
    int focus_ = 4;      // 默认中心 5(JKL)
    int letterIdx_ = 0;
};

}  // namespace ime
