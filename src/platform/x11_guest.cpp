#include "platform/x11_guest.h"

#include <X11/Xatom.h>

#include <cstdio>
#include <cstring>

namespace disp {
namespace {

// Xlib 的默认错误处理是直接 exit：对已经销毁或还不 viewable 的窗口 reparent/map/抢焦点
// 都可能报 BadWindow/BadMatch，一个子程序的退出不该带走我们全部的终端
int ignoreXError(Display*, XErrorEvent*) { return 0; }

// 这几个请求的错误在当前作用域内被吞掉（XSync 保证错误在处理完之前回来）。
// 只在收编/显示/交还时用，主循环里没有 X 错误可吞
struct ErrorGuard {
    Display* dpy;
    XErrorHandler prev;
    explicit ErrorGuard(Display* d) : dpy(d), prev(XSetErrorHandler(ignoreXError)) {}
    ~ErrorGuard() {
        XSync(dpy, False);
        XSetErrorHandler(prev);
    }
};

// 窗口的 _NET_WM_PID（CARDINAL）；没有这个属性返回 -1
long windowPid(Display* dpy, Window w) {
    Atom type = 0;
    int fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char* data = nullptr;
    const Atom pidAtom = XInternAtom(dpy, "_NET_WM_PID", False);
    const int rc = XGetWindowProperty(dpy, w, pidAtom, 0, 1, False, XA_CARDINAL, &type, &fmt, &n,
                                      &after, &data);
    if (rc != Success || !data) return -1;  // 属性不存在 = 程序没报 PID
    const long pid = (fmt == 32 && n >= 1) ? long(reinterpret_cast<unsigned long*>(data)[0]) : -1;
    XFree(data);
    return pid;
}

// /proc/<pid>/stat 的第 4 个字段是 ppid；comm 里可能有空格和括号，所以从最后一个 ')' 往后读
pid_t parentPid(pid_t pid) {
    char path[64], buf[512];
    std::snprintf(path, sizeof path, "/proc/%d/stat", int(pid));
    std::FILE* f = std::fopen(path, "r");
    if (!f) return 0;
    const bool ok = std::fgets(buf, sizeof buf, f) != nullptr;
    std::fclose(f);
    if (!ok) return 0;
    const char* rp = std::strrchr(buf, ')');
    int pp = 0;
    return rp && std::sscanf(rp + 1, " %*c %d", &pp) == 1 ? pid_t(pp) : 0;
}

// shell 起的 GUI 程序常常还隔着一两层包装脚本，往上追 8 层足够；
// /proc 没挂时 parentPid 返回 0，退化成只认相等（最多嵌不进去，不会误认）
bool fromShell(pid_t pid, pid_t shellPid) {
    for (int depth = 0; depth < 8 && pid > 0; ++depth) {
        if (pid == shellPid) return true;
        const pid_t pp = parentPid(pid);
        if (pp <= 0) return false;
        pid = pp;
    }
    return false;
}

}  // namespace

bool X11Guest::init(Display* dpy, Window host) {
    if (!dpy || !host) return false;
    dpy_ = XOpenDisplay(DisplayString(dpy));
    if (!dpy_) {
        std::fprintf(stderr, "x11: 打不开第二条 X 连接，GUI 子程序不嵌\n");
        return false;
    }
    host_ = host;
    return true;
}

void X11Guest::shutdown() {
    if (dpy_) XCloseDisplay(dpy_);  // 子窗口先由 release() 交还，这里只管关连接
    dpy_ = nullptr;
    host_ = 0;
    guest_ = 0;
    gone_ = false;
}

// XQueryTree 不会因为窗口不存在而报错（XGetWindowAttributes 会 → Xlib 直接 exit），
// 所以"这个 id 还在不在"一律用它问
bool X11Guest::childOf(Window parent, unsigned long win) const {
    Window root = 0, par = 0, *kids = nullptr;
    unsigned n = 0;
    if (!XQueryTree(dpy_, parent, &root, &par, &kids, &n)) return false;
    bool found = false;
    for (unsigned i = 0; i < n && !found; ++i) found = kids[i] == win;
    if (kids) XFree(kids);
    return found;
}

unsigned long X11Guest::findChildWindow(pid_t shellPid) {
    if (!dpy_ || shellPid <= 0) return 0;
    // 没有窗口管理器，程序开的顶层窗口就挂在根窗口下（不是挂在我们的窗口下）
    Window root = 0, par = 0, *kids = nullptr;
    unsigned n = 0;
    if (!XQueryTree(dpy_, DefaultRootWindow(dpy_), &root, &par, &kids, &n)) return 0;
    // 一个程序常开好几个顶层窗口（chromium 就有主窗 + 200x200 辅助窗 + 剪贴板窗），
    // 挑面积最大的那个 *已 map* 的窗口，否则会嵌进一个看不见的小窗
    unsigned long best = 0;
    long bestArea = 0;
    ErrorGuard guard(dpy_);  // XGetWindowAttributes 对已销毁窗口会报错，别让它带走终端
    for (unsigned i = 0; i < n; ++i) {
        if (kids[i] == host_) continue;
        XWindowAttributes attr;
        if (!XGetWindowAttributes(dpy_, kids[i], &attr)) continue;
        if (attr.map_state != IsViewable) continue;  // 还没显示出来的（启动期窗口）不算
        const long area = long(attr.width) * long(attr.height);
        if (area <= bestArea) continue;
        const long pid = windowPid(dpy_, kids[i]);
        if (pid > 0 && fromShell(pid_t(pid), shellPid)) {
            best = kids[i];
            bestArea = area;
        }
    }
    if (kids) XFree(kids);
    return best;
}

bool X11Guest::adopt(unsigned long win, int x, int y, int w, int h) {
    if (!dpy_ || !host_ || !win) return false;
    // 先确认它还在：记着的是上一个 tab 的窗口时，程序可能早就退出了
    if (!childOf(host_, win) && !childOf(DefaultRootWindow(dpy_), win)) return false;
    ErrorGuard guard(dpy_);
    if (guest_ && guest_ != win) XUnmapWindow(dpy_, guest_);  // 一个宿主只摆一个程序，换 tab 先收上一个
    guest_ = win;
    gone_ = false;
    // 没有窗口管理器，坐标就是我们算的像素，1:1 不缩放
    XReparentWindow(dpy_, win, host_, x, y);
    XResizeWindow(dpy_, win, w, h);
    XSelectInput(dpy_, win, StructureNotifyMask);  // 只想知道它被销毁；mask 是每条连接各自记的
    XMapRaised(dpy_, win);
    XSetInputFocus(dpy_, win, RevertToParent, CurrentTime);
    XFlush(dpy_);
    return true;
}

void X11Guest::show(bool on) {
    if (!dpy_ || !guest_) return;
    ErrorGuard guard(dpy_);
    if (on)
        XMapRaised(dpy_, guest_);
    else
        XUnmapWindow(dpy_, guest_);
    XFlush(dpy_);
}

void X11Guest::release() {
    if (!dpy_ || !guest_) return;
    {
        // 回根窗口放 (0,0)：我们退出后它至少还活着，不会跟着宿主窗口一起被销毁
        ErrorGuard guard(dpy_);
        XReparentWindow(dpy_, guest_, DefaultRootWindow(dpy_), 0, 0);
        XFlush(dpy_);
    }
    guest_ = 0;
    gone_ = false;
}

bool X11Guest::alive() {
    if (!dpy_ || !guest_) return false;
    // 只取这条连接上收编窗口的 StructureNotify：不碰 SDL 的事件队列，也不会阻塞主循环
    XEvent ev;
    while (XCheckWindowEvent(dpy_, guest_, StructureNotifyMask, &ev)) {
        if (ev.type == DestroyNotify) gone_ = true;
    }
    return !gone_;
}

}  // namespace disp
