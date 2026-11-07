#pragma once
// uinput 虚拟键盘：把字符/按键打到"当前焦点程序"，本程序不需要知道对方是谁。
// 这样手柄映射区就能脱离自带终端（Ghostty / bash / 任何程序都能收）。
#include <cstddef>
#include <string>

namespace input {

class UinputKeyboard {
public:
    ~UinputKeyboard() { close(); }

    bool open(const char* name = "switch-ime-term virtual keyboard");
    void close();
    bool ok() const { return fd_ >= 0; }

    // 可打印 ASCII + 常见控制码（\r \t \x7f 转义序列）都走这里
    void typeBytes(const std::string& bytes, int gapMs = 2);

    // Alt+↑ / Alt+↓：overlay 模式下由窗口管理器（openbox）的快捷键切窗口用。
    // 只能是 Alt：X 里 Tab 不是修饰键，绑不出「Tab+↑」这种组合，Alt 才行
    void tapAltUp();
    void tapAltDown();

    // 打不出来的非 ASCII 字符数（uinput 只发 EV_KEY 1..127），调用方退出时统一报
    std::size_t dropped() const { return dropped_; }

private:
    void tap(int keycode, int mod);  // mod = 同时按住的修饰键键码，0 = 不按
    void sync();

    int fd_ = -1;
    std::size_t dropped_ = 0;
};

}  // namespace input
