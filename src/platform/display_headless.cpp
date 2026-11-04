#include "platform/display_backend.h"
#include "platform/soft_canvas.h"

#include <png.h>

#include <cstdio>
#include <vector>

namespace {

// 无窗口后端：出 PNG，供 CI 像素回归和肉眼核对
class HeadlessBackend : public disp::DisplayBackend {
public:
    bool init(int w, int h) override {
        canvas_.resize(w, h);
        return true;
    }
    void shutdown() override {}
    disp::Capabilities caps() const override { return {true, true, 4096}; }

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
    void endFrame() override {}
    bool captureFrame(const std::string& path) override { return writePng(path); }
    const char* name() const override { return "headless"; }

    void setCanvasSize(int w, int h) override { canvas_.resize(w, h); }
    // 无头渲染可以用 SIT_SCALE 指定缩放，方便验高分辨率下的排版和字形
    float scaleHint(int, int) const override {
        const char* e = std::getenv("SIT_SCALE");
        return e ? float(std::atof(e)) : 1.0f;
    }

private:
    bool writePng(const std::string& path) {
        const int w = canvas_.width(), h = canvas_.height();
        const std::vector<uint8_t>& fb = canvas_.pixels();
        FILE* fp = std::fopen(path.c_str(), "wb");
        if (!fp) return false;
        png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        png_infop info = png_create_info_struct(png);
        if (setjmp(png_jmpbuf(png))) {
            png_destroy_write_struct(&png, &info);
            std::fclose(fp);
            return false;
        }
        png_init_io(png, fp);
        png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_write_info(png, info);
        std::vector<png_bytep> rows(h);
        for (int y = 0; y < h; ++y) rows[y] = const_cast<png_bytep>(&fb[size_t(y) * w * 4]);
        png_write_image(png, rows.data());
        png_write_end(png, nullptr);
        png_destroy_write_struct(&png, &info);
        std::fclose(fp);
        return true;
    }

    disp::SoftCanvas canvas_;
};

}  // namespace

REGISTER_DISPLAY_BACKEND("headless", HeadlessBackend)
