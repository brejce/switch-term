#pragma once
// 虚拟鼠标：绝对坐标移动 + 按键。
// 用途：X 把这台触摸屏当成"绝对坐标鼠标"（没有 INPUT_PROP_DIRECT / BTN_TOOL_FINGER），
// 只出 motion 不出按下 —— 所以我们自己读触摸，再用这个标准鼠标把移动和点击打进系统。
class UInputMouse {
public:
    // 懒创建；量程 = 屏幕像素（给绝对坐标用，overlay 的触摸指针走这条）
    bool ensure(int screenW = 1280, int screenH = 720);
    void moveTo(int x, int y);
    void click(int button);                 // 0x110=左键 0x111=右键
    bool ok() const { return fd_ >= 0; }
    ~UInputMouse();

private:
    void sync();
    int fd_ = -1;
};

