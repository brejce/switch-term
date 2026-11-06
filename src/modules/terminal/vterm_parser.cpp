// libvterm 实现：包住已验证的 term::VTermBridge（宽字符、256 色、sb_pushline 都在里面）
#include "modules/terminal/terminal_parser.h"

#include "term/vterm_bridge.h"

namespace term {
namespace {

class VTermParser : public TerminalParser {
public:
    bool init(int rows, int cols) override { return bridge_.init(rows, cols); }
    void resize(int rows, int cols) override { bridge_.resize(rows, cols); }
    void feed(const char* data, size_t len) override { bridge_.feed(data, len); }

    Cell cellAt(int row, int col) const override {
        term::CellView v = bridge_.cellAt(row, col);
        Cell c;
        c.cp = v.cp;
        c.fg = v.fg;
        c.bg = v.bg;
        c.bold = v.bold;
        c.underline = v.underline;
        c.reverse = v.reverse;
        c.wide = v.wide;
        return c;
    }
    void cursor(int& row, int& col, bool& visible) const override {
        bridge_.cursor(row, col);
        visible = bridge_.cursorVisible();
    }

    void setHistoryLimit(int lines) override {
        bridge_.setHistoryLimit(lines < 0 ? 0 : size_t(lines));
    }
    int historySize() const override { return bridge_.historySize(); }
    Cell historyAt(int line, int col) const override {
        term::CellView v = bridge_.historyAt(line, col);
        Cell c;
        c.cp = v.cp;
        c.fg = v.fg;
        c.bg = v.bg;
        c.bold = v.bold;
        c.underline = v.underline;
        c.reverse = v.reverse;
        c.wide = v.wide;
        return c;
    }

    bool dirty() const override { return bridge_.dirty(); }
    void clearDirty() override { bridge_.clearDirty(); }
    void markDirty() override { bridge_.markDirty(); }
    bool rowDirty(int row) const override { return bridge_.rowDirty(row); }
    void markRowDirty(int row) override { bridge_.markRowDirty(row); }

    int rows() const override { return bridge_.rowsNow(); }
    int cols() const override { return bridge_.colsNow(); }
    const char* name() const override { return "vterm"; }

private:
    term::VTermBridge bridge_;
};

}  // namespace

TerminalParser* createParser(const char* name) {
    // ghostty 还没接（libghostty-vt 未集成），先回落到 vterm，接口保持不变
    (void)name;
    return new VTermParser();
}

}  // namespace term
