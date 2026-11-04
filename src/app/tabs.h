#pragma once
// 多 tab 的下标运算：纯函数，不认 PTY/UI，自检才不用起真终端。

namespace app {

// 循环取模：count 个 tab 里从 active 走 delta 格（负值往回）
inline int tabStep(int active, int count, int delta) {
    if (count <= 0) return 0;
    int i = (active + delta) % count;
    if (i < 0) i += count;
    return i;
}

// count = 删之前的数量，删完剩 count-1；删的是最后一个就落到新的最后一个
inline int tabAfterClose(int active, int count) {
    if (count <= 1) return 0;
    return active >= count - 1 ? count - 2 : active;
}

}  // namespace app
