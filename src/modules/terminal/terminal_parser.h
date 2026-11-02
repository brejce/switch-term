#pragma once
// 终端解析层接口：主程序不认识 libvterm，也不认识 libghostty-vt，
// 只认这一层。换解析器不影响 UI、输入法、手柄映射。
#include <cstdint>
#include <cstddef>

namespace term {

struct Cell {
    uint32_t cp = ' ';      // Unicode 码点
    uint32_t fg = 0xc8c8d4; // 前景 RGB
    uint32_t bg = 0x06060a; // 背景 RGB
    bool bold = false;
    bool underline = false;
    bool reverse = false;
    bool wide = false;  // 占两格（宽字符的第一格）
};

class TerminalParser {
public:
    virtual ~TerminalParser() = default;

    virtual bool init(int rows, int cols) = 0;
    virtual void resize(int rows, int cols) = 0;
    virtual void feed(const char* data, size_t len) = 0;

    virtual Cell cellAt(int row, int col) const = 0;
    virtual void cursor(int& row, int& col, bool& visible) const = 0;

    // 滚动历史（可选，默认不支持）
    virtual int historySize() const { return 0; }
    virtual Cell historyAt(int line, int col) const {
        (void)line;
        (void)col;
        return {};
    }

    virtual bool dirty() const = 0;
    virtual void clearDirty() = 0;
    virtual void markDirty() = 0;

    virtual int rows() const = 0;
    virtual int cols() const = 0;
    virtual const char* name() const = 0;
};

// 按名字造解析器：vterm（默认）/ dummy（UI 开发）/ ghostty（未实现时回落到 vterm）
TerminalParser* createParser(const char* name);

}  // namespace term
