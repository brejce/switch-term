#include "input/uinput_mouse.h"

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace {

void send(int fd, int type, int code, int value) {
    input_event ev;
    std::memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    ::write(fd, &ev, sizeof ev);
}

}  // namespace

UInputMouse::~UInputMouse() {
    if (fd_ >= 0) {
        ::ioctl(fd_, UI_DEV_DESTROY);
        ::close(fd_);
    }
}

bool UInputMouse::ensure(int screenW, int screenH) {
    if (fd_ >= 0) return true;
    fd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd_ < 0) return false;
    ::ioctl(fd_, UI_SET_EVBIT, EV_KEY);
    ::ioctl(fd_, UI_SET_EVBIT, EV_ABS);
    ::ioctl(fd_, UI_SET_EVBIT, EV_SYN);
    ::ioctl(fd_, UI_SET_KEYBIT, BTN_LEFT);
    ::ioctl(fd_, UI_SET_KEYBIT, BTN_RIGHT);
    ::ioctl(fd_, UI_SET_ABSBIT, ABS_X);
    ::ioctl(fd_, UI_SET_ABSBIT, ABS_Y);
    struct uinput_abs_setup ax = {.code = ABS_X,
                                  .absinfo = {.value = 0, .minimum = 0, .maximum = screenW - 1}};
    struct uinput_abs_setup ay = {.code = ABS_Y,
                                  .absinfo = {.value = 0, .minimum = 0, .maximum = screenH - 1}};
    ::ioctl(fd_, UI_ABS_SETUP, &ax);
    ::ioctl(fd_, UI_ABS_SETUP, &ay);
    // 用 UI_DEV_SETUP：老的 uinput_user_dev 写法在这台机器上建出的设备 X 认得但事件不生效
    struct uinput_setup us;
    std::memset(&us, 0, sizeof us);
    std::snprintf(us.name, UINPUT_MAX_NAME_SIZE, "switch-ime-term touch pointer");
    us.id.bustype = BUS_USB;
    us.id.vendor = 0x1209;
    us.id.product = 0x0002;
    us.id.version = 1;
    if (::ioctl(fd_, UI_DEV_SETUP, &us) < 0 || ::ioctl(fd_, UI_DEV_CREATE) < 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    ::usleep(400 * 1000);  // 等 udev/libinput 认到设备，太短会丢前几个事件
    return true;
}

void UInputMouse::moveTo(int x, int y) {
    if (fd_ < 0) return;
    send(fd_, EV_ABS, ABS_X, x);
    send(fd_, EV_ABS, ABS_Y, y);
    sync();
}

void UInputMouse::click(int button) {
    if (fd_ < 0) return;
    send(fd_, EV_KEY, button, 1);
    sync();
    ::usleep(40 * 1000);
    send(fd_, EV_KEY, button, 0);
    sync();
}

void UInputMouse::sync() {
    if (fd_ < 0) return;
    send(fd_, EV_SYN, SYN_REPORT, 0);
}

