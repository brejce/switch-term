// fbdev 后端：直接写 /dev/fb0（Switch/Switchroot 上是同一套）
// 只做 1:1 居中贴图，不做缩放（Switch 恰好 1280×720）
#include <fcntl.h>
#include <linux/fb.h>
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

#include "platform/damage.h"
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
        return true;
    }

    void shutdown() override {
        leaveRaw();
        if (fb_) ::munmap(fb_, mapLen_);
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
    // 局部重画：不清画布，只有脏区会被重画；没画到的像素跟 fb 一样停在上一帧
    void beginFrame(const disp::DamageList&) override {}
    void fillRect(const disp::Rect& r, disp::Color c) override { canvas_.fillRect(r, c); }
    void drawTexture(disp::TextureId id, const disp::Rect& src, const disp::Rect& dst,
                     disp::Color tint) override {
        canvas_.drawTexture(id, src, dst, tint);
    }
    void setClip(const disp::Rect& r) override { canvas_.setClip(r); }
    void clearClip() override { canvas_.clearClip(); }
    void setBaseClip(const disp::Rect& r) override { canvas_.setBaseClip(r); }

    void endFrame() override { blit(disp::Rect{0, 0, fbW_, fbH_}); }
    void endFrame(const disp::DamageList& d) override {
        for (int i = 0; i < d.size(); ++i) blit(d.data()[i]);
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
        (void)clicks;  // 触屏已搬到 input::EvdevTouch，后端不再产点击
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
    // 把画布上的一块（画布坐标）搬进 fb。rot_ = 90/180/270 时一并做旋转
    // （面板原生 720x1280 竖装，画布是 1280x720）。
    // 按 16×16 小块遍历：块内读是一行里连续 64B、写是一段连续 64B，两边都落在
    // 同一条缓存行里。原来按行扫、旋转时写侧是跨列的，实测空闲就把一个核吃满。
    void blit(const disp::Rect& r0) {
        disp::Rect r = r0;
        if (r.x < 0) {
            r.w += r.x;
            r.x = 0;
        }
        if (r.y < 0) {
            r.h += r.y;
            r.y = 0;
        }
        if (r.right() > fbW_) r.w = fbW_ - r.x;
        if (r.bottom() > fbH_) r.h = fbH_ - r.y;
        if (r.empty()) return;

        const uint8_t* px = canvas_.pixels().data();
        const int CW = fbW_, CH = fbH_;
        const int bpc = bpp_ / 8;
        auto put = [&](int fx, int fy, const uint8_t* src) {
            if (fx < 0 || fx >= xres_ || fy < 0 || fy >= yres_) return;
            uint8_t* dst = fb_ + size_t(fy) * size_t(stride_) + size_t(fx) * size_t(bpc);
            if (bpp_ == 32) {  // XRGB8888 小端
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                dst[3] = 0;
            } else {  // RGB565
                const uint16_t v =
                    uint16_t(((src[0] >> 3) << 11) | ((src[1] >> 2) << 5) | (src[2] >> 3));
                dst[0] = uint8_t(v & 0xFF);
                dst[1] = uint8_t(v >> 8);
            }
        };
        constexpr int kTile = 16;  // 16px × 4B = 64B = 一条缓存行
        for (int y0 = r.y; y0 < r.bottom(); y0 += kTile) {
            const int yEnd = std::min(y0 + kTile, r.bottom());
            for (int x0 = r.x; x0 < r.right(); x0 += kTile) {
                const int xEnd = std::min(x0 + kTile, r.right());
                for (int y = y0; y < yEnd; ++y) {
                    const uint8_t* src = px + (size_t(y) * size_t(CW) + size_t(x0)) * 4;
                    for (int x = x0; x < xEnd; ++x, src += 4) {
                        switch (rot_) {
                            case 90: put(CH - 1 - y, x, src); break;
                            case 180: put(CW - 1 - x, CH - 1 - y, src); break;
                            case 270: put(y, CW - 1 - x, src); break;
                            default: put(x + ox_, y + oy_, src); break;
                        }
                    }
                }
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
    disp::SoftCanvas canvas_;
    termios old_{};
    bool raw_ = false;
};

}  // namespace

REGISTER_DISPLAY_BACKEND("fbdev", FbdevBackend)
