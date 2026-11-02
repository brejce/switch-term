#pragma once
// 弹窗布局表：锁定某个 T9 格子后，它的字符键按九宫格坐标展开成一排/一个 L。
// 纯常量 + 查表，以后要搬进 Lua 只换数据源，调用点不用改。
namespace ui {

// kPopupCells[locked][n] = 第 n 个字符落在九宫格的哪个 cell（-1 = 没有）
// cell 索引 row-major：0 1 2 / 3 4 5 / 6 7 8
// 每张表都包含锁定格自身，所以「锁定源」那格同时也是一个字符键。
inline constexpr int kPopupCells[9][4] = {
    {0, 1, 3, -1},  // 0 ".,?"  → nw 形 3 格
    {0, 1, 2, -1},  // 1 ABC
    {1, 2, 5, -1},  // 2 DEF
    {0, 3, 6, -1},  // 3 GHI
    {3, 4, 5, -1},  // 4 JKL
    {2, 5, 8, -1},  // 5 MNO
    {3, 6, 7, 8},   // 6 PQRS   → L 形 4 格
    {6, 7, 8, -1},  // 7 TUV
    {5, 6, 7, 8},   // 8 WXYZ   → 反 L 形 4 格
};

// 锁定格子 focus 后，第 n 个字符落在九宫格的哪个 cell 上（-1 = 没有）
inline int popupCellFor(int focus, int n) {
    if (focus < 0 || focus > 8 || n < 0 || n > 3) return -1;
    return kPopupCells[focus][n];
}

// 锁定格子 focus 的字符键占几个格子
inline int popupCellCount(int focus) {
    if (focus < 0 || focus > 8) return 0;
    int n = 0;
    while (n < 4 && kPopupCells[focus][n] >= 0) ++n;
    return n;
}

// 反查：点中九宫格 cell 时它是第几个字符（-1 = 这格不是字符键）
inline int popupLetterIdx(int focus, int cell) {
    for (int n = 0; n < popupCellCount(focus); ++n)
        if (kPopupCells[focus][n] == cell) return n;
    return -1;
}

}  // namespace ui
