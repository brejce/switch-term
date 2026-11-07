#include "input/uinput_mouse.h"

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <cstdio>

#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
void emit(int fd, int type, int code, int value) {
    input_event ev{};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    (void)!::write(fd, &ev, sizeof ev);
}
}  // namespace

bool UInputMouse::ensure() {
    if (fd_ >= 0) return true;
    fd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd_ < 0) {
        fprintf(stderr, "uinput: 打不开 /dev/uinput（权限？）\n");
        return false;
    }
    ::ioctl(fd_, UI_SET_EVBIT, EV_KEY);
    ::ioctl(fd_, UI_SET_EVBIT, EV_SYN);
    ::ioctl(fd_, UI_SET_KEYBIT, BTN_LEFT);
    ::ioctl(fd_, UI_SET_KEYBIT, BTN_RIGHT);

    uinput_setup us{};
    us.id.bustype = BUS_USB;
    us.id.vendor = 0x1209;
    us.id.product = 0x5154;
    snprintf(us.name, sizeof us.name, "switch-ime-term virtual mouse");
    if (::ioctl(fd_, UI_DEV_SETUP, &us) < 0 || ::ioctl(fd_, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "uinput: 创建设备失败\n");
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

void UInputMouse::click(int button) {
    if (!ensure()) return;
    emit(fd_, EV_KEY, button, 1);
    emit(fd_, EV_SYN, SYN_REPORT, 0);
    emit(fd_, EV_KEY, button, 0);
    emit(fd_, EV_SYN, SYN_REPORT, 0);
}

UInputMouse::~UInputMouse() {
    if (fd_ >= 0) {
        ::ioctl(fd_, UI_DEV_DESTROY);
        ::close(fd_);
    }
}
