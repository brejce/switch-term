#include "term/vterm_bridge.h"

#include <cstring>

namespace term {

bool VTermBridge::init(int rows, int cols) {
    rows_ = rows;
    cols_ = cols;
    vt_ = vterm_new(rows, cols);
    if (!vt_) return false;
    vterm_set_utf8(vt_, 1);
    screen_ = vterm_obtain_screen(vt_);
    if (!screen_) return false;

    static VTermScreenCallbacks cb;
    std::memset(&cb, 0, sizeof cb);
    cb.damage = [](VTermRect, void* u) {
        static_cast<VTermBridge*>(u)->markDirty();
        return 1;
    };
    cb.movecursor = [](VTermPos, VTermPos, int, void* u) {
        static_cast<VTermBridge*>(u)->markDirty();
        return 1;
    };
    cb.settermprop = [](VTermProp prop, VTermValue* val, void* u) {
        auto* self = static_cast<VTermBridge*>(u);
        if (prop == VTERM_PROP_CURSORVISIBLE) self->cursorVisible_ = val->boolean != 0;
        self->markDirty();
        return 1;
    };
    cb.sb_pushline = [](int cols, const VTermScreenCell* cells, void* u) {
        static_cast<VTermBridge*>(u)->sbPush(cols, cells);
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
