#pragma once
// libvterm → 格网格。damage 只做「整屏需重绘」标记：101×29 全量重读的代价远小于脏矩形维护
// shortcut: 无局部脏矩形；滚动回放只往上攒历史行，往下 pop 不还原（libvterm sb_popline 返回 0）
#include <vterm.h>

#include <cstdint>
#include <deque>
#include <vector>

#include "theme/palette.h"

namespace term {

struct CellView {
    uint32_t cp = ' ';
    uint32_t fg = pal::text;
    uint32_t bg = pal::term;
    bool bold = false;
    bool underline = false;
    bool reverse = false;
    bool wide = false;  // width==2：下一格是延续格
};

class VTermBridge {
public:
    bool init(int rows, int cols);
    void resize(int rows, int cols);
    void feed(const char* bytes, size_t n);

    CellView cellAt(int row, int col) const;
    // 滚动历史：滚出屏幕顶部的行（最老在前），供 TerminalView 回放
    int historySize() const { return int(hist_.size()); }
    CellView historyAt(int line, int col) const;
    void sbPush(int cols, const VTermScreenCell* cells);
    void cursor(int& row, int& col) const;

    int colsNow() const { return cols_; }
    int rowsNow() const { return rows_; }
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }
    void markDirty() { dirty_ = true; }
    bool cursorVisible() const { return cursorVisible_; }
    size_t bytesIn() const { return bytesIn_; }

private:
    uint32_t colorOf(VTermColor c, bool isFg) const;
    CellView toView(const VTermScreenCell& c) const;

    VTerm* vt_ = nullptr;
    VTermScreen* screen_ = nullptr;
    bool dirty_ = true;
    bool cursorVisible_ = true;
    int rows_ = 0, cols_ = 0;
    std::deque<std::vector<CellView>> hist_;
    static constexpr size_t kHistMax = 2000;
    size_t bytesIn_ = 0;
};

}  // namespace term
