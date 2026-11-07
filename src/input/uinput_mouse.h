#pragma once
// 虚拟鼠标：只负责点击，不移动指针（指针位置留给真鼠标）
class UInputMouse {
public:
    bool ensure();            // 懒创建 /dev/uinput 设备
    void click(int button);   // 0x110=左键 0x111=右键
    ~UInputMouse();

private:
    int fd_ = -1;
};
