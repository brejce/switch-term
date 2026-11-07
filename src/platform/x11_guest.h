#pragma once
// 把别的程序开的 X 窗口嵌进我们窗口里（设备上没有窗口管理器，位置和大小全是我们自己算）
#include <sys/types.h>

#include <X11/Xlib.h>

namespace disp {

class X11Guest {
public:
    // dpy/host = 本进程自己的 Display 和窗口（SDL 后端的 nativeDisplay/nativeWindow）
    bool init(Display* dpy, Window host);
    void shutdown();

    // 根窗口的直接子窗口里找 shell 起的程序开的那个窗；找不到返回 0
    unsigned long findChildWindow(pid_t shellPid);
    // 把 win 摆到宿主的 (x,y) 处并设成 w×h（窗口由调用方按 tab 记住）
    bool adopt(unsigned long win, int x, int y, int w, int h);
    void show(unsigned long win, bool on);
    // 交还给根窗口，免得我们的窗口销毁时把它一起带走
    void release(unsigned long win);
    // 该窗口还在不在（收编之后可能会被 destroy，也可能被程序自己重建 → 调用方要重新对账）
    bool alive(unsigned long win);

private:
    bool childOf(Window parent, unsigned long win) const;

    Display* dpy_ = nullptr;  // 自己开的连接：SDL 会吞掉同一个 Display 上不属于它的事件
    Window host_ = 0;
};

}  // namespace disp
