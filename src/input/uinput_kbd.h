#pragma once
// uinput 虚拟键盘：把字符/按键打到"当前焦点程序"，本程序不需要知道对方是谁。
// 这样手柄映射区就能脱离自带终端（Ghostty / bash / 任何程序都能收）。
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

private:
    void tap(int keycode, bool shift);
    void sync();

    int fd_ = -1;
};

}  // namespace input
