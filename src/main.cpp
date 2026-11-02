// switch-ime-term · M1：PTY + libvterm + 1280×720 字符网格 UI
#include <poll.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cmath>
#include <fstream>
#include <cstring>
#include <sys/stat.h>
#include <string>
#include <thread>

#include "app/app.h"
#include "app/ime_route.h"
#include "input/uinput_mouse.h"
#include "modules/ime/t9_ime.h"
#include "config/lua_config.h"
#include "ui/screen.h"
#include "input/evdev_gamepad.h"
#include "input/uinput_kbd.h"
#include "platform/display_backend.h"
#include "render/font_manager.h"
#include "ime/builtin_ime.h"
#include "input/gamepad.h"
#include "render/renderer.h"
#include "term/pty.h"
#include "term/vterm_bridge.h"
#include "ui/layout.h"
#include "ui/panels.h"
#include "ui/terminal_view.h"
#include "ui/theme.h"

using namespace ui;

namespace {

void usage() {
    std::printf(
        "switch-ime-term\n"
        "  --backend <name>   sdl | headless（默认按环境自动选）\n"
        "  --out <path>       输出 PNG（headless），默认 frame.png\n"
        "  --run <cmd>        启动后先执行一条命令（截图/CI 用）\n"
        "  --list-backends    列出已注册后端\n"
        "  --theme <name>     配色主题（--list-themes 可查）\n"
        "  --list-themes      列出配色主题\n"
        "  --layout <path>    布局文件（默认 layout.lua）\n"
        "  --sink pty|uinput  输出到自带 PTY，还是 uinput 打给前台程序\n"
        "  --type <text>      只调 uinput 打字然后退出（自检用）\n"
        "  --no-gamepad       不打开 evdev 手柄（默认自动扫 /dev/input/event*）\n"
        "  --print-config     打印从 config/*.lua 装配出来的原生结构\n"
        "  --audit-ime        逐条核对输入法行为（标准输入法对照表）\n"
        "  --audit-t9         核对 T9 状态机 + 手柄按键映射（合成事件）\n"
        "  --ui-preview <png> 只渲染新 UI（M0）到 PNG，不启 PTY\n"
        "  --legacy           走旧 UI（新架构调试用）\n"
        "  --scale <f>        预览用缩放（默认 1）\n"
        "  --selftest         PTY + 摇杆自检\n"
        "  --help\n");
}

// libvterm 不回设备属性查询，fish 启动时会一直等（bash 不等）。
// 只答应答 DA1，其余查询（背景色/XTVERSION/kitty 键盘）fish 有超时兜底。
// shortcut: 逐块子串匹配，查询被拆到两次 read 会漏；实测 fish 每次都整段到达
void answerDeviceAttributes(term::Pty& pty, const char* buf, long n) {
    std::string s(buf, size_t(n));
    if (s.find("\x1b[0c") != std::string::npos || s.find("\x1b[c") != std::string::npos) {
        const char* reply = "\x1b[?62;22c";  // VT220 + ANSI 彩色
        pty.writeMaster(reply, std::strlen(reply));
    }
}

// 把 PTY 里可读的数据全部灌进 vterm；false = shell 已退出
bool pumpPty(term::Pty& pty, term::VTermBridge& vt) {
    char buf[8192];
    long n = pty.readMaster(buf, sizeof buf);
    if (n > 0) {
        answerDeviceAttributes(pty, buf, n);
        vt.feed(buf, size_t(n));
        return true;
    }
    return n == 0;  // 0 = 暂无数据，-1 = EOF
}

}  // namespace

// fbdev 打包自检：把一帧写进普通文件，回读验证 XRGB8888 字节序与居中偏移
static int fbdevSelfTest() {
    const char* path = "/tmp/sit_fbtest.raw";
    ::setenv("SIT_FBDEV", path, 1);
    int w = 1280, h = 720;
    disp::DisplayBackend* bd = disp::DisplayRegistry::instance().create("fbdev");
    int bad = 0;
    if (!bd || !bd->init(w, h)) {
        std::printf("[selftest] fbdev SKIP（没有可用的 fbdev）\n");
        ::unsetenv("SIT_FBDEV");
        delete bd;
        return 0;
    }
    bd->beginFrame();
    bd->fillRect({0, 0, w, h}, disp::rgb(0xFF0000));          // 全红
    bd->fillRect({10, 10, 8, 8}, disp::rgb(0x00FF00));        // 绿块
    bd->fillRect({w - 10, h - 10, 8, 8}, disp::rgb(0x0000FF)); // 蓝块
    bd->endFrame();
    bd->shutdown();
    delete bd;
    ::unsetenv("SIT_FBDEV");

    FILE* f = std::fopen(path, "rb");
    if (!f) return 1;
    auto px = [&](int x, int y, uint8_t* o) {
        std::fseek(f, long(size_t(y) * w * 4 + size_t(x) * 4), SEEK_SET);
        return std::fread(o, 1, 4, f) == 4;
    };
    uint8_t b[4];
    auto chk = [&](int x, int y, uint8_t r, uint8_t g, uint8_t bl) {
        if (!px(x, y, b)) { ++bad; return; }
        if (b[0] != bl || b[1] != g || b[2] != r) ++bad;  // BGRX
    };
    chk(0, 0, 0xFF, 0, 0);
    chk(12, 12, 0, 0xFF, 0);
    chk(w - 8, h - 8, 0, 0, 0xFF);
    std::fclose(f);
    std::printf("[selftest] fbdev %s（XRGB8888 打包 + 居中）\n", bad ? "FAIL" : "OK");
    return bad ? 1 : 0;
}

// 拼音自检：索引加载 + 单音节 + 双音节组合
static int imeSelfTest() {
    ime::PinyinIndex idx;
    if (!idx.load("data/pinyin.idx")) return 1;
    auto ni = idx.query("ni", 8);
    auto nihao = idx.query("nihao", 8);
    auto zhuomian = idx.query("zhuomian", 8);
    bool ok = !ni.empty() && ni[0].text == "你" && !nihao.empty() && nihao[0].text == "你好" &&
              !zhuomian.empty() && zhuomian[0].text == "桌面";
    auto half = idx.query("y", 8);   // 前缀匹配
    if (ok) ok = !half.empty();
    // 注音抽查：你→nǐ，好→hǎo
    if (ok) ok = ni[0].annot == "n\xC7\x90";
    {   // n-gram 上下文重排：基线第一个是 是，上下文「知」之后 识 应该被顶到第一
        ime::BuiltinIme b;
        if (!b.load("data/pinyin.idx", "data/ngram.bin")) return 1;
        b.push('s'); b.push('h'); b.push('i');
        std::string base = b.cands().empty() ? "-" : b.cands()[0].text;
        b.setContext(0x77E5);  // 知
        std::string ctx = b.cands().empty() ? "-" : b.cands()[0].text;
        bool ng = (ctx == "识");
        std::printf("[selftest] ngram %s  shi 基线首位=%s，上下文「知」后首位=%s\n",
                    ng ? "OK" : "FAIL", base.c_str(), ctx.c_str());
        if (!ng) ok = false;
    }
    std::printf("[selftest] pinyin %s  y→%s  ni→%s(%s)  nihao→%s(%s)  zhuomian→%s  (%zu 音节)\n",
                ok ? "OK" : "FAIL", half.empty() ? "-" : half[0].text.c_str(),
                ni.empty() ? "-" : ni[0].text.c_str(),
                ni.empty() ? "-" : ni[0].annot.c_str(),
                nihao.empty() ? "-" : nihao[0].text.c_str(),
                nihao.empty() ? "-" : nihao[0].annot.c_str(),
                zhuomian.empty() ? "-" : zhuomian[0].text.c_str(), idx.syllables());
    return ok ? 0 : 1;
}

// 摇杆自检：8 扇区 + 死区 + 滞回
static int gamepadSelfTest() {
    struct Case {
        float x, y;
        int expect;
    };
    // 屏幕坐标（y 向下）：右=0 右上=1 上=2 左上=3 左=4 左下=5 下=6 右下=7
    const Case cases[] = {
        {1, 0, 0},          {0.7f, -0.7f, 1}, {0, -1, 2},        {-0.7f, -0.7f, 3},
        {-1, 0, 4},         {-0.7f, 0.7f, 5}, {0, 1, 6},         {0.7f, 0.7f, 7},
        {0.1f, 0.1f, -1},   // 死区
    };
    input::StickNormalizer n;
    int bad = 0;
    for (auto& c : cases) {
        int got = n.update({c.x, c.y});
        if (got != c.expect) {
            std::printf("  sector(%+.1f,%+.1f) = %d, expect %d\n", c.x, c.y, got, c.expect);
            ++bad;
        }
    }
    // 滞回：停在 E 扇区后偏移 10°，不应跳到 SE
    n.update({1, 0});
    if (n.update({0.68f, 0.12f}) != 0) {
        std::printf("  hysteresis failed\n");
        ++bad;
    }
    // 字母表抽查
    if (std::strcmp(input::leftLetterFor(2), "i") || std::strcmp(input::rightLetterFor(4), "d"))
        ++bad;
    std::printf("[selftest] gamepad %s\n", bad ? "FAIL" : "OK");
    return bad ? 1 : 0;
}

// PTY 自检：不依赖终端模拟器，只验证 fork/读写/回收这条链路
static int selfTest() {
    term::Pty p;
    if (!p.spawn({"/bin/sh", "-c", "printf 'hello-pty\\n'"}, 80, 24)) return 1;
    std::string got;
    for (int i = 0; i < 200 && got.find("hello-pty") == std::string::npos; ++i) {
        char buf[256];
        long n = p.readMaster(buf, sizeof buf);
        if (n > 0)
            got.append(buf, size_t(n));
        else if (n < 0)
            break;
        else
            ::usleep(10000);
    }
    bool ok = got.find("hello-pty") != std::string::npos;
    std::printf("[selftest] pty %s (%zu bytes)\n", ok ? "OK" : "FAIL", got.size());
    return ok ? 0 : 1;
}

// 主题自检：切换生效、未知名字不改动配色、能切回来
static int themeSelfTest() {
    pal::loadDefault();
    uint32_t base = pal::bg;
    bool ok = pal::current() && std::strcmp(pal::current(), "switch") == 0;
    ok = ok && pal::load("light") && pal::bg != base && pal::term != base;
    ok = ok && !pal::load("no-such-theme") && std::strcmp(pal::current(), "light") == 0;
    pal::loadDefault();
    ok = ok && pal::bg == base;
    std::printf("[selftest] theme %s  ", ok ? "OK" : "FAIL");
    for (const char* const* n = pal::names(); *n; ++n) std::printf("%s ", *n);
    std::printf("\n");
    return ok ? 0 : 1;
}

// 滚动自检：30 行塞进 10 行屏，历史应正好 20 行，最早一行是 line0
static int scrollSelfTest() {
    term::VTermBridge vt;
    if (!vt.init(10, 20)) return 1;
    std::string all;
    for (int i = 0; i < 30; ++i) all += "line" + std::to_string(i) + "\r\n";
    vt.feed(all.data(), all.size());
    std::string first;
    for (int c = 0; c < 5; ++c) first += char(vt.historyAt(0, c).cp);
    bool ok = vt.historySize() >= 20 && first == "line0";  // 末尾那个 \n 会多滚一行
    std::printf("[selftest] scrollback %s  历史 %d 行，最早=\"%s\"\n", ok ? "OK" : "FAIL",
                vt.historySize(), first.c_str());
    return ok ? 0 : 1;
}

// 配置自检：engine.lua 缺文件→默认值，缺键→保留默认值，写了的键才覆盖
static int configSelfTest() {
    cfg::Engine e;
    bool ok = !cfg::loadEngine("/tmp/sit_no_such_engine.lua", e) && e.scrollback == 2000 &&
              e.candPerPage == 7 && e.doubleTapMs == 400;
    const char* path = "/tmp/sit_engine_selftest.lua";
    if (FILE* f = std::fopen(path, "w")) {
        std::fputs("engine = {}\nengine.terminal = { scrollback = 11 }\n"
                   "engine.input = { double_tap_ms = 250 }\nengine.sink = \"uinput\"\n",
                   f);
        std::fclose(f);
    }
    cfg::Engine e2;
    ok = ok && cfg::loadEngine(path, e2) && e2.scrollback == 11 && e2.doubleTapMs == 250 &&
         e2.sink == "uinput";
    ok = ok && e2.candPerPage == 7 && e2.ngram && e2.deadzone == 0.35f;  // 缺的键还在默认值
    std::remove(path);
    std::printf("[selftest] config %s  scrollback=%d double_tap=%dms sink=%s\n", ok ? "OK" : "FAIL",
                e2.scrollback, e2.doubleTapMs, e2.sink.c_str());
    return ok ? 0 : 1;
}

// 中英切换自检：组字中切到英文，拼音必须原样交出而不是消失
static int switchSelfTest() {
    ime::BuiltinIme im;
    im.push('z');
    im.push('m');
    im.push('u');
    bool composing = im.composing();
    std::string raw = im.toggle();
    bool ok = composing && raw == "zmu" && !im.chinese && !im.composing();
    ok = ok && im.toggle().empty() && im.chinese;
    std::printf("[selftest] langswitch %s  组字中切英文交出=\"%s\"\n", ok ? "OK" : "FAIL",
                raw.c_str());
    return ok ? 0 : 1;
}

// 剪贴板：走 wl-clipboard（Wayland 本机）
static std::string clipboardRead() {
    std::string out;
    FILE* f = ::popen("wl-paste --no-newline 2>/dev/null", "r");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    ::pclose(f);
    return out;
}

static void clipboardWrite(const std::string& text) {
    FILE* f = ::popen("wl-copy 2>/dev/null", "w");
    if (!f) return;
    std::fwrite(text.data(), 1, text.size(), f);
    ::pclose(f);
}

// 选区自检：中文宽字符占两格，导出必须是干净的 UTF-8，不能带延续格垃圾
static int selectSelfTest() {
    term::VTermBridge vt;
    if (!vt.init(4, 20)) return 1;
    const char* txt = "你好,兄弟,\r\n";
    vt.feed(txt, std::strlen(txt));
    ui::TerminalView v;
    v.grid = &vt;
    v.cols = 20;
    v.rows = 4;
    v.cellW = 8;
    v.cellH = 21;
    v.selAnchor = 0;
    v.selCaret = 9;  // 覆盖「你好,兄弟,」10 格
    std::string got = ui::selectedText(v);
    std::printf("[selftest] selection 取到 %zu 字节:", got.size());
    for (unsigned char c : got) std::printf(" %02x", c);
    std::printf("\n  文本=\"%s\"\n", got.c_str());
    bool ok = got == "你好,兄弟,";
    std::printf("[selftest] selection %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// 手柄映射自检：喂合成 evdev 事件，不用真手柄
static int gamepadEvdevSelfTest() {
    input::GamepadMapper m;
    for (int code : {ABS_X, ABS_Y, ABS_RX, ABS_RY}) m.setAxisRange(code, 0, 255);
    std::string bytes;
    int action = -1;
    input::GamepadHandlers h{[&](const std::string& s) { bytes += s; },
                             [&](int a) { action = a; }};
    auto ev = [&](int t, int c, int v) {
        input_event e{};
        e.type = t;
        e.code = c;
        e.value = v;
        m.onEvent(e, h);
    };

    ev(EV_ABS, ABS_X, 255);  // 左摇杆推右
    ev(EV_ABS, ABS_Y, 127);
    ev(EV_SYN, SYN_REPORT, 0);
    std::string lE = input::leftLetterFor(0);
    bool s1 = !bytes.empty() && bytes == lE;
    bool ok = s1;

    bytes.clear();
    ev(EV_KEY, BTN_SOUTH, 1);  // A
    int aA = (bytes == "\r");
    ok = ok && aA;

    bytes.clear();
    ev(EV_KEY, BTN_WEST, 1);  // Y = ESC
    int aY = (bytes == "\x1b");
    ok = ok && aY;

    action = -1;
    ev(EV_ABS, ABS_HAT0X, -1);  // 十字左（单按 = 一次性修饰）
    int aHat = (action == input::kGpActStickyCtrl);
    ok = ok && aHat;

    action = -1;
    ev(EV_ABS, ABS_HAT0X, 0);
    ev(EV_ABS, ABS_HAT0X, -1);  // 400ms 内再按一次 = 锁定
    int aHat2 = (action == input::kGpActLockCtrl);
    ok = ok && aHat2;

    action = -1;
    ev(EV_KEY, BTN_TL, 1);  // L
    int aL = (action == input::kGpActLayer);
    ok = ok && aL;
    bool s5 = false;

    bytes.clear();
    ev(EV_ABS, ABS_RX, 127);  // 右摇杆推上
    ev(EV_ABS, ABS_RY, 0);
    ev(EV_SYN, SYN_REPORT, 0);
    std::string lastBytes = bytes;
    s5 = (bytes == input::rightLetterFor(2));
    ok = ok && s5;

    std::printf("[selftest] gamepad %s  左摇杆右→\"%s\" A:%d Y:%d 十字左:%d L:%d 右摇杆上→\"%s\"(期望\"%s\")\n",
                ok ? "OK" : "FAIL", lE.c_str(), aA, aY, aHat, aL, lastBytes.c_str(),
                input::rightLetterFor(2));
    return ok ? 0 : 1;
}

// 相对路径解析：先从当前目录找，再从可执行文件所在目录找（从别处启动也能跑）
static std::string exeDir;
static std::string findRes(const std::string& rel) {
    if (std::ifstream(rel).good()) return rel;
    for (const std::string dir : {exeDir, exeDir + "/.."}) {  // 二进制在 build/ 时数据在上一级
        std::string p = dir + "/" + rel;
        if (std::ifstream(p).good()) return p;
    }
    return rel;  // 都没有就返回原路径，让调用方报错
}

// 信号触发的布局重载：kill -HUP / kill -USR1，不用点窗口
static volatile sig_atomic_t gReloadReq = 0;
static void onReloadSignal(int) { gReloadReq = 1; }

int main(int argc, char** argv) {
    if (argc > 0 && argv[0]) {
        std::string exe = argv[0];
        size_t slash = exe.find_last_of('/');
        exeDir = slash == std::string::npos ? "." : exe.substr(0, slash);
    }
    std::string backend;  // 空 = 自动：有显示会话给 sdl，否则 headless
    std::string themeName;
    std::string layoutPath = "layout.lua";
    std::string layoutResolved = "layout.lua";
    std::string sink = "pty";
    bool useGamepad = true;
    bool legacy = false;
    std::string typeText;
    std::string uiPreview;
    float previewScale = 1.0f;
    std::string out = "frame.png";
    std::string runCmd;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { usage(); return 0; }
        if (a == "--selftest") return selfTest() + gamepadSelfTest() + imeSelfTest() + fbdevSelfTest() + themeSelfTest() +
               scrollSelfTest() + switchSelfTest() + selectSelfTest() + gamepadEvdevSelfTest() +
               configSelfTest();
        if (a == "--list-backends") {
            for (auto& n : disp::DisplayRegistry::instance().names()) std::printf("%s\n", n.c_str());
            return 0;
        }
        if (a == "--theme" && i + 1 < argc) { themeName = argv[++i]; continue; }
        if (a == "--layout" && i + 1 < argc) { layoutPath = argv[++i]; continue; }
        if (a == "--sink" && i + 1 < argc) { sink = argv[++i]; continue; }
        if (a == "--no-gamepad") { useGamepad = false; continue; }
        if (a == "--legacy") { legacy = true; continue; }
        if (a == "--probe-mouse") {
            // 建虚拟鼠标设备 → 查内核是否注册 → 点一下 → 退出
            UInputMouse m;
            if (!m.ensure()) return 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            int found = 0;
            for (int i = 0; i < 64; ++i) {
                std::string n = "/sys/class/input/input" + std::to_string(i) + "/name";
                std::FILE* f = std::fopen(n.c_str(), "r");
                if (!f) continue;
                char buf[128] = {0};
                if (std::fgets(buf, sizeof buf, f) && std::string(buf).find("virtual mouse") != std::string::npos)
                    ++found;
                std::fclose(f);
            }
            m.click(BTN_LEFT);
            std::printf("uinput 设备注册：%s\n", found ? "是（已出现在 /sys/class/input）" : "否");
            std::printf("左键点击：已发出 BTN_LEFT 按下+抬起\n");
            return found ? 0 : 1;
        }
        if (a == "--audit-t9") {
            cfg::Ui uic;
            if (!cfg::loadUi(findRes("config/ui.lua").c_str(), uic)) return 1;
            int bad = 0;
            ime::T9Ime t9;
            t9.setKeys(uic.t9Alpha);
            auto ck = [&](const char* n, bool ok, const std::string& got, const std::string& exp) {
                if (!ok) ++bad;
                std::printf("  %-24s %s 实得=%s 期望=%s\n", n, ok ? "OK  " : "FAIL", got.c_str(),
                            exp.c_str());
            };
            std::printf("T9 状态机：\n");
            // 焦点：默认 5(JKL)=4；九宫格内移动，越界不循环
            ck("默认焦点 5(JKL)", t9.focus() == 4, std::to_string(t9.focus()), "4");
            t9.stick(0);  // E  → 6(MNO)
            ck("右→6(MNO)", t9.focus() == 5, std::to_string(t9.focus()), "5");
            t9.stick(6);  // S  → 9(WXYZ)
            ck("下→9(WXYZ)", t9.focus() == 8, std::to_string(t9.focus()), "8");
            t9.stick(4);  // W  → 8(TUV)
            ck("左→8(TUV)", t9.focus() == 7, std::to_string(t9.focus()), "7");
            t9.stick(0);  // → 9(WXYZ)
            ck("再右→9(WXYZ)", t9.focus() == 8, std::to_string(t9.focus()), "8");
            t9.stick(0);  // 已在最右列，不应移动
            ck("最右列不动", t9.focus() == 8, std::to_string(t9.focus()), "8");
            t9.reset();
            // 魔法键：Grid → Popup → 连击上屏
            std::string m1 = t9.magic();
            ck("LB 进弹窗", t9.popup() && m1.empty(), t9.popup() ? "popup" : "grid", "popup");
            std::string m2 = t9.magic();
            ck("LB 上屏首字母 J", m2 == "J", m2, "J(5=JKL)");
            t9.stick(0);  // 弹窗内右 → 下一个字母
            std::string m3 = t9.magic();
            ck("摇杆右后上屏 K", m3 == "K", m3, "K");
            t9.stick(4);  // 弹窗内左 → 回退
            std::string m4 = t9.magic();
            ck("摇杆左后回 J", m4 == "J", m4, "J");
            ck("连击不退出弹窗", t9.popup(), t9.popup() ? "popup" : "grid", "popup");
            t9.leavePopup();
            ck("Esc 类退出弹窗", !t9.popup(), "grid", "grid");

            // 手柄事件 → PadButton/扇区（合成 evdev 事件）
            std::printf("手柄映射：\n");
            input::PadState pad;
            for (int code : {ABS_X, ABS_Y, ABS_RX, ABS_RY}) pad.setAxisRange(code, 0, 255);
            std::string got;
            input::PadHandlers ph{[&](int b) {
                                      static const char* nm[] = {"LB", "LT", "RB", "RT", "A",
                                                                 "B",  "X",  "Y",  "UP",
                                                                 "DOWN", "LEFT", "RIGHT"};
                                      got = nm[b];
                                  },
                                  [&](int sec) { got = "Lstick" + std::to_string(sec); },
                                  [](int) {}};
            auto ev = [&](int t, int c, int v) {
                input_event e{};
                e.type = t; e.code = c; e.value = v;
                pad.onEvent(e, ph);
            };
            ev(EV_KEY, BTN_SOUTH, 1);
            ck("A 键", got == "A", got, "A");
            ev(EV_KEY, BTN_TL, 1);
            ck("左上肩=L", got == "LB", got, "LB");
            ev(EV_ABS, ABS_HAT0X, -1);
            ck("十字左", got == "LEFT", got, "LEFT");
            got.clear();
            ev(EV_ABS, ABS_X, 255);
            ev(EV_ABS, ABS_Y, 127);
            ev(EV_SYN, SYN_REPORT, 0);
            ck("左摇杆推右→扇区0", got == "Lstick0", got, "Lstick0");
            std::printf("结果：%s（%d 项失败）\n", bad ? "有偏差" : "全部符合", bad);
            return bad ? 1 : 0;
        }
        if (a == "--audit-ime") {
            ime::BuiltinIme im;
            if (!im.load(findRes("data/pinyin.idx"), findRes("data/ngram.bin"))) {
                std::printf("需要 data/pinyin.idx\n");
                return 1;
            }
            int bad = 0;
            auto run1 = [&](char ch) { return app::routeKey(im, ch); };
            auto type = [&](const char* s) {
                std::string out;
                for (const char* p2 = s; *p2; ++p2) out += run1(*p2).bytes;
                return out;
            };
            auto check = [&](const char* name, bool ok, const std::string& got,
                             const char* expect) {
                if (!ok) ++bad;
                std::printf("  %-28s %s  实得=\"%s\" 期望=\"%s\"\n", name, ok ? "OK  " : "FAIL",
                            got.c_str(), expect);
            };
            std::printf("输入法行为核对：\n");
            // 1 组字不输出
            im.clear();
            std::string o = type("nihao");
            check("组字不落屏", o.empty() && im.preedit() == "nihao", o, "");
            // 2 空格上屏首候选
            o = run1(' ').bytes;
            check("空格→首候选", o == "你好" && !im.composing(), o, "你好");
            // 3 回车 → 拼音原样
            im.clear();
            type("nihao");
            o = run1('\r').bytes;
            check("回车→拼音原样", o == "nihao" && !im.composing(), o, "nihao");
            // 4 数字选词
            im.clear();
            type("nihao");
            o = run1('2').bytes;
            check("数字2→第2候选", !o.empty() && o != "你好", o, "(第2个候选)");
            // 5 退格
            im.clear();
            type("nihao");
            run1(0x7f);
            check("退格删字母", im.preedit() == "niha", im.preedit(), "niha");
            // 6 Tab 翻页（候选项够多才翻页，否则停在第 1 页是对的）
            im.clear();
            type("yi");
            int p0 = im.page(), pc = im.pageCount();
            run1('\t');
            bool tabOk = (pc > 1) ? (im.page() == p0 + 1) : (im.page() == p0);
            check("Tab翻页", tabOk,
                  std::to_string(im.page()) + " (共" + std::to_string(pc) + "页, 组字=" +
                      im.preedit() + ", 候选=" + std::to_string(im.cands().size()) + ")",
                  "(有第2页则+1)");
            // 7 Esc 取消
            im.clear();
            type("nihao");
            o = run1(0x1b).bytes;
            check("Esc取消组字", !im.composing() && o == "\x1b", o, "ESC");
            // 8 标点：先上屏候选再发标点
            im.clear();
            type("nihao");
            o = run1(',').bytes;
            check("标点先上屏候选", o == "你好,", o, "你好,");
            // 9 组字中切语言：拼音原样落下
            im.clear();
            type("zmu");
            o = run1(0x00).bytes;
            check("Ctrl+Space 拼音落下", o == "zmu" && !im.chinese, o, "zmu");
            // 10 英文态字母直通
            o = type("abc");
            check("英文态直通", o == "abc", o, "abc");
            // 11 锁 Shift 后大写字母仍能组字（自动转小写）
            im.toggle();  // 回中文
            im.clear();
            o = type("Nihao");
            check("大写也能组字", o.empty() && im.preedit() == "nihao", im.preedit(), "nihao");
            o = run1(' ').bytes;
            check("大写组字后上屏", o == "你好", o, "你好");
            // 13 T9 九键：64426 → nihao → 你好
            im.clear();
            if (!im.chinese) im.toggle();
            for (char d : std::string("64426")) im.push(d);
            bool t9ok = im.digitsBuffer() && !im.cands().empty() &&
                        im.cands()[0].text == "你好";
            check("T9 九键 64426→你好", t9ok,
                  im.cands().empty() ? "(无候选)" : im.cands()[0].text + " / 组字=" + im.preedit(),
                  "你好");
            o = run1(' ').bytes;
            check("T9 空格上屏", o == "你好", o, "你好");
            // 14 英文 T9：43556 → hello
            if (im.chinese) im.toggle();  // 切英文
            im.clear();
            for (char d : std::string("43556")) im.push(d);
            bool en9ok = im.digitsBuffer() && !im.cands().empty() && im.cands()[0].text == "hello";
            check("英文 T9 43556→hello", en9ok,
                  im.cands().empty() ? "(无候选)" : im.cands()[0].text, "hello");
            if (!im.chinese) im.toggle();  // 切回中文
            // 15 修饰键 + 明确字母 = 组合键（Ctrl+C 这样打出来）
            if (im.chinese) im.toggle();  // 英文态
            im.clear();
            o = app::routeKey(im, 'c', 1).bytes;
            check("Ctrl+c → 0x03", o == std::string(1, char(0x03)), "0x03", "0x03");
            o = app::routeKey(im, 'x', 4).bytes;
            check("Alt+x → ESC x", o == std::string("\x1bx"), "ESC+x", "ESC+x");
            o = app::routeKey(im, 'a', 2).bytes;
            check("Shift+a → A", o == "A", o, "A");
            if (!im.chinese) im.toggle();  // 回中文
            // 16 中文态：修饰键不抢字母，照样进组字
            if (!im.chinese) im.toggle();  // 回中文
            im.clear();
            o = app::routeKey(im, 'c', 1).bytes;
            check("中文态 Ctrl+c 不组合", o.empty() && im.preedit() == "c",
                  o.empty() ? ("组字=" + im.preedit()) : o, "组字=c");
            im.clear();
            // 17 组字中误触控制码被吞
            im.clear();
            type("ni");
            o = run1(0x01).bytes;  // 锁 Ctrl 时按 a 会变成这个
            check("控制码被吞", o.empty() && im.preedit() == "ni", o, "");
            std::printf("结果：%s（%d 项失败）\n", bad ? "有偏差" : "全部符合", bad);
            return bad ? 1 : 0;
        }
        if (a == "--print-config") {
            cfg::Profile prof;
            bool ok = cfg::loadProfile("config", prof);
            const cfg::Ui& ui = prof.ui;
            const cfg::KeyMap& km = prof.keymap;
            const cfg::Engine& eng = prof.engine;
            std::printf("装配 %s\n", ok ? "OK" : "FAIL");
            std::printf("panel_w=%d stick_box=%d cell=%dx%d title=%s\n", ui.panelW, ui.stickBox,
                        ui.cellW, ui.cellH, ui.title.c_str());
            for (int i = 0; i < 9; i += 3)
                std::printf("T9 %s:%s  %s:%s  %s:%s\n", ui.t9Alpha[i].num.c_str(),
                            ui.t9Alpha[i].letters.c_str(), ui.t9Alpha[i + 1].num.c_str(),
                            ui.t9Alpha[i + 1].letters.c_str(), ui.t9Alpha[i + 2].num.c_str(),
                            ui.t9Alpha[i + 2].letters.c_str());
            static const char* kn[] = {"LB", "LT", "RB", "RT", "A", "B", "X", "Y",
                                       "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT"};
            for (int k = 0; k < cfg::kKeyCount; ++k)
                if (km.single[k] || km.dbl[k]) {
                    std::printf("  %-10s 单击=%s", kn[k], cfg::actionName(km.single[k]));
                    if (km.dbl[k]) std::printf("  双击=%s", cfg::actionName(km.dbl[k]));
                    std::printf("\n");
                }
            std::printf("axis STICK_R mouse_move=%d\n", km.axisMouseMove ? 1 : 0);
            std::printf(
                "engine: scrollback=%d cand_per_page=%d ngram=%d t9=%d deadzone=%.2f "
                "hysteresis_deg=%.0f double_tap_ms=%d log=%s%s%s sink=%s\n",
                eng.scrollback, eng.candPerPage, eng.ngram, eng.t9, eng.deadzone,
                eng.hysteresisDeg, eng.doubleTapMs, eng.logLevel.c_str(),
                eng.logPath.empty() ? "" : "@", eng.logPath.c_str(), eng.sink.c_str());
            return ok ? 0 : 1;
        }
        if (a == "--type" && i + 1 < argc) { typeText = argv[++i]; continue; }
        if (a == "--ui-preview" && i + 1 < argc) { uiPreview = argv[++i]; continue; }
        if (a == "--scale" && i + 1 < argc) { previewScale = std::atof(argv[++i]); continue; }
        if (a == "--list-themes") {
            for (const char* const* n = pal::names(); *n; ++n)
                std::printf("%s%s\n", *n, std::strcmp(*n, pal::current()) == 0 ? "  (默认)" : "");
            return 0;
        }
        if (a == "--backend" && i + 1 < argc) { backend = argv[++i]; continue; }
        if (a == "--out" && i + 1 < argc) { out = argv[++i]; continue; }
        if (a == "--run" && i + 1 < argc) { runCmd = argv[++i]; continue; }
        std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
        usage();
        return 1;
    }

    // 后端选择：有显示会话走 sdl；裸 TTY（Switch 也是这种）有 fb0 就写帧缓冲；否则只出图
    if (backend.empty()) {
        if (std::getenv("WAYLAND_DISPLAY") || std::getenv("DISPLAY"))
            backend = "sdl";
        else if (::access("/dev/fb0", W_OK) == 0)
            backend = "fbdev";
        else
            backend = "headless";
    }

    pal::loadDefault();
    if (!themeName.empty() && !pal::load(themeName.c_str())) {
        std::fprintf(stderr, "没有主题 '%s'，用默认 %s\n", themeName.c_str(), pal::current());
    }
    if (!uiPreview.empty()) {  // M0：只画新 UI，验排版
        cfg::Ui uic;
        if (!cfg::loadUi(findRes("config/ui.lua").c_str(), uic)) {
            std::fprintf(stderr, "读不到 config/ui.lua\n");
            return 1;
        }
        ui::layout::setScale(previewScale);
        ui::Geometry g;
        g.fromConfig(uic, previewScale);

        disp::DisplayBackend* pb = disp::DisplayRegistry::instance().create("headless");
        if (!pb || !pb->init(g.screenW, g.screenH)) return 1;
        render::FontManager pfm;
        if (!pfm.init()) {
            std::fprintf(stderr, "字体加载失败\n");
            return 1;
        }
        ui::FontBook pfonts;
        if (!pfonts.init(pfm)) {
            std::fprintf(stderr, "FontBook 初始化失败\n");
            return 1;
        }
        render::Renderer pr;
        pr.init(pb, &pfm);

        ui::AppState st;
        st.compose = "nihao";
        st.cands = {{"你好", "nǐ hǎo"}, {"你", "nǐ"}, {"尼", "ní"}, {"呢", "ne"},
                    {"泥", "ní"},   {"拟", "nǐ"}, {"逆", "nì"}};
        st.candPageCount = 3;
        st.modsText = "CTRL";
        if (std::getenv("SIT_POPUP")) {  // 预览弹窗态
            st.popup = true;
            st.focusIdx = 4;
            st.popupLetter = 'K';
            st.popupIdx = 1;
            st.popupCount = 3;
        }

        pr.beginFrame();
        pr.fill({0, 0, g.screenW, g.screenH}, ui::c(ui::col::kBg));
        ui::drawTopBar(pr, pfonts, uic, g, st);
        ui::drawT9Panel(pr, pfonts, uic, g, st);
        ui::drawTermBackground(pr, pfonts, g);
        // 终端区示例内容（M1 接 TerminalParser 后由网格填）
        int ty = g.contentY + g.termPadTop;
        int bh = (pfonts.term->ascent - pfonts.term->descent);
        auto line = [&](int row, const std::string& t, uint32_t colr) {
            pr.textBaseline(g.termX + g.termPadX, ty + bh + row * ui::layout::kCellH, t,
                            pfonts.term, ui::c(colr));
        };
        line(0, "switch@arch:~$ echo \"你好，Switch\"", ui::col::kGreen);
        line(1, "你好，Switch", ui::col::kText);
        line(2, "switch@arch:~$ sudo pacman -S --needed fcitx5", ui::col::kGreen);
        line(3, ":: 正在解析依赖关系...", ui::col::kDim);
        ui::drawComposeLine(pr, pfonts, g, st);
        ui::drawMousePanel(pr, pfonts, uic, g, st);
        ui::drawBottomBar(pr, pfonts, g, st);
        pr.endFrame();
        bool ok = pb->captureFrame(uiPreview);
        std::printf("ui-preview: %s  %dx%d scale=%.2f\n", ok ? "OK" : "FAIL", g.screenW, g.screenH,
                    previewScale);
        pb->shutdown();
        delete pb;
        return ok ? 0 : 1;
    }

    if (!typeText.empty()) {  // 只验 uinput：打到当前焦点程序，看看能不能收到
        input::UinputKeyboard kb;
        if (!kb.open()) {
            std::fprintf(stderr, "uinput 打不开：检查 /dev/uinput 权限\n");
            return 1;
        }
        int gap = std::getenv("SIT_UINPUT_GAP") ? std::atoi(std::getenv("SIT_UINPUT_GAP")) : 2;
        kb.typeBytes(typeText + "\r", gap);
        return 0;
    }

    ui::layout::recompute();
    ui::keys::resetDefaults();
    layoutResolved = findRes(layoutPath);
    if (ui::layout::loadFile(layoutResolved.c_str()))  // 布局/键位都在 layout.lua，改完不用重编
        std::printf("layout: %s  panel=%d term=%dx%d 字符格=%d×%d\n", layoutPath.c_str(),
                    ui::layout::kPanelW, ui::layout::kTermCols, ui::layout::kTermRows,
                    ui::layout::kCellW, ui::layout::kCellH);

    disp::DisplayBackend* bd = disp::DisplayRegistry::instance().create(backend);
    if (!bd) {
        std::fprintf(stderr, "backend '%s' not found\n", backend.c_str());
        return 1;
    }
    if (!bd->init(layout::kDesignW, layout::kDesignH)) {
        std::fprintf(stderr, "backend init failed\n");
        return 1;
    }

    // 按窗口实际像素决定缩放：画布 = 设计尺寸 × 缩放，字体也用同一系数重栅格化
    layout::setScale(bd->scaleHint(layout::kDesignW, layout::kDesignH));
    bd->setCanvasSize(layout::kScreenW, layout::kScreenH);
    std::fprintf(stderr, "ui: 缩放 %.2fx → 画布 %dx%d，字体 %.1fpx\n", layout::kScale,
                 layout::kScreenW, layout::kScreenH, 13.0 * layout::kScale);

    if (!legacy) {  // 新架构：ui/screen + TerminalParser
        int rc = app::run(bd, exeDir + "/..", out, runCmd);
        bd->shutdown();
        delete bd;
        return rc;
    }

    render::FontManager fm;
    if (!fm.init()) return 1;
    FontBook fonts;
    if (!fonts.init(fm)) {
        std::fprintf(stderr, "font init failed\n");
        return 1;
    }
    render::Renderer r;
    if (!r.init(bd, &fm)) return 1;

    term::VTermBridge vt;
    if (!vt.init(layout::kTermRows, layout::kTermCols)) {
        std::fprintf(stderr, "vterm init failed\n");
        return 1;
    }
    term::Pty pty;
    const char* shell = std::getenv("SHELL");
    if (!pty.spawn({shell && *shell ? shell : "/bin/bash"}, layout::kTermCols, layout::kTermRows))
        return 1;

    if (!runCmd.empty()) {
        // 先等 shell 画出提示符：fish 初始化时 tcsetattr(TCSAFLUSH) 会丢弃已写入的输入
        for (int i = 0; i < 80 && vt.bytesIn() == 0; ++i) {
            struct pollfd pfd{pty.masterFd(), POLLIN, 0};
            ::poll(&pfd, 1, 16);
            if (!pumpPty(pty, vt)) break;
        }
        std::string line = runCmd + "\r";
        pty.writeMaster(line.data(), line.size());
        for (int i = 0; i < 30; ++i) {
            struct pollfd pfd{pty.masterFd(), POLLIN, 0};
            ::poll(&pfd, 1, 16);
            if (!pumpPty(pty, vt)) break;
        }
    }

    TerminalView view;
    view.grid = &vt;
    view.cols = layout::kTermCols;
    view.rows = layout::kTermRows;
    view.cellW = layout::kCellW;
    view.cellH = layout::kCellH;

    ime::BuiltinIme ime;
    if (!ime.load(findRes("data/pinyin.idx"), findRes("data/ngram.bin")))
        std::fprintf(stderr, "警告：没有 data/pinyin.idx，中文输入不可用\n");

    UiState ui;
    ui.ime = ime.chinese ? "BUILTIN" : "EN";
    if (std::getenv("SIT_SYMBOL")) ui.layer = "SYMBOL";  // 无头验符号层的开关
    input::UinputKeyboard ukbd;
    bool useUinput = (sink == "uinput");
    if (useUinput && !ukbd.open()) {
        std::fprintf(stderr, "uinput 打不开，回落到自带 PTY\n");
        useUinput = false;
    }
    if (useUinput) std::fprintf(stderr, "sink: uinput（打到前台程序）\n");
    auto emit = [&](const std::string& s) {  // 映射输出的唯一出口
        if (s.empty()) return;
        if (useUinput)
            ukbd.typeBytes(s);
        else
            pty.writeMaster(s.data(), s.size());
    };
    std::string pending;  // 待写 PTY
    std::vector<HitCell> hits;
    // 中英切换：正在组的拼音原样落到 shell（别吞字）
    auto toggleLang = [&]() {
        std::string raw = ime.toggle();
        if (std::getenv("SIT_DEBUG"))
            std::fprintf(stderr, "[ime] switch -> %s, 交出=\"%s\"\n",
                         ime.chinese ? "ZH" : "EN", raw.c_str());
        pending += raw;
        ui.ime = ime.chinese ? "BUILTIN" : "EN";
    if (std::getenv("SIT_SYMBOL")) ui.layer = "SYMBOL";  // 无头验符号层的开关
        ui.lang = ime.chinese ? "ZH" : "EN";
    };
    // 面板动作统一入口：点击 / 键盘 / 将来的手柄按钮都走这里
    auto doAction = [&](int a) {
        if (a == ui::kActIme) {
            toggleLang();
        } else if (a == ui::kActCopy) {
            // 有选区就复制选区，没有就复制整个可见区
            clipboardWrite(ui::selActive(view) ? ui::selectedText(view) : ui::visibleText(view));
            ui::selClear(view);
            vt.markDirty();
        } else if (a == ui::kActPaste) {
            std::string clip = clipboardRead();
            if (!clip.empty()) {
                pending += clip;
                vt.markDirty();
            }
        } else if (a == ui::kActLockCtrl || a == ui::kActLockShift || a == ui::kActLockAlt) {
            unsigned bit = a == ui::kActLockCtrl ? 1u : a == ui::kActLockShift ? 2u : 4u;
            bd->locks ^= int(bit);
            ui.locks = bd->locks;
            vt.markDirty();
        } else if (a == ui::kActStickyCtrl || a == ui::kActStickyShift || a == ui::kActStickyAlt) {
            unsigned bit = a == ui::kActStickyCtrl ? 1u : a == ui::kActStickyShift ? 2u : 4u;
            bd->sticky ^= int(bit);  // 再按一下取消
            ui.sticky = bd->sticky;
            vt.markDirty();
        } else if (a == ui::kActLayer) {  // L：字母层 ↔ 符号/数字层
            ui.layer = ui.layer == "ALPHA" ? "SYMBOL" : "ALPHA";
            vt.markDirty();
        } else if (a == ui::kActCursorLeft || a == ui::kActCursorRight) {
            int dir = (a == ui::kActCursorRight) ? 1 : -1;
            if (bd->locks & 2) {  // Shift 锁着 = 扩选，不发光标键
                if (!ui::selActive(view)) {
                    int cr = 0, cc = 0;
                    vt.cursor(cr, cc);
                    view.selAnchor = view.selCaret = cr * view.cols + cc;
                }
                ui::selStep(view, dir);
                vt.markDirty();
            } else {
                pending += (dir > 0 ? "\x1b[C" : "\x1b[D");
            }
        } else if (a == ui::kActSelLeft || a == ui::kActSelRight) {
            if (!ui::selActive(view)) {  // 从终端光标处开始选
                int cr = 0, cc = 0;
                vt.cursor(cr, cc);
                view.selAnchor = view.selCaret = cr * view.cols + cc;
            }
            ui::selStep(view, a == ui::kActSelRight ? 1 : -1);
            vt.markDirty();
        }
    };
    // 输入路由：键盘字节和手柄字节共用。中文模式下字母进 IME，锁 Ctrl 时空格 = Ctrl+Space
    auto routeKeys = [&](const std::string& in) {
        // 修饰键：锁定的常驻，sticky 的只作用于这一个键（实体键盘"先按 Ctrl 再按空格"）
        const bool ctrl = (bd->locks | bd->sticky) & 1;
        const bool shift = (bd->locks | bd->sticky) & 2;
        for (char chRaw : in) {
            char ch = chRaw;
            if (ctrl && ch == ' ') ch = 0x00;  // Ctrl+空格 = 切中英
            else if (ctrl && ch >= 'a' && ch <= 'z') ch = char(ch - 'a' + 1);   // Ctrl+C 等
            else if (shift && ch >= 'a' && ch <= 'z') ch = char(ch - 'a' + 'A');
            if (ch == 0x00) {
                toggleLang();
                continue;
            }
            if (ime.chinese && ime.composing()) {
                if (ch >= '1' && ch <= '7') {
                    pending += ime.commit((ime.page() * 7) + (ch - '1'));
                    continue;
                }
                if (ch == ' ') {
                    pending += ime.commit(-1);
                    continue;
                }
            }
            if (ime.chinese && ch >= 'a' && ch <= 'z') {
                ime.push(ch);
                continue;
            }
            if (ime.chinese && ch == 0x7f && ime.composing()) {
                ime.backspace();
                continue;
            }
            if (ime.chinese && ch == '\t' && ime.composing()) {
                ime.nextPage();
                continue;
            }
            if (ime.chinese && ch == '\r' && ime.composing()) {
                pending += ime.commit(-1);
                continue;
            }
            if (ch == 0x1b && ui::selActive(view)) {  // Esc 取消选区（照样发给 shell）
                ui::selClear(view);
                vt.markDirty();
            }
            if (ime.composing() && ch < 0x20) ime.clear();  // 控制键取消组字
            pending += ch;
        }
        if (bd->sticky) {  // 一次性修饰键用完即释放
            bd->sticky = 0;
            ui.sticky = 0;
        }
    };

    input::EvdevGamepad gamepad;
    if (useGamepad) {
        int n = gamepad.open();
        if (n > 0) {
            for (const auto& nm : gamepad.names())
                std::fprintf(stderr, "gamepad: %s\n", nm.c_str());
        } else {
            std::fprintf(stderr, "gamepad: 没有可读的手柄（Switch 上要让用户进 input 组）\n");
        }
    }
    input::GamepadHandlers gph{
        [&](const std::string& s) {
            routeKeys(s);
            vt.markDirty();
        },
        [&](int a) { doAction(a); }};

    // 布局热重载：layout.lua 的 mtime 变了就重读，不用重启
    auto layoutMTime = [](const std::string& p) -> long long {
        struct stat st {};
        if (::stat(p.c_str(), &st) != 0) return 0;
        return (long long)st.st_mtime * 1000000000LL + (long long)st.st_mtim.tv_nsec;
    };
    long long layoutStamp = layoutMTime(layoutResolved);
    long long nextStatMs = 0;

    // 应用缩放：重排布局 → 重建字体 → 换画布 → 终端列数变了就 resize
    auto applyScale = [&](float want) {
        if (std::fabs(want - ui::layout::kScale) < 0.06f) return;
        ui::layout::setScale(want);
        fonts.init(fm);
        bd->setCanvasSize(ui::layout::kScreenW, ui::layout::kScreenH);
        view.cols = ui::layout::kTermCols;
        view.rows = ui::layout::kTermRows;
        view.cellW = ui::layout::kCellW;
        view.cellH = ui::layout::kCellH;
        if (view.cols != vt.colsNow() || view.rows != vt.rowsNow()) {
            vt.resize(view.rows, view.cols);
            pty.resize(view.cols, view.rows);
        }
        vt.markDirty();
        std::fprintf(stderr, "ui: 缩放 %.2fx → 画布 %dx%d，终端 %dx%d\n", ui::layout::kScale,
                     ui::layout::kScreenW, ui::layout::kScreenH, view.cols, view.rows);
    };

    int lastScroll = -1;   // 上次绘制用的滚动位置
    int lastHist = 0;      // 上次见到的历史行数
    bool running = true;
    // 布局热重载：Ctrl+Shift+R 手动触发（不轮询，改完按一下）
    auto reloadLayout = [&]() {
        if (!ui::layout::loadFile(layoutResolved.c_str())) return;
        ui::layout::recompute();
        bd->setCanvasSize(ui::layout::kScreenW, ui::layout::kScreenH);
        fonts.init(fm);
        view.cols = ui::layout::kTermCols;
        view.rows = ui::layout::kTermRows;
        view.cellW = ui::layout::kCellW;
        view.cellH = ui::layout::kCellH;
        vt.resize(view.rows, view.cols);
        pty.resize(view.cols, view.rows);
        vt.markDirty();
        std::fprintf(stderr, "layout: 重载 %s → 终端 %dx%d\n", layoutResolved.c_str(), view.cols,
                     view.rows);
    };

    std::signal(SIGHUP, onReloadSignal);
    std::signal(SIGUSR1, onReloadSignal);

    do {
        std::string keys;
        std::vector<disp::Point> clicks;
        running = bd->pump(keys, clicks);  // 无窗口后端返回 false：只画一帧
        if (useGamepad && gamepad.count() > 0) gamepad.poll(gph);
        if (bd->uiDirty) {  // 窗口被瓦片/全屏改了尺寸 → 重排 + 补一帧
            bd->uiDirty = 0;
            applyScale(bd->scaleHint(ui::layout::kDesignW, ui::layout::kDesignH));
            vt.markDirty();
        }
        if (gReloadReq) {  // kill -USR1 / -HUP
            gReloadReq = 0;
            reloadLayout();
        }
        if (bd->reloadReq) {  // Ctrl+Shift+R
            bd->reloadReq = 0;
            reloadLayout();
        }
        if (bd->shiftTap) {  // 单独按一下 Shift = 中英切换
            toggleLang();
            bd->shiftTap = 0;
        }
        if (bd->pasteReq) {  // Ctrl+Shift+V
            doAction(ui::kActPaste);
            bd->pasteReq = 0;
        }
        if (bd->copyReq) {  // Ctrl+Shift+C
            doAction(ui::kActCopy);
            bd->copyReq = 0;
        }
        if (bd->selReq) {  // Shift+左右 / 手柄 Shift+ZL·ZR
            if (!ui::selActive(view)) {
                int cr = 0, cc = 0;
                vt.cursor(cr, cc);
                view.selAnchor = view.selCaret = cr * view.cols + cc;
                vt.markDirty();
            }
            ui::selStep(view, bd->selReq);
            vt.markDirty();
            bd->selReq = 0;
        }
        if (bd->wheel) {  // 滚轮/翻页 → 回看历史
            view.scroll = std::clamp(view.scroll + bd->wheel * 3, 0, vt.historySize());
            if (std::getenv("SIT_DEBUG"))
                std::fprintf(stderr, "[scroll] wheel=%d -> scroll=%d (历史 %d 行)\n", bd->wheel,
                             view.scroll, vt.historySize());
            bd->wheel = 0;
        }
        for (auto& c : bd->dblClicks) {  // 双击：只用来切换修饰键锁定
            for (auto& h : hits) {
                if (c.x >= h.rect.x && c.x < h.rect.right() && c.y >= h.rect.y &&
                    c.y < h.rect.bottom()) {
                    doAction(h.dblAction);
                    break;
                }
            }
        }
        bd->dblClicks.clear();
        for (auto& c : clicks) {  // 鼠标/触摸：命中面板字符格 = 等同于按下该键
            for (auto& h : hits) {
                if (c.x >= h.rect.x && c.x < h.rect.right() && c.y >= h.rect.y &&
                    c.y < h.rect.bottom()) {
                    if (h.action)
                        doAction(h.action);
                    else
                        keys += h.bytes;
                    break;
                }
            }
        }
        routeKeys(keys);
        if (!pending.empty()) {
            if (std::getenv("SIT_DEBUG"))
                std::fprintf(stderr, "[pty<-] %zu 字节: \"%s\"\n", pending.size(),
                             pending.c_str());
            emit(pending);
            pending.clear();
        }

        struct pollfd pfd{pty.masterFd(), POLLIN, 0};
        if (::poll(&pfd, 1, 16) > 0 && (pfd.revents & POLLIN)) {
            if (!pumpPty(pty, vt)) running = false;
        }

        // 回看时来了新输出：钉住正在看的内容，别被顶走
        int histNow = vt.historySize();
        if (view.scroll > 0 && histNow > lastHist)
            view.scroll = std::min(view.scroll + (histNow - lastHist), histNow);
        lastHist = histNow;

        if (!vt.dirty() && ime.preedit() == view.preedit && ime.cands() == view.cands &&
            ime.page() == view.candPage && view.scroll == lastScroll)
            continue;  // preedit / 候选 / 页码 / 滚动位置 变化都要重绘
        vt.clearDirty();
        view.preedit = ime.preedit();
        view.cands = ime.cands();
        view.candPage = ime.page();
        view.candPageCount = ime.pageCount();
        lastScroll = view.scroll;

        int cr = 0, cc = 0;
        vt.cursor(cr, cc);
        bd->setTextInputRect({layout::kTermX + layout::kTermPadX + cc * layout::kCellW,
                              layout::kContentY + layout::kTermPadTop + cr * layout::kCellH,
                              layout::kCellW, layout::kCellH});

        r.beginFrame();
        r.fill({0, 0, layout::kScreenW, layout::kScreenH}, c(col::kBg));
        drawTopBar(r, fonts, ui);
        hits.clear();
        drawLeftPanel(r, fonts, 0, ui, &hits);
        drawTerminal(r, fonts, view);
        drawRightPanel(r, fonts, layout::kScreenW - layout::kPanelW, ui, &hits);
        drawBottomBar(r, fonts, ui);
        r.endFrame();
    } while (running);

    if (backend == "headless") {
        if (!bd->captureFrame(out)) {
            std::fprintf(stderr, "capture failed: %s\n", out.c_str());
            return 1;
        }
        std::fprintf(stderr, "[out] %s (%dx%d)\n", out.c_str(), layout::kScreenW,
                     layout::kScreenH);
    }

    bd->shutdown();
    delete bd;
    return 0;
}
