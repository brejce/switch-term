#include "term/vterm_bridge.h"

#include <algorithm>
#include <cstring>

namespace term {

void VTermBridge::markRows(int first, int last) {
    dirty_ = true;
    if (rowDirty_.size() != size_t(rows_)) rowDirty_.assign(size_t(rows_), 1);
    const int lo = std::max(0, first), hi = std::min(rows_, last);
    if (lo >= hi) {  // 没行号可用（如 resize 中）：按整屏
        std::fill(rowDirty_.begin(), rowDirty_.end(), uint8_t(1));
        return;
    }
    std::fill(rowDirty_.begin() + lo, rowDirty_.begin() + hi, uint8_t(1));
}

void VTermBridge::markDirty() { markRows(0, rows_); }

void VTermBridge::markRowDirty(int row) { markRows(row, row + 1); }

void VTermBridge::clearDirty() {
    dirty_ = false;
    std::fill(rowDirty_.begin(), rowDirty_.end(), uint8_t(0));
}

bool VTermBridge::rowDirty(int row) const {
    if (row < 0 || row >= int(rowDirty_.size())) return dirty_;  // 尺寸对不上：别漏画
    return rowDirty_[size_t(row)] != 0;
}

bool VTermBridge::init(int rows, int cols) {
    rows_ = rows;
    cols_ = cols;
    rowDirty_.assign(size_t(rows), 1);
    vt_ = vterm_new(rows, cols);
    if (!vt_) return false;
    vterm_set_utf8(vt_, 1);
    screen_ = vterm_obtain_screen(vt_);
    if (!screen_) return false;

    static VTermScreenCallbacks cb;
    std::memset(&cb, 0, sizeof cb);
    cb.damage = [](VTermRect rect, void* u) {
        // end_row/end_col 是开区间
        static_cast<VTermBridge*>(u)->markRows(rect.start_row, rect.end_row);
        return 1;
    };
    cb.movecursor = [](VTermPos pos, VTermPos oldpos, int, void* u) {
        // 旧位置和新位置都要重画：光标是从一个格子抹掉再画到另一个格子
        auto* self = static_cast<VTermBridge*>(u);
        self->markRows(pos.row, pos.row + 1);
        self->markRows(oldpos.row, oldpos.row + 1);
        return 1;
    };
    cb.settermprop = [](VTermProp prop, VTermValue* val, void* u) {
        auto* self = static_cast<VTermBridge*>(u);
        if (prop == VTERM_PROP_CURSORVISIBLE) self->cursorVisible_ = val->boolean != 0;
        self->markDirty();
        return 1;
    };
    cb.sb_pushline = [](int cols, const VTermScreenCell* cells, void* u) {
        auto* self = static_cast<VTermBridge*>(u);
        self->sbPush(cols, cells);
        self->markDirty();  // 滚出一行 = 整屏上移
        return 1;
    };
    vterm_screen_set_callbacks(screen_, &cb, this);
    vterm_screen_set_damage_merge(screen_, VTERM_DAMAGE_SCROLL);
    vterm_screen_reset(screen_, 1);
    return true;
}

void VTermBridge::resize(int rows, int cols) {
    if (!vt_) return;
    rows_ = rows;
    cols_ = cols;
    rowDirty_.assign(size_t(rows), 1);
    vterm_set_size(vt_, rows, cols);
    vterm_screen_flush_damage(screen_);
    dirty_ = true;
}

void VTermBridge::feed(const char* bytes, size_t n) {
    if (!vt_) return;
    vterm_input_write(vt_, bytes, n);
    bytesIn_ += n;
    vterm_screen_flush_damage(screen_);
    dirty_ = true;
}

CellView VTermBridge::cellAt(int row, int col) const {
    VTermScreenCell c;
    VTermPos pos{row, col};
    if (!vterm_screen_get_cell(screen_, pos, &c)) return CellView{};
    return toView(c);
}

CellView VTermBridge::historyAt(int line, int col) const {
    if (line < 0 || line >= int(hist_.size())) return CellView{};
    const std::vector<CellView>& row = hist_[size_t(line)];
    if (col < 0 || col >= int(row.size())) return CellView{};
    return row[size_t(col)];
}

void VTermBridge::setHistoryLimit(size_t lines) {
    histMax_ = lines;
    while (hist_.size() > histMax_) hist_.pop_front();
}

void VTermBridge::sbPush(int cols, const VTermScreenCell* cells) {
    std::vector<CellView> row;
    row.reserve(size_t(cols));
    for (int i = 0; i < cols; ++i) row.push_back(toView(cells[i]));
    hist_.push_back(std::move(row));
    while (hist_.size() > histMax_) hist_.pop_front();
}

CellView VTermBridge::toView(const VTermScreenCell& c) const {
    CellView v;
    uint32_t ch = c.chars[0];
    // 宽字符的延续格里是垃圾码点（0xFFFFFFFF 之类），不校验就会编出非法 UTF-8
    v.cp = (ch && ch <= 0x10FFFF && !(ch >= 0xD800 && ch <= 0xDFFF)) ? ch : ' ';
    v.fg = colorOf(c.fg, true);
    v.bg = colorOf(c.bg, false);
    v.bold = c.attrs.bold;
    v.underline = c.attrs.underline;
    v.reverse = c.attrs.reverse;
    v.wide = c.width > 1;
    if (v.reverse) {
        uint32_t t = v.fg;
        v.fg = v.bg;
        v.bg = t;
    }
    return v;
}

uint32_t VTermBridge::colorOf(VTermColor c, bool isFg) const {
    if (isFg ? VTERM_COLOR_IS_DEFAULT_FG(&c) : VTERM_COLOR_IS_DEFAULT_BG(&c))
        return isFg ? pal::text : pal::term;
    if (c.type == VTERM_COLOR_INDEXED && c.indexed.idx < 16)  // ANSI 16 色跟主题走
        return pal::ansi[c.indexed.idx];
    VTermColor t = c;
    vterm_screen_convert_color_to_rgb(screen_, &t);  // 16-255 走 vterm 自己的表
    return (uint32_t(t.rgb.red) << 16) | (uint32_t(t.rgb.green) << 8) | t.rgb.blue;
}

void VTermBridge::cursor(int& row, int& col) const {
    VTermPos pos{0, 0};
    vterm_state_get_cursorpos(vterm_obtain_state(vt_), &pos);
    row = pos.row;
    col = pos.col;
}

}  // namespace term
