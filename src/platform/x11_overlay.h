#pragma once
// 覆盖层模式：本程序从"自绘终端"退化成压在 openbox 上面的一圈 UI。
// 设备上没有合成器，中间那块"洞"只能从窗口形状里挖掉（XShape）：挖掉的像素不显示、
// 挖掉的输入区域不接收鼠标，于是下面的 alacritty / chromium 原样可见也能直接点。
#include <string>
#include <vector>

#include <X11/Xlib.h>

#include "platform/display_backend.h"

namespace disp {

class X11Overlay {
public:
    // dpy/host = 本进程自己的 X 连接和窗口（SDL 后端的 nativeDisplay / nativeWindow）
    bool init(Display* dpy, Window host);

    // visible = 要保留的矩形（窗口内坐标，屏幕像素）。两个 shape 必须同时设成同一组：
    // ShapeBounding 管"看得见"、ShapeInput 管"点得到"。只设前者的话洞里的鼠标仍被我们收走，
    // 下面的终端永远点不动；只设后者则是看得见但点不穿的灰板。
    // 覆盖层必须永远在最上层（否则普通窗口会盖住顶栏和键区）；
    // 没有 WM 时这一句会被忽略，不影响
    void setAlwaysOnTop();
    // 把 X 焦点交给某个窗口：我们的覆盖层自己不需要焦点（手柄走 evdev），
    // 但它全屏置顶后会变成"活动窗口"，uinput 打的字就会进我们自己这里
    void focus(unsigned long w);
    void applyShape(const std::vector<Rect>& visible, int screenW, int screenH);

    struct Win {
        unsigned long id = 0;
        std::string title;
    };
    // 窗口管理器（openbox）维护的 _NET_CLIENT_LIST，顺序 = 任务栏顺序；跳过 skip_taskbar /
    // 不可见 / 我们自己的窗口
    std::vector<Win> listWindows();
    unsigned long activeWindow();  // _NET_ACTIVE_WINDOW，0 = 读不到

private:
    std::string windowTitle(Window w);

    Display* dpy_ = nullptr;
    Window host_ = 0;
    bool shapeOk_ = false;  // 没有 XShape 扩展就不挖洞（难看但不会崩）
};

}  // namespace disp
