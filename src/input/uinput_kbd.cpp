#include "input/uinput_kbd.h"

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace input {
namespace {

struct KeyStroke {
    int key = 0;
    bool shift = false;
};

// ASCII → Linux 键码（US 布局）。0 = 打不出来
KeyStroke asciiToKey(char ch) {
    if (ch >= 'a' && ch <= 'z') return {KEY_A + (ch - 'a'), false};
    if (ch >= 'A' && ch <= 'Z') return {KEY_A + (ch - 'A'), true};
    if (ch >= '1' && ch <= '9') return {KEY_1 + (ch - '1'), false};
    switch (ch) {
        case '0': return {KEY_0, false};
        case ' ': return {KEY_SPACE, false};
        case '\n': return {KEY_ENTER, false};
        case '\r': return {KEY_ENTER, false};
        case '\t': return {KEY_TAB, false};
        case '\x7f': return {KEY_BACKSPACE, false};
        case '-': return {KEY_MINUS, false};
        case '_': return {KEY_MINUS, true};
        case '=': return {KEY_EQUAL, false};
        case '+': return {KEY_EQUAL, true};
        case '[': return {KEY_LEFTBRACE, false};
        case '{': return {KEY_LEFTBRACE, true};
        case ']': return {KEY_RIGHTBRACE, false};
        case '}': return {KEY_RIGHTBRACE, true};
        case '\\': return {KEY_BACKSLASH, false};
        case '|': return {KEY_BACKSLASH, true};
        case ';': return {KEY_SEMICOLON, false};
        case ':': return {KEY_SEMICOLON, true};
        case '\'': return {KEY_APOSTROPHE, false};
        case '"': return {KEY_APOSTROPHE, true};
        case '`': return {KEY_GRAVE, false};
        case '~': return {KEY_GRAVE, true};
        case ',': return {KEY_COMMA, false};
        case '<': return {KEY_COMMA, true};
        case '.': return {KEY_DOT, false};
        case '>': return {KEY_DOT, true};
        case '/': return {KEY_SLASH, false};
        case '?': return {KEY_SLASH, true};
        case '!': return {KEY_1, true};
        case '@': return {KEY_2, true};
        case '#': return {KEY_3, true};
        case '$': return {KEY_4, true};
        case '%': return {KEY_5, true};
        case '^': return {KEY_6, true};
        case '&': return {KEY_7, true};
        case '*': return {KEY_8, true};
        case '(': return {KEY_9, true};
        case ')': return {KEY_0, true};
        default: return {};
    }
}

}  // namespace

bool UinputKeyboard::open(const char* name) {
    fd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd_ < 0) {
        fd_ = ::open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
    }
    if (fd_ < 0) return false;

    ::ioctl(fd_, UI_SET_EVBIT, EV_KEY);
    ::ioctl(fd_, UI_SET_EVBIT, EV_SYN);
    for (int k = 1; k < 128; ++k) ::ioctl(fd_, UI_SET_KEYBIT, k);  // 够 ASCII 用

    uinput_user_dev dev;
    std::memset(&dev, 0, sizeof dev);
    std::snprintf(dev.name, UINPUT_MAX_NAME_SIZE, "%s", name);
    dev.id.bustype = BUS_USB;
    dev.id.vendor = 0x1209;   // 自定义
    dev.id.product = 0x0001;
    dev.id.version = 1;
    if (::write(fd_, &dev, sizeof dev) < 0) {
        std::fprintf(stderr, "uinput: 写入设备描述失败: %s\n", std::strerror(errno));
        close();
        return false;
    }
    if (::ioctl(fd_, UI_DEV_CREATE) < 0) {
        std::fprintf(stderr, "uinput: UI_DEV_CREATE 失败: %s\n", std::strerror(errno));
        close();
        return false;
    }
    // 等 udev → libinput → compositor 认到设备。太短会整串丢键（实测 120ms 不稳）
    ::usleep(std::getenv("SIT_UINPUT_DELAY") ? std::atoi(std::getenv("SIT_UINPUT_DELAY")) * 1000
                                             : 500 * 1000);
    return true;
}

void UinputKeyboard::close() {
    if (fd_ >= 0) {
        ::ioctl(fd_, UI_DEV_DESTROY);
        ::close(fd_);
        fd_ = -1;
    }
}

void UinputKeyboard::sync() {
    input_event ev;
    std::memset(&ev, 0, sizeof ev);
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ::write(fd_, &ev, sizeof ev);
}

void UinputKeyboard::tap(int keycode, bool shift) {
    if (fd_ < 0 || !keycode) return;
    auto send = [&](int code, int value) {
        input_event ev;
        std::memset(&ev, 0, sizeof ev);
        ev.type = EV_KEY;
        ev.code = code;
        ev.value = value;
        ::write(fd_, &ev, sizeof ev);
    };
    if (shift) send(KEY_LEFTSHIFT, 1);
    send(keycode, 1);
    sync();
    send(keycode, 0);
    if (shift) send(KEY_LEFTSHIFT, 0);
    sync();
}

void UinputKeyboard::typeBytes(const std::string& bytes, int gapMs) {
    for (size_t i = 0; i < bytes.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(bytes[i]);
        if (ch == 0x1b) {  // 转义序列：把常见的映射成真按键
            int key = 0;
            if (i + 2 < bytes.size() && bytes[i + 1] == '[') {
                switch (bytes[i + 2]) {
                    case 'A': key = KEY_UP; i += 2; break;
                    case 'B': key = KEY_DOWN; i += 2; break;
                    case 'C': key = KEY_RIGHT; i += 2; break;
                    case 'D': key = KEY_LEFT; i += 2; break;
                    case 'H': key = KEY_HOME; i += 2; break;
                    case 'F': key = KEY_END; i += 2; break;
                    case 'Z': key = KEY_TAB; i += 2; break;  // Shift+Tab 先当 Tab
                    default:
                        if (i + 3 < bytes.size()) {
                            if (bytes[i + 2] == '3' && bytes[i + 3] == '~') {
                                key = KEY_DELETE;
                                i += 3;
                            } else if (bytes[i + 2] == '2' && bytes[i + 3] == '~') {
                                key = KEY_INSERT;
                                i += 3;
                            } else if (bytes[i + 2] == '5' && bytes[i + 3] == '~') {
                                key = KEY_PAGEUP;
                                i += 3;
                            } else if (bytes[i + 2] == '6' && bytes[i + 3] == '~') {
                                key = KEY_PAGEDOWN;
                                i += 3;
                            }
                        }
                        break;
                }
            } else {
                key = KEY_ESC;  // 单独一个 ESC
            }
            tap(key, false);
        } else if (ch >= 0x80) {
            // uinput 不送 Unicode，UTF-8 多字节没法用键码表达（中文要靠系统输入法）
            ++dropped_;
            while (i + 1 < bytes.size() && (static_cast<unsigned char>(bytes[i + 1]) & 0xC0) == 0x80)
                ++i;  // 跳完这个 UTF-8 字符的延续字节，一个字符只算一次
        } else {
            KeyStroke ks = asciiToKey(char(ch));
            tap(ks.key, ks.shift);
        }
        if (gapMs > 0) ::usleep(useconds_t(gapMs) * 1000);
    }
}

}  // namespace input
