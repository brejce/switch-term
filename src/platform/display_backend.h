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
    virtual void fillRect(const Rect&, Color) = 0;
    virtual void drawTexture(TextureId, const Rect& src, const Rect& dst, Color tint) = 0;
    virtual void endFrame() = 0;
    // 窗口分辨率变了：重排画布（UI 会按新尺寸重画）
    virtual void setCanvasSize(int w, int h) { (void)w; (void)h; }
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
    virtual bool pump(std::string& keys, std::vector<Point>& clicks) {
        (void)keys;
        (void)clicks;
        return false;
    }
    virtual const char* name() const = 0;

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
