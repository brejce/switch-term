#include "platform/x11_overlay.h"

#include <X11/Xatom.h>
#include <X11/extensions/shape.h>

#include <algorithm>
#include <cstdio>

namespace disp {

namespace {

// Xlib 的默认错误处理器直接 exit：_NET_CLIENT_LIST 里可能残留已经退出的窗口 id，
// 读它的属性就是 BadWindow —— 一个程序退出不该带走我们的 UI
int ignoreXError(Display*, XErrorEvent*) { return 0; }

// 这一段里的 X 错误全部吞掉：构造时换上处理器，析构时 XSync 让错误先回来再恢复
struct ErrorGuard {
    Display* dpy;
    XErrorHandler prev;
    explicit ErrorGuard(Display* d) : dpy(d), prev(XSetErrorHandler(ignoreXError)) {}
    ~ErrorGuard() {
        XSync(dpy, False);
        XSetErrorHandler(prev);
    }
};

// 读 32 位格式的属性（_NET_CLIENT_LIST / _NET_WM_STATE / _NET_ACTIVE_WINDOW 都是）
std::vector<unsigned long> getCardinals(Display* dpy, Window w, Atom prop, Atom type) {
    std::vector<unsigned long> out;
    Atom at = 0;
    int fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char* data = nullptr;
    const int rc = XGetWindowProperty(dpy, w, prop, 0, 1024, False, type, &at, &fmt, &n, &after,
                                      &data);
    if (rc != Success || !data) return out;
    if (fmt == 32 && at == type)
        out.assign(reinterpret_cast<unsigned long*>(data),
                   reinterpret_cast<unsigned long*>(data) + n);
    XFree(data);
    return out;
}

// 读字符串属性（format 8）。type 传 0 = AnyPropertyType（WM_NAME 可能是 STRING 或 COMPOUND_TEXT）
std::string getText(Display* dpy, Window w, Atom prop, Atom type) {
    Atom at = 0;
    int fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char* data = nullptr;
    const int rc = XGetWindowProperty(dpy, w, prop, 0, 256, False, type, &at, &fmt, &n, &after,
                                      &data);
    if (rc != Success || !data) return {};
    std::string s;
    if (fmt == 8) s.assign(reinterpret_cast<char*>(data), size_t(n));
    XFree(data);
    return s;
}

}  // namespace

bool X11Overlay::init(Display* dpy, Window host) {
    if (!dpy || !host) return false;
    dpy_ = dpy;
    host_ = host;
    int evBase = 0, errBase = 0;
    shapeOk_ = XShapeQueryExtension(dpy_, &evBase, &errBase) != 0;
    if (!shapeOk_) std::fprintf(stderr, "x11: 没有 XShape 扩展，overlay 挖不出洞\n");
    return true;
}

void X11Overlay::setAlwaysOnTop() {
    if (!dpy_ || !host_) return;
    ErrorGuard guard(dpy_);
    // 用 EWMH 的 _NET_WM_STATE 加 ABOVE：openbox 会把它放到 above 层，普通窗口就在下面了
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = host_;
    ev.xclient.message_type = XInternAtom(dpy_, "_NET_WM_STATE", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 1;  // 1 = _NET_WM_STATE_ADD
    ev.xclient.data.l[1] = long(XInternAtom(dpy_, "_NET_WM_STATE_ABOVE", False));
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = 2;  // source = pager
    XSendEvent(dpy_, DefaultRootWindow(dpy_), False,
               SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XFlush(dpy_);
}

void X11Overlay::focus(unsigned long w) {
    if (!dpy_ || !w || w == host_) return;
    ErrorGuard guard(dpy_);  // frontWin 可能刚好已经被关掉
    XSetInputFocus(dpy_, w, RevertToParent, CurrentTime);
    XFlush(dpy_);
}

void X11Overlay::applyShape(const std::vector<Rect>& visible, int screenW, int screenH) {
    if (!dpy_ || !host_ || !shapeOk_) return;
    std::vector<XRectangle> rects;
    rects.reserve(visible.size());
    for (const Rect& b : visible) {
        // 夹进屏幕：窗口尺寸变了以后旧矩形留在外面会多出一条看不见的可见边
        const int x0 = std::clamp(b.x, 0, screenW), y0 = std::clamp(b.y, 0, screenH);
        const int x1 = std::clamp(b.right(), 0, screenW), y1 = std::clamp(b.bottom(), 0, screenH);
        if (x1 <= x0 || y1 <= y0) continue;
        rects.push_back(XRectangle{short(x0), short(y0), ushort(x1 - x0), ushort(y1 - y0)});
    }
    // ShapeSet = 把形状直接替换成这组矩形（不是并集叠加）：一次调用就等于"先清空再逐个加"，
    // 顺序无关，也不会漏掉清空那一步
    for (int shape : {int(ShapeBounding), int(ShapeInput)}) {
        XShapeCombineRectangles(dpy_, host_, shape, 0, 0, rects.data(), int(rects.size()),
                                ShapeSet, Unsorted);
    }
    XFlush(dpy_);
}

std::string X11Overlay::windowTitle(Window w) {
    std::string s = getText(dpy_, w, XInternAtom(dpy_, "_NET_WM_NAME", False),
                            XInternAtom(dpy_, "UTF8_STRING", False));
    // 老程序只写 WM_NAME（可能是 latin1/COMPOUND_TEXT，非 UTF-8 时中文会乱，能用就行）
    if (s.empty()) s = getText(dpy_, w, XA_WM_NAME, 0);
    return s;
}

std::vector<X11Overlay::Win> X11Overlay::listWindows() {
    std::vector<Win> out;
    if (!dpy_) return out;
    ErrorGuard guard(dpy_);  // 下面每个属性读都可能撞到已经销毁的窗口
    const Window root = DefaultRootWindow(dpy_);
    const std::vector<unsigned long> ids =
        getCardinals(dpy_, root, XInternAtom(dpy_, "_NET_CLIENT_LIST", False), XA_WINDOW);
    if (ids.empty()) return out;
    const Atom wmState = XInternAtom(dpy_, "_NET_WM_STATE", False);
    const Atom skipTaskbar = XInternAtom(dpy_, "_NET_WM_STATE_SKIP_TASKBAR", False);
    const Atom skipPager = XInternAtom(dpy_, "_NET_WM_STATE_SKIP_PAGER", False);
    for (unsigned long id : ids) {
        if (!id || id == host_) continue;
        XWindowAttributes attr;
        if (!XGetWindowAttributes(dpy_, Window(id), &attr)) continue;
        if (attr.map_state != IsViewable) continue;  // 最小化/还没显示的不进列表
        // skip_taskbar / skip_pager 的（chromium 的辅助窗、通知气泡）不该出现能切过去的列表里
        const std::vector<unsigned long> st = getCardinals(dpy_, Window(id), wmState, XA_ATOM);
        bool skip = false;
        for (size_t i = 0; i < st.size() && !skip; ++i)
            skip = st[i] == skipTaskbar || st[i] == skipPager;
        if (skip) continue;
        out.push_back({id, windowTitle(Window(id))});
    }
    return out;
}

unsigned long X11Overlay::activeWindow() {
    if (!dpy_) return 0;
    ErrorGuard guard(dpy_);
    const std::vector<unsigned long> v = getCardinals(
        dpy_, DefaultRootWindow(dpy_), XInternAtom(dpy_, "_NET_ACTIVE_WINDOW", False), XA_WINDOW);
    return v.empty() ? 0 : v[0];
}

}  // namespace disp
