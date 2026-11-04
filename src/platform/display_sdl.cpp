// SDL2 窗口后端：Wayland/X11 都能开窗，键盘/鼠标/触摸三源都收
#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include "platform/display_backend.h"
#include "platform/soft_canvas.h"

namespace {

// xterm 序列
// shiftHeld 由调用方按事件顺序跟踪后传入：SDL 的 keysym.mod 对合成/快速输入会滞后，
// 直接用会把 _ 打成 -、把大小写打反
std::string keySequence(const SDL_KeyboardEvent& k, bool shiftHeld, bool ctrlHeld, bool altHeld) {
    const SDL_Keycode sym = k.keysym.sym;
    const Uint16 mod = k.keysym.mod;
    const bool ctrl = ctrlHeld || (mod & KMOD_CTRL) != 0, alt = altHeld, shift = shiftHeld;

    switch (sym) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER: return "\r";
        case SDLK_BACKSPACE: return "\x7f";
        case SDLK_TAB: return shift ? "\x1b[Z" : "\t";
        case SDLK_ESCAPE: return "\x1b";
        case SDLK_INSERT: return "\x1b[2~";
        case SDLK_DELETE: return "\x1b[3~";
        case SDLK_HOME: return "\x1b[H";
        case SDLK_END: return "\x1b[F";
        case SDLK_PAGEUP: return "\x1b[5~";
        case SDLK_PAGEDOWN: return "\x1b[6~";
        case SDLK_UP: return "\x1b[A";
        case SDLK_DOWN: return "\x1b[B";
        case SDLK_RIGHT: return "\x1b[C";
        case SDLK_LEFT: return "\x1b[D";
        case SDLK_F1: return "\x1bOP";
        case SDLK_F2: return "\x1bOQ";
        case SDLK_F3: return "\x1bOR";
        case SDLK_F4: return "\x1bOS";
        case SDLK_F5: return "\x1b[15~";
        case SDLK_F6: return "\x1b[17~";
        case SDLK_F7: return "\x1b[18~";
        case SDLK_F8: return "\x1b[19~";
        case SDLK_F9: return "\x1b[20~";
        case SDLK_F10: return "\x1b[21~";
        case SDLK_F11: return "\x1b[23~";
        case SDLK_F12: return "\x1b[24~";
        default: break;
    }
    if (sym == SDLK_SPACE && ctrl) return std::string(1, '\0');  // Ctrl+Space 切换中英
    if (sym >= SDLK_a && sym <= SDLK_z) {
        // Ctrl+字母 → C0 控制码；Alt+字母 → ESC 前缀（可打印字符本身走 TEXTINPUT）
        if (ctrl) return std::string(1, char(sym - SDLK_a + 1));
        if (alt) return std::string("\x1b") + char(sym);
    }
    // 可打印 ASCII：keysym 值就是 ASCII 码，Shift 由调用方跟踪的状态决定
    if (sym >= 0x20 && sym < 0x7F) {
        char ch = char(sym);
        if (shift) {
            static const char* kFrom = "1234567890-=[]\\;',./`";
            static const char* kTo = "!@#$%^&*()_+{}|:\"<>?~";
            if (ch >= 'a' && ch <= 'z')
                ch = char(ch - 'a' + 'A');
            else if (const char* p = std::strchr(kFrom, ch))
                ch = kTo[p - kFrom];
        }
        return std::string(1, ch);
    }
    return {};
}

class SdlBackend : public disp::DisplayBackend {
public:
    bool init(int w, int h) override {
        fbW_ = w;
        fbH_ = h;
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return false;
        }
        win_ = SDL_CreateWindow("switch-ime-term", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_SHOWN);
        ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        // 放大时不插值：双线性会把 13px 的字糊成一片，最近邻至少笔画是硬的
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!win_ || !ren_ || !tex_) {
            std::fprintf(stderr, "SDL window: %s\n", SDL_GetError());
            return false;
        }
        // 让合成器可以自由缩放（niri 瓦片/全屏）。缩放自己算：
        // SDL_RenderSetLogicalSize 在 1.25 的 fractional scaling 下会把 DPR 叠两次（放大 1.56 倍并被裁）
        SDL_SetWindowResizable(win_, SDL_TRUE);
        canvas_.resize(w, h);
        SDL_StopTextInput();  // 用内置 IME：不要让 fcitx5 画它自己的候选框
        return true;
    }

    void shutdown() override {
        if (tex_) SDL_DestroyTexture(tex_);
        if (ren_) SDL_DestroyRenderer(ren_);
        if (win_) SDL_DestroyWindow(win_);
        SDL_Quit();
    }

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

    void setCanvasSize(int w, int h) override {
        if (w == fbW_ && h == fbH_) return;
        fbW_ = w;
        fbH_ = h;
        canvas_.resize(w, h);
        if (tex_) SDL_DestroyTexture(tex_);
        tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
        uiDirty = 1;
    }

    // 画布按窗口的实际像素尺寸来排，字才是 1:1 清晰
    float scaleHint(int designW, int designH) const override {
        int ow = designW, oh = designH;
        SDL_GetRendererOutputSize(ren_, &ow, &oh);
        float s = std::min(float(ow) / designW, float(oh) / designH);
        return std::clamp(s, 0.5f, 4.0f);
    }

    void endFrame() override {
        if (std::getenv("SIT_DEBUG")) {
            static int lw = -1, lh = -1, lx = -1, ly = -1;
            disp::Rect vp0 = viewport();
            if (vp0.w != lw || vp0.h != lh || vp0.x != lx || vp0.y != ly) {
                lw = vp0.w; lh = vp0.h; lx = vp0.x; ly = vp0.y;
                int ww = 0, wh = 0, ow = 0, oh = 0;
                SDL_GetWindowSize(win_, &ww, &wh);
                SDL_GetRendererOutputSize(ren_, &ow, &oh);
                std::fprintf(stderr, "[blit] 窗口 %dx%d 输出 %dx%d 画面 %dx%d @(%d,%d)\n", ww, wh,
                             ow, oh, vp0.w, vp0.h, vp0.x, vp0.y);
            }
        }
        SDL_UpdateTexture(tex_, nullptr, canvas_.pixels().data(), canvas_.width() * 4);
        SDL_RenderClear(ren_);
        disp::Rect vp = viewport();
        SDL_Rect dst{vp.x, vp.y, vp.w, vp.h};
        SDL_RenderCopy(ren_, tex_, nullptr, &dst);
        SDL_RenderPresent(ren_);
    }

    bool shiftDown_ = false, shiftUsed_ = false, ctrlDown_ = false;

    bool pump(std::string& keys, std::vector<disp::ClickEvent>& clicks) override {
        bool run = true;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT: run = false; break;
                case SDL_MOUSEMOTION: {  // 悬停需要位置；在黑边上给 -1
                    disp::Point p = toFb(e.motion.x, e.motion.y);
                    mouseX = p.x;
                    mouseY = p.y;
                    break;
                }
                case SDL_TEXTINPUT:
                    keys += e.text.text;
                    preedit_.clear();  // 上屏后 preedit 归零
                    break;
                case SDL_TEXTEDITING: preedit_ = e.edit.text; break;
                case SDL_KEYDOWN:
                    if (e.key.keysym.sym == SDLK_LSHIFT || e.key.keysym.sym == SDLK_RSHIFT) {
                        shiftDown_ = true;  // Shift 本身不发字符，抬起时判断是不是单击
                        break;
                    }
                    if (e.key.keysym.sym == SDLK_LCTRL || e.key.keysym.sym == SDLK_RCTRL) {
                        ctrlDown_ = true;
                        break;
                    }
                    if (shiftDown_) shiftUsed_ = true;
                    if (std::getenv("SIT_DEBUG"))  // 临时：看修饰键跟踪是否对得上
                        std::fprintf(stderr, "[key] down sym=%d mod=0x%x ctrl=%d shift=%d\n",
                                     int(e.key.keysym.sym), int(e.key.keysym.mod), ctrlDown_,
                                     shiftDown_);
                    if (((e.key.keysym.mod & KMOD_CTRL) && (e.key.keysym.mod & KMOD_SHIFT)) ||
                        ((ctrlDown_ || (locks & 1)) && (shiftDown_ || (locks & 2)))) {  // 剪贴板
                        if (e.key.keysym.sym == SDLK_c) { copyReq = 1; break; }
                        if (e.key.keysym.sym == SDLK_v) { pasteReq = 1; break; }
                        if (e.key.keysym.sym == SDLK_r) { reloadReq = 1; break; }  // 重载布局
                    }
                    // Shift+左右 = 扩展选区（跟手柄 Shift+ZL/ZR 同一个动作）
                    if ((shiftDown_ || (e.key.keysym.mod & KMOD_SHIFT) || (locks & 2)) &&
                        (e.key.keysym.sym == SDLK_LEFT || e.key.keysym.sym == SDLK_RIGHT)) {
                        selReq += (e.key.keysym.sym == SDLK_RIGHT) ? 1 : -1;
                        break;
                    }
                    // Shift+PgUp/PgDn = 回看历史，不发给 shell
                    if ((e.key.keysym.mod & KMOD_SHIFT) &&
                        (e.key.keysym.sym == SDLK_PAGEUP || e.key.keysym.sym == SDLK_PAGEDOWN)) {
                        wheel += (e.key.keysym.sym == SDLK_PAGEUP) ? 20 : -20;
                        break;
                    }
                    // Esc 现在发给 shell（vim/htop 要用），退出靠关窗
                    if (!(ctrlDown_ || (locks & 1)) || e.key.keysym.sym != SDLK_q)
                        keys += keySequence(e.key, shiftDown_ || (locks & 2) != 0,
                                            ctrlDown_ || (locks & 1) != 0,
                                            (e.key.keysym.mod & KMOD_ALT) || (locks & 4));
                    break;
                case SDL_MOUSEBUTTONDOWN: {
                    // 用 e.button.button 判键：左=1 右=3 中=2 → 我们的 1/2/3
                    const int btn = e.button.button == SDL_BUTTON_LEFT     ? 1
                                    : e.button.button == SDL_BUTTON_RIGHT ? 2
                                    : e.button.button == SDL_BUTTON_MIDDLE ? 3
                                                                           : 0;
                    if (!btn) break;
                    disp::Point p = toFb(e.button.x, e.button.y);
                    if (p.x < 0) break;  // 点在黑边上
                    if (btn == 1 && e.button.clicks >= 2)  // 双击单独走，不跟单击混
                        dblClicks.push_back(p);
                    else
                        clicks.push_back({p.x, p.y, btn});
                    break;
                }
                case SDL_KEYUP:
                    if (std::getenv("SIT_DEBUG"))
                        std::fprintf(stderr, "[key] up sym=%d shiftDown=%d used=%d\n",
                                     int(e.key.keysym.sym), shiftDown_, shiftUsed_);
                    if (e.key.keysym.sym == SDLK_LSHIFT || e.key.keysym.sym == SDLK_RSHIFT) {
                        if (shiftDown_ && !shiftUsed_) shiftTap = 1;
                        shiftDown_ = shiftUsed_ = false;
                    }
                    if (e.key.keysym.sym == SDLK_LCTRL || e.key.keysym.sym == SDLK_RCTRL)
                        ctrlDown_ = false;
                    break;
                case SDL_WINDOWEVENT:
                    if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                        shiftDown_ = shiftUsed_ = ctrlDown_ = false;
                    // 尺寸/曝光变了要重画一帧：否则合成器会拿旧缓冲拉伸填充
                    if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                        e.window.event == SDL_WINDOWEVENT_RESIZED ||
                        e.window.event == SDL_WINDOWEVENT_EXPOSED ||
                        e.window.event == SDL_WINDOWEVENT_SHOWN)
                        uiDirty = 1;
                    break;
                case SDL_MOUSEWHEEL:
                    if (std::getenv("SIT_DEBUG"))
                        std::fprintf(stderr, "[wheel] y=%d dir=%d\n", e.wheel.y, int(e.wheel.direction));
                    wheel += e.wheel.y;
                    break;
                case SDL_FINGERDOWN: {  // 触摸：归一化坐标
                    clicks.push_back({int(e.tfinger.x * fbW_), int(e.tfinger.y * fbH_), 1});
                    break;
                }
                default: break;
            }
        }
        SDL_Delay(16);
        return run;
    }

    bool captureFrame(const std::string&) override { return false; }
    const char* name() const override { return "sdl"; }

    std::string preedit() const override { return preedit_; }

    void setTextInputRect(const disp::Rect& r) override {
        if (!win_) return;
        int ww = fbW_, wh = fbH_;
        SDL_GetWindowSize(win_, &ww, &wh);
        if (ww <= 0 || wh <= 0) return;
        SDL_Rect wr{int(int64_t(r.x) * ww / fbW_), int(int64_t(r.y) * wh / fbH_),
                    int(int64_t(r.w) * ww / fbW_), int(int64_t(r.h) * wh / fbH_)};
        SDL_SetTextInputRect(&wr);
    }

private:
    // 窗口可能被合成器缩放（niri 把 1280×720 放成 1024×576），点击坐标要换算回帧缓冲
    // 画面在窗口里的位置（输出像素坐标）：等比缩放 + 居中
    disp::Rect viewport() const {
        int ow = fbW_, oh = fbH_;
        SDL_GetRendererOutputSize(ren_, &ow, &oh);
        float s = std::min(float(ow) / fbW_, float(oh) / fbH_);
        int w = int(fbW_ * s + 0.5f), h = int(fbH_ * s + 0.5f);
        return {(ow - w) / 2, (oh - h) / 2, w, h};
    }

    // 窗口逻辑坐标 → 画面坐标；落在黑边（等比缩放留的边）上返回 -1
    disp::Point toFb(int wx, int wy) const {
        int ww = fbW_, wh = fbH_;
        SDL_GetWindowSize(win_, &ww, &wh);
        int ow = fbW_, oh = fbH_;
        SDL_GetRendererOutputSize(ren_, &ow, &oh);
        if (ww <= 0 || wh <= 0) return {-1, -1};
        disp::Rect vp = viewport();
        float px = float(wx) * ow / ww, py = float(wy) * oh / wh;  // 先换到输出像素
        if (px < vp.x || py < vp.y || px >= vp.right() || py >= vp.bottom()) return {-1, -1};
        return {int((px - vp.x) * fbW_ / vp.w), int((py - vp.y) * fbH_ / vp.h)};
    }

    int fbW_ = 0, fbH_ = 0;
    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    SDL_Texture* tex_ = nullptr;
    disp::SoftCanvas canvas_;
    std::string preedit_;
};

}  // namespace

REGISTER_DISPLAY_BACKEND("sdl", SdlBackend)
