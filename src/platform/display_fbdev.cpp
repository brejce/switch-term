// fbdev 后端：直接写 /dev/fb0（Switch/Switchroot 上是同一套）
// 只做 1:1 居中贴图，不做缩放（Switch 恰好 1280×720）
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

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
    void fillRect(const disp::Rect& r, disp::Color c) override { canvas_.fillRect(r, c); }
    void drawTexture(disp::TextureId id, const disp::Rect& src, const disp::Rect& dst,
                     disp::Color tint) override {
        canvas_.drawTexture(id, src, dst, tint);
    }

    void endFrame() override {
        const uint8_t* px = canvas_.pixels().data();
        for (int y = 0; y < fbH_; ++y) {
            int fy = y + oy_;
            if (fy < 0 || fy >= yres_) break;
            uint8_t* dst = fb_ + size_t(fy) * stride_ + size_t(ox_) * (bpp_ / 8);
            const uint8_t* src = px + size_t(y) * fbW_ * 4;
            if (bpp_ == 32) {
                for (int x = 0; x < fbW_; ++x, src += 4, dst += 4) {  // XRGB8888 小端
                    dst[0] = src[2];
                    dst[1] = src[1];
                    dst[2] = src[0];
                    dst[3] = 0;
                }
            } else {  // RGB565
                for (int x = 0; x < fbW_; ++x, src += 4, dst += 2) {
                    uint16_t v = uint16_t(((src[0] >> 3) << 11) | ((src[1] >> 2) << 5) | (src[2] >> 3));
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

    // 控制台键盘：stdin 已在 raw 模式，Ctrl+Q 退出，其余原样送 PTY
    bool pump(std::string& keys, std::vector<disp::ClickEvent>& clicks) override {
        (void)clicks;
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
    int fbW_ = 0, fbH_ = 0;
    disp::SoftCanvas canvas_;
    termios old_{};
    bool raw_ = false;
};

}  // namespace

REGISTER_DISPLAY_BACKEND("fbdev", FbdevBackend)
