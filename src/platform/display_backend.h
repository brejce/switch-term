#pragma once
// 显示后端接口（对齐 IMPLEMENTATION.md §3.1）
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace disp {

enum class PixelFormat { RGBA8888, BGRA8888, A8 };

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
};

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
};

inline Color rgb(uint32_t v, uint8_t a = 255) {
    return Color{uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v), a};
}
inline Color withAlpha(Color c, uint8_t a) { c.a = a; return c; }
inline Color mix(Color a, Color b, float t) {
    auto l = [t](uint8_t x, uint8_t y) {
        return uint8_t(x + (y - x) * t + 0.5f);
    };
    return Color{l(a.r, b.r), l(a.g, b.g), l(a.b, b.b), l(a.a, b.a)};
}

struct Point {
    int x = 0, y = 0;
};

// 鼠标点击：帧缓冲坐标 + 按键（1=左 2=右 3=中）
struct ClickEvent {
    int x = 0, y = 0, button = 1;
};

class DamageList;

struct TextureDesc {
    int width = 0, height = 0;
    PixelFormat format = PixelFormat::RGBA8888;
    bool dynamic = false;
};

using TextureId = uint32_t;

struct Capabilities {
    bool alpha_blend = false;
    bool hw_scale = false;
    int max_texture_size = 0;
};

class DisplayBackend {
public:
    virtual ~DisplayBackend() = default;
    virtual bool init(int width, int height) = 0;
    virtual void shutdown() = 0;
    virtual Capabilities caps() const = 0;
    virtual TextureId createTexture(const TextureDesc&) = 0;
    virtual void destroyTexture(TextureId) = 0;
    virtual void updateTexture(TextureId, const uint8_t* pixels, int row_stride) = 0;
    virtual void beginFrame() = 0;
    // 带脏区的一帧：默认忽略脏区走旧路径（整屏清 + 整屏提交），SDL/headless 不用改
    virtual void beginFrame(const DamageList& d) {
        (void)d;
        beginFrame();
    }
    virtual void fillRect(const Rect&, Color) = 0;
    virtual void drawTexture(TextureId, const Rect& src, const Rect& dst, Color tint) = 0;
    // 裁剪矩形：之后的 fillRect/drawTexture（含字形）只在它里面落地；clearClip = 回到基础裁剪
    virtual void setClip(const Rect&) = 0;
    virtual void clearClip() = 0;
    // 基础裁剪 = 本帧要重画的范围（脏区，由 beginFrame(damage) 的调用方设置）；默认不启用
    virtual void setBaseClip(const Rect&) {}
    virtual void endFrame() = 0;
    virtual void endFrame(const DamageList& d) {
        (void)d;
        endFrame();
    }
    // 窗口分辨率变了：重排画布（UI 会按新尺寸重画）
    virtual void setCanvasSize(int w, int h) { (void)w; (void)h; }
    // 平台方向（engine.display.rotate）：只对 fbdev 这种“面板原生竖装”的后端有意义
    virtual void setRotation(int deg) { (void)deg; }
    // 这个后端希望用多大缩放（画布像素 / 设计尺寸）
    virtual float scaleHint(int designW, int designH) const {
        (void)designW;
        (void)designH;
        return 1.0f;
    }
    virtual bool captureFrame(const std::string& path) { (void)path; return false; }
    // 系统 IME 的 preedit（拼音串）；无窗口后端恒空
    virtual std::string preedit() const { return {}; }
    // 告诉系统 IME「光标在这儿」，候选框才会贴着输入位置弹
    virtual void setTextInputRect(const Rect&) {}
    // 收一帧事件：keys 追加待写入 PTY 的字节，clicks 追加界面点击（帧缓冲坐标）；
    // 返回 false = 退出（关窗）。无窗口后端返回 false，调用方只画一帧
    virtual bool pump(std::string& keys, std::vector<ClickEvent>& clicks) {
        (void)keys;
        (void)clicks;
        return false;
    }
    virtual const char* name() const = 0;
    // 原生窗口句柄（X11：Display* / Window），把 GUI 子程序嵌进我们窗口要用；无窗口后端恒空
    virtual void* nativeDisplay() const { return nullptr; }
    virtual unsigned long nativeWindow() const { return 0; }
    // 这个后端把画布送上屏幕时做了多少度旋转（0/90/180/270）。触摸坐标要按同一个量反向换算：
    // fbdev 自己把 1280x720 画布转进 720x1280 的 fb，所以是 270；X/SDL 窗口就是画布尺寸，是 0
    virtual int displayRotation() const { return 0; }

    // 鼠标位置（帧缓冲坐标，-1 = 指针不在画面内/从没动过）。后端写，主循环读来算悬停格。
    // 默认 -1：无鼠标后端（fbdev/headless）+ 手柄时悬停不能把选中清掉。
    int mouseX = -1, mouseY = -1;

    // 滚轮/翻页累计量（+ 向上看历史）。后端写，主循环取走后清零。
    int wheel = 0;
    // Shift 单击（按下后没敲别的键就抬起）= 中英切换。0 无事件，1 有
    int shiftTap = 0;
    // Ctrl+Shift+C / Ctrl+Shift+V（终端里 Ctrl+C 是中断，剪贴板必须带 Shift）
    int copyReq = 0, pasteReq = 0;
    int uiDirty = 0;
    int reloadReq = 0;  // 手动触发 layout.lua 热重载（Ctrl+Shift+R）  // 窗口尺寸/曝光变化 → 必须补一帧，否则合成器把旧帧拉伸填满
    int selReq = 0;  // 选区步进（Shift+左右 / 手柄 Shift+ZL·ZR），正的往右
    // 双击锁定的修饰键位图：1=Ctrl 2=Shift 4=Alt。锁 Shift 只影响光标/选区组合键，
    // 不改字母大小写（不做 caps）
    int locks = 0;
    // 一次性修饰键（打给下一个键就释放）：1=Ctrl 2=Shift 4=Alt
    int sticky = 0;
    // 物理按住的修饰键：和 sticky 的区别是"松手就没了"（有 release 事件的方向键/肩键才有）
    int holding = 0;
    int heldUsed = 0;  // 按住期间已经被用掉过：松手时不再留一次性修饰
    std::vector<Point> dblClicks;  // 双击（clicks>=2）单独走，用来切换锁定
};

using CreateFn = DisplayBackend* (*)();

class DisplayRegistry {
public:
    static DisplayRegistry& instance();
    void add(const char* name, CreateFn fn);
    DisplayBackend* create(const std::string& name);
    std::vector<std::string> names() const;

private:
    std::map<std::string, CreateFn> map_;
};

struct BackendRegistrar {
    BackendRegistrar(const char* name, CreateFn fn) {
        DisplayRegistry::instance().add(name, fn);
    }
};

}  // namespace disp

#define REGISTER_DISPLAY_BACKEND(NAME, CLS)                       \
    namespace {                                                   \
    disp::DisplayBackend* make_##CLS() { return new CLS(); }       \
    static disp::BackendRegistrar reg_##CLS(NAME, &make_##CLS);    \
    }
