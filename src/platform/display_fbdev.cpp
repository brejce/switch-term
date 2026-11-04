// fbdev 后端：直接写 /dev/fb0（Switch/Switchroot 上是同一套）
// 只做 1:1 居中贴图，不做缩放（Switch 恰好 1280×720）
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "platform/display_backend.h"
#include "platform/soft_canvas.h"

namespace {

class FbdevBackend : public disp::DisplayBackend {
public:
    bool init(int w, int h) override {
        const char* env = std::getenv("SIT_FBDEV");
        const char* path = env && *env ? env : "/dev/fb0";
        fbW_ = w;
        fbH_ = h;

        fd_ = ::open(path, O_RDWR | O_CREAT, 0644);  // 自检时路径是普通文件
        if (fd_ < 0) {
            std::fprintf(stderr, "fbdev: 打不开 %s: %s\n", path, std::strerror(errno));
            return false;
        }
        struct stat st{};
        ::fstat(fd_, &st);
        if (S_ISREG(st.st_mode)) {  // 自检用：普通文件当帧缓冲，固定 32bpp
            xres_ = w;
            yres_ = h;
            bpp_ = 32;
            stride_ = w * 4;
            if (::ftruncate(fd_, off_t(stride_) * yres_) != 0) return false;
        } else {
            struct fb_var_screeninfo vi{};
            struct fb_fix_screeninfo fi{};
            if (::ioctl(fd_, FBIOGET_VSCREENINFO, &vi) || ::ioctl(fd_, FBIOGET_FSCREENINFO, &fi)) {
                std::fprintf(stderr, "fbdev: ioctl 失败: %s\n", std::strerror(errno));
                return false;
            }
            xres_ = int(vi.xres);
            yres_ = int(vi.yres);
            bpp_ = int(vi.bits_per_pixel);
            stride_ = int(fi.line_length);
            std::fprintf(stderr, "fbdev: %s %dx%d %dbpp stride=%d\n", fi.id, xres_, yres_, bpp_,
                         stride_);
        }
        if (bpp_ != 32 && bpp_ != 16) {
            std::fprintf(stderr, "fbdev: 不支持 %d bpp\n", bpp_);
            return false;
        }
        mapLen_ = size_t(stride_) * yres_;
        void* m = ::mmap(nullptr, mapLen_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (m == MAP_FAILED) {
            std::fprintf(stderr, "fbdev: mmap 失败: %s\n", std::strerror(errno));
            return false;
        }
        fb_ = static_cast<uint8_t*>(m);
        ox_ = (xres_ - w) / 2;  // 居中
        oy_ = (yres_ - h) / 2;
        if (ox_ < 0) ox_ = 0;
        if (oy_ < 0) oy_ = 0;

        canvas_.resize(w, h);
        enterRaw();
        if (!S_ISREG(st.st_mode)) scanTouch();
        return true;
    }

    void shutdown() override {
        leaveRaw();
        if (fb_) ::munmap(fb_, mapLen_);
        if (touchFd_ >= 0) ::close(touchFd_);
        if (fd_ >= 0) ::close(fd_);
        fb_ = nullptr;
        fd_ = -1;
    }

    disp::Capabilities caps() const override { return {true, false, 0}; }
    disp::TextureId createTexture(const disp::TextureDesc& d) override {
        return canvas_.createTexture(d);
    }
    void destroyTexture(disp::TextureId) override {}
    void updateTexture(disp::TextureId id, const uint8_t* p, int stride) override {
        canvas_.updateTexture(id, p, stride);
    }
    void beginFrame() override { canvas_.clear(); }
    void fillRect(const disp::Rect& r, disp::Color c) override { canvas_.fillRect(r, c); }
    void drawTexture(disp::TextureId id, const disp::Rect& src, const disp::Rect& dst,
                     disp::Color tint) override {
        canvas_.drawTexture(id, src, dst, tint);
    }
    void setClip(const disp::Rect& r) override { canvas_.setClip(r); }
    void clearClip() override { canvas_.clearClip(); }

    void endFrame() override {
        // rot_ = 90/180/270 时做软件旋转（面板原生 720x1280 竖装，画布是 1280x720）
        const uint8_t* px = canvas_.pixels().data();
        const int CW = fbW_, CH = fbH_;
        const int bpc = bpp_ / 8;
        for (int y = 0; y < CH; ++y) {
            const uint8_t* src = px + size_t(y) * size_t(CW) * 4;
            for (int x = 0; x < CW; ++x, src += 4) {
                int fx, fy;
                switch (rot_) {
                    case 90: fx = CH - 1 - y; fy = x; break;
                    case 180: fx = CW - 1 - x; fy = CH - 1 - y; break;
                    case 270: fx = y; fy = CW - 1 - x; break;
                    default: fx = x + ox_; fy = y + oy_; break;
                }
                if (fx < 0 || fx >= xres_ || fy < 0 || fy >= yres_) continue;
                uint8_t* dst = fb_ + size_t(fy) * size_t(stride_) + size_t(fx) * bpc;
                if (bpp_ == 32) {  // XRGB8888 小端
                    dst[0] = src[2];
                    dst[1] = src[1];
                    dst[2] = src[0];
                    dst[3] = 0;
                } else {  // RGB565
                    uint16_t v =
                        uint16_t(((src[0] >> 3) << 11) | ((src[1] >> 2) << 5) | (src[2] >> 3));
                    dst[0] = uint8_t(v & 0xFF);
                    dst[1] = uint8_t(v >> 8);
                }
            }
        }
    }

    bool captureFrame(const std::string&) override { return false; }
    const char* name() const override { return "fbdev"; }

    void setCanvasSize(int w, int h) override {
        fbW_ = w;
        fbH_ = h;
        canvas_.resize(w, h);
        ox_ = std::max(0, (xres_ - w) / 2);
        oy_ = std::max(0, (yres_ - h) / 2);
    }

    void setRotation(int deg) override { rot_ = ((deg % 360) + 360) % 360; }

    // 控制台键盘：stdin 已在 raw 模式，Ctrl+Q 退出，其余原样送 PTY
    bool pump(std::string& keys, std::vector<disp::ClickEvent>& clicks) override {
        pumpTouch(clicks);
        unsigned char buf[64];
        ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
        if (n > 0) {
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == 0x11) return false;  // Ctrl+Q
                keys += char(buf[i]);
            }
        }
        ::usleep(16000);
        return true;
    }

private:
    static bool bitSet(const unsigned long* bits, int bit) {
        constexpr int kWordBits = int(sizeof(unsigned long) * 8);
        return (bits[bit / kWordBits] & (1UL << (bit % kWordBits))) != 0;
    }

    void scanTouch() {
        constexpr int kWords = (ABS_MAX + int(sizeof(unsigned long) * 8)) /
                               int(sizeof(unsigned long) * 8);
        for (int i = 0; i < 64; ++i) {
            char path[64];
            std::snprintf(path, sizeof path, "/dev/input/event%d", i);
            int fd = ::open(path, O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;
            unsigned long absBits[kWords]{};
            if (::ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absBits), absBits) >= 0 &&
                bitSet(absBits, ABS_MT_POSITION_X) && bitSet(absBits, ABS_MT_POSITION_Y) &&
                ::ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &touchXInfo_) == 0 &&
                ::ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &touchYInfo_) == 0) {
                touchFd_ = fd;
                return;
            }
            ::close(fd);
        }
    }

    disp::Point touchPoint() const {
        const int xr = std::max(1, touchXInfo_.maximum - touchXInfo_.minimum);
        const int yr = std::max(1, touchYInfo_.maximum - touchYInfo_.minimum);
        const int px = (touchX_ - touchXInfo_.minimum) * std::max(1, xres_ - 1) / xr;
        const int py = (touchY_ - touchYInfo_.minimum) * std::max(1, yres_ - 1) / yr;
        switch (rot_) {
            case 90:
                return {py * fbW_ / std::max(1, yres_),
                        fbH_ - 1 - px * fbH_ / std::max(1, xres_)};
            case 180:
                return {fbW_ - 1 - px * fbW_ / std::max(1, xres_),
                        fbH_ - 1 - py * fbH_ / std::max(1, yres_)};
            case 270:
                return {fbW_ - 1 - py * fbW_ / std::max(1, yres_),
                        px * fbH_ / std::max(1, xres_)};
            default:
                return {px * fbW_ / std::max(1, xres_),
                        py * fbH_ / std::max(1, yres_)};
        }
    }

    void pumpTouch(std::vector<disp::ClickEvent>& clicks) {
        if (touchFd_ < 0) return;
        input_event ev{};
        while (::read(touchFd_, &ev, sizeof ev) == ssize_t(sizeof ev)) {
            if (ev.type == EV_ABS) {
                if (ev.code == ABS_MT_TRACKING_ID) {
                    if (ev.value >= 0) {
                        touchActive_ = true;
                        touchEnding_ = false;
                        touchHavePoint_ = false;
                        touchMoved_ = false;
                        touchRemainder_ = 0;
                    } else {
                        touchEnding_ = touchActive_;
                        touchActive_ = false;
                    }
                } else if (ev.code == ABS_MT_POSITION_X) {
                    touchX_ = ev.value;
                } else if (ev.code == ABS_MT_POSITION_Y) {
                    touchY_ = ev.value;
                }
            }
            if (ev.type != EV_SYN || ev.code != SYN_REPORT ||
                (!touchActive_ && !touchEnding_))
                continue;

            const disp::Point p = touchPoint();
            if (!touchHavePoint_) {
                touchStart_ = touchLast_ = p;
                touchHavePoint_ = true;
            } else if (touchActive_) {
                touchRemainder_ += p.y - touchLast_.y;
                touchLast_ = p;
                const int rowPx = std::max(8, fbH_ * 21 / 720);
                while (std::abs(touchRemainder_) >= rowPx) {
                    const int d = touchRemainder_ > 0 ? 1 : -1;
                    wheel += d;
                    touchRemainder_ -= d * rowPx;
                }
                touchMoved_ = touchMoved_ || std::abs(p.y - touchStart_.y) >= rowPx / 2;
            }
            if (touchEnding_) {
                if (!touchMoved_ && touchHavePoint_)
                    clicks.push_back({touchLast_.x, touchLast_.y, 1});
                touchEnding_ = false;
                touchHavePoint_ = false;
            }
        }
    }

    void enterRaw() {
        if (!::isatty(STDIN_FILENO)) return;
        if (::tcgetattr(STDIN_FILENO, &old_)) return;
        termios t = old_;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        raw_ = ::tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0;
    }
    void leaveRaw() {
        if (raw_) ::tcsetattr(STDIN_FILENO, TCSANOW, &old_);
        raw_ = false;
    }

    int fd_ = -1;
    uint8_t* fb_ = nullptr;
    size_t mapLen_ = 0;
    int xres_ = 0, yres_ = 0, bpp_ = 32, stride_ = 0;
    int ox_ = 0, oy_ = 0;
    int rot_ = 0;  // engine.display.rotate，由 app 在 init 后调 setRotation
    int fbW_ = 0, fbH_ = 0;
    int touchFd_ = -1;
    input_absinfo touchXInfo_{}, touchYInfo_{};
    int touchX_ = 0, touchY_ = 0, touchRemainder_ = 0;
    bool touchActive_ = false, touchEnding_ = false, touchHavePoint_ = false,
         touchMoved_ = false;
    disp::Point touchStart_{}, touchLast_{};
    disp::SoftCanvas canvas_;
    termios old_{};
    bool raw_ = false;
};

}  // namespace

REGISTER_DISPLAY_BACKEND("fbdev", FbdevBackend)
