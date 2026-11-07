#pragma once
// libvterm → 格网格。damage 只记「整屏要重绘 + 哪几行变过」：行位图直接来自
// libvterm 的 damage/movecursor 回调，不再额外维护局部脏矩形。
// shortcut: 只有行粒度（不做列），滚动时按整屏；列粒度要改绘制层的按格重画
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
    void setHistoryLimit(size_t lines);  // engine.terminal.scrollback
    int historySize() const { return int(hist_.size()); }
    CellView historyAt(int line, int col) const;
    void sbPush(int cols, const VTermScreenCell* cells);
    void cursor(int& row, int& col) const;

    int colsNow() const { return cols_; }
    int rowsNow() const { return rows_; }
    bool dirty() const { return dirty_; }
    void clearDirty();
    void markDirty();  // 整块脏：所有行都置位
    // 行级脏位：libvterm 的 damage/movecursor 回调已经带行号，顺手记下就够
    bool rowDirty(int row) const;
    void markRowDirty(int row);
    bool cursorVisible() const { return cursorVisible_; }
    // 备用屏（vim/less/pi 这类全屏 TUI）：这里没有 scrollback，滚轮要转发给里面的程序
    bool altScreen() const { return altScreen_; }
    // 里面的程序开了鼠标上报（?1000/?1002/?1003）与 SGR 坐标（?1006）
    bool mouseReports() const { return mouse_; }
    bool mouseSgr() const { return mouseSgr_; }
    size_t bytesIn() const { return bytesIn_; }

private:
    uint32_t colorOf(VTermColor c, bool isFg) const;
    CellView toView(const VTermScreenCell& c) const;
    void markRows(int first, int last);  // [first, last) 行变脏
    // shortcut: CSI ?N h/l 只在单次 feed 内识别（跨 read 拆开的情况忽略）——
    // 这些模式都是程序启动时一次性设的，拆开几乎不会发生
    void scanModes(const char* p, size_t n);

    VTerm* vt_ = nullptr;
    VTermScreen* screen_ = nullptr;
    bool dirty_ = true;
    bool cursorVisible_ = true;
    bool altScreen_ = false, mouse_ = false, mouseSgr_ = false;
    int rows_ = 0, cols_ = 0;
    std::vector<uint8_t> rowDirty_;  // 每行一个字节（用 vector 是为了不引入 <bitset>）
    std::deque<std::vector<CellView>> hist_;
    size_t histMax_ = 2000;  // 默认 = config/engine.lua 的 terminal.scrollback
    size_t bytesIn_ = 0;
};

}  // namespace term
