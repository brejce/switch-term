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
#include <vector>

#include "app/app.h"
#include "app/ime_route.h"
#include "app/physics.h"
#include "app/tabs.h"
#include "input/uinput_mouse.h"
#include "config/lua_config.h"
#include "ui/screen.h"
#include "input/evdev_gamepad.h"
#include "input/uinput_kbd.h"
#include "platform/display_backend.h"
#include "render/font_manager.h"
#include "ime/builtin_ime.h"
#include "input/gamepad.h"
#include "render/renderer.h"
#include "term/osc_filter.h"
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
        "  --tabs <n>         启动时开 n 个终端 tab（默认 1，截图验多 tab 用）\n"
        "  --list-backends    列出已注册后端\n"
        "  --list-gamepad     列出 /dev/input 设备与程序选中了哪些（手柄排障用）\n"
        "  --theme <name>     配色主题（--list-themes 可查）\n"
        "  --list-themes      列出配色主题\n"
        "  --layout <path>    布局文件（默认 layout.lua）\n"
        "  --sink pty|uinput  输出到自带 PTY，还是 uinput 打给前台程序\n"
        "  --type <text>      只调 uinput 打字然后退出（自检用）\n"
        "  --no-gamepad       不打开 evdev 手柄（默认自动扫 /dev/input/event*）\n"
        "  --print-config     打印从 config/*.lua 装配出来的原生结构\n"
        "  --audit-ime        逐条核对输入法行为（标准输入法对照表）\n"
        "  --audit-input      核对手柄按键/摇杆映射 + keymap 装配（合成事件）\n"
        "  --audit-physics    核对引力场：零位/推进/穿越速度/回落/脱开 + 确定性（PHYSICS.md §5）\n"
        "  --ui-preview <png> 只渲染新 UI（M0）到 PNG，不启 PTY\n"

        "  --preview-keys     配合 --ui-preview：只画 v2 的 30 键静态键帽\n"
        "  --preview-layer <0|1|2>  --preview-keys 时画哪一层（0=ALPHA 1=NUM 2=SYM）\n"
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

// 多 tab 下标自检：切/关都只动下标，循环与边界靠断言验
static int tabsSelfTest() {
    bool ok = app::tabStep(0, 3, -1) == 2 && app::tabStep(2, 3, 1) == 0 &&
              app::tabStep(0, 1, 1) == 0 && app::tabAfterClose(2, 3) == 1 &&
              app::tabAfterClose(0, 2) == 0 && app::tabAfterClose(0, 1) == 0;
    std::printf("[selftest] tabs %s  回绕 0-1→2 2+1→0 只1个不动；关闭 最后→前一个 其余不变\n",
                ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// OSC 过滤器自检：term/quit/标题三类标记必须剥掉，其他字节一个不漏、不跨边界丢
static int oscSelfTest() {
    std::vector<term::OscEvent> ev;
    std::string pend;
    auto feed = [&](const char* s, size_t n) {
        ev.clear();
        return term::oscFilter(pend, s, n, &ev);
    };
    const char* quitSeq = "\033]1337;quit\a";
    const char* titleSeq = "ab\033]0;前端\a";
    const char* newSeq = "\033]1337;newtab=二号\a";
    const char* mixSeq = "x\033]1337;quit\ay";

    std::string o1 = feed(quitSeq, std::strlen(quitSeq));
    bool a1 = o1.empty() && ev.size() == 1 && ev[0].kind == term::OscEvent::Quit;

    std::string o2 = feed(titleSeq, std::strlen(titleSeq));
    bool a2 = o2 == "ab" && ev.size() == 1 && ev[0].kind == term::OscEvent::Title &&
              ev[0].arg == "前端";
    std::string t2 = ev.empty() ? "" : ev[0].arg;

    size_t half = std::strlen(newSeq) / 2;  // 序列被切成两次 read
    std::string o3a = feed(newSeq, half);
    size_t n3a = ev.size();
    std::string o3b = feed(newSeq + half, std::strlen(newSeq) - half);
    bool a3 = o3a.empty() && n3a == 0 && o3b.empty() && ev.size() == 1 &&
              ev[0].kind == term::OscEvent::NewTab && ev[0].arg == "二号";

    std::string o4 = feed(mixSeq, std::strlen(mixSeq));
    bool a4 = o4 == "xy" && ev.size() == 1 && ev[0].kind == term::OscEvent::Quit;

    bool ok = a1 && a2 && a3 && a4;
    std::printf("[selftest] osc %s\n", ok ? "OK" : "FAIL");
    std::printf("  ① 单发 quit：透传=%zu 字节 事件=1(Quit) → %s\n", o1.size(),
                a1 ? "OK" : "FAIL");
    std::printf("  ② 文本+标题：透传=\"%s\" 标题=\"%s\" → %s\n", o2.c_str(), t2.c_str(),
                a2 ? "OK" : "FAIL");
    std::printf("  ③ 跨 read：前半 透传=%zu 事件=%zu；后半 透传=%zu 事件=%zu 名字=\"%s\" → %s\n",
                o3a.size(), n3a, o3b.size(), ev.size(), a3 ? "二号" : "?", a3 ? "OK" : "FAIL");
    std::printf("  ④ 夹在文本中：透传=\"%s\" 事件=1(Quit) → %s\n", o4.c_str(),
                a4 ? "OK" : "FAIL");
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
    ok = ok && e2.imeEnabled;  // 没写 engine.ime.enabled = 默认启用
    if (FILE* f = std::fopen(path, "w")) {
        std::fputs("engine = {}\nengine.ime = { enabled = false }\n", f);
        std::fclose(f);
    }
    cfg::Engine e3;
    ok = ok && cfg::loadEngine(path, e3) && !e3.imeEnabled;
    std::remove(path);

    // 直通路径（ime.enabled = false）：不组字、不切语言、修饰键照旧
    bool raw = app::routeRaw('b').bytes == "b" && !app::routeRaw('b').consumed;
    raw = raw && app::routeRaw('b', 1).bytes == std::string(1, char(0x02));
    raw = raw && app::routeRaw('b', 4).bytes == "\x1b" "b";
    raw = raw && app::routeRaw('b', 2).bytes == "B";
    raw = raw && app::routeRaw('\t').bytes == "\t" && app::routeRaw(0x7f).bytes == "\x7f";
    raw = raw && app::routeRaw(0x00).bytes.empty() && !app::routeRaw(0x00).consumed;  // 不切语言
    raw = raw && app::routeRaw('\xe4').bytes == std::string(1, char(0xe4));  // 高位字节原样
    ok = ok && raw;
    std::printf("[selftest] config %s  scrollback=%d double_tap=%dms sink=%s ime_enabled=%d\n",
                ok ? "OK" : "FAIL", e2.scrollback, e2.doubleTapMs, e2.sink.c_str(),
                e2.imeEnabled);
    std::printf("[selftest] raw-route %s  b=\"b\" Ctrl+b=0x02 Ctrl+Space=空 高位字节直通\n",
                raw ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// uinput 自检：没开设备也能验「跳过非 ASCII」是按字符计数、且不卡死（tap 在未 open 时空转）
static int uinputSelfTest() {
    input::UinputKeyboard kb;
    kb.typeBytes("你好abc", 0);  // 2 个中文字 = 6 个 UTF-8 字节
    bool ok = kb.dropped() == 2;
    std::printf("[selftest] uinput %s  未开设备时丢字计数=%zu（期望 2）\n", ok ? "OK" : "FAIL",
                kb.dropped());
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

// 手柄自检：只核 C++ 这层（evdev 键码 → PadButton / 摇杆扇区）。
// 键 → 动作在 config/keymap.lua，不在这里测（那张表已经不在 C++ 里了）。
static int gamepadEvdevSelfTest() {
    input::PadState pad(0.35f, 8.f);  // = engine.input 的默认死区/滞回
    for (int code : {ABS_X, ABS_Y, ABS_RX, ABS_RY}) pad.setAxisRange(code, 0, 255);
    int btn = -1, lsec = -2, rsec = -2;
    input::PadHandlers h{[&](int b) { btn = b; },
                         [&](int s) { lsec = s; },
                         [&](int s) { rsec = s; }};
    auto ev = [&](int t, int c, int v) {
        input_event e{};
        e.type = t;
        e.code = c;
        e.value = v;
        pad.onEvent(e, h);
    };
    struct Case {
        int code, want;
        const char* name;
    };
    const Case cases[] = {
        {BTN_TL, input::kPadLB, "L(BTN_TL)"},       {BTN_TL2, input::kPadLT, "ZL(BTN_TL2)"},
        {BTN_TR, input::kPadRB, "R(BTN_TR)"},       {BTN_TR2, input::kPadRT, "ZR(BTN_TR2)"},
        {BTN_EAST, input::kPadA, "A"},              {BTN_SOUTH, input::kPadB, "B"},
        {BTN_NORTH, input::kPadX, "X"},             {BTN_WEST, input::kPadY, "Y"},
        {BTN_DPAD_UP, input::kPadUp, "DPAD_UP"},    {BTN_DPAD_DOWN, input::kPadDown, "DPAD_DOWN"},
        {BTN_DPAD_LEFT, input::kPadLeft, "DPAD_LEFT"},
        {BTN_DPAD_RIGHT, input::kPadRight, "DPAD_RIGHT"}, {BTN_START, input::kPadPlus, "+"},
        {BTN_THUMBL, input::kPadL3, "L3"},
    };
    bool ok = true;
    for (const Case& c : cases) {
        btn = -1;
        ev(EV_KEY, c.code, 1);
        if (btn != c.want) {
            ok = false;
            std::printf("  [gamepad] %s → %d（期望 %d）\n", c.name, btn, c.want);
        }
    }
    ev(EV_ABS, ABS_X, 255);  // 左摇杆推右 → 扇区 0
    ev(EV_ABS, ABS_Y, 127);
    ev(EV_SYN, SYN_REPORT, 0);
    const bool s0 = (lsec == 0);
    ev(EV_ABS, ABS_X, 127);  // 回中 → -1
    ev(EV_SYN, SYN_REPORT, 0);
    const bool sBack = (lsec == -1);
    ok = ok && s0 && sBack;
    std::printf("[selftest] gamepad %s  键码 %zu 项  左摇杆右→%d  回中→%d\n", ok ? "OK" : "FAIL",
                sizeof(cases) / sizeof(cases[0]), (s0 ? 0 : -9), (sBack ? -1 : -9));
    return ok ? 0 : 1;
}

// 相对路径解析：先从当前目录找，再从可执行文件所在目录找（从别处启动也能跑）
static std::string exeDir;
static std::string findRes(const std::string& rel) {
    if (std::ifstream(rel).good()) return rel;
    for (const std::string& dir : {exeDir, exeDir + "/.."}) {  // 二进制在 build/ 时数据在上一级
        std::string p = dir + "/" + rel;
        if (std::ifstream(p).good()) return p;
    }
    return rel;  // 都没有就返回原路径，让调用方报错
}

// --audit-physics：docs/PHYSICS.md §5 的断言 + 确定性。键位取 ui.lua、旋钮取 engine.lua，
// 所以调配置后这里会直接反映出来；跑两遍才能验的确定性也在这里一次性验完。
static int auditPhysics() {
    cfg::Ui uic;
    if (!cfg::loadUi(findRes("config/ui.lua").c_str(), uic)) return 1;
    cfg::Engine eng;
    cfg::loadEngine(findRes("config/engine.lua").c_str(), eng);
    eng.physics.deadzone = eng.deadzone;  // 和 app 主循环一致：曲线起点 = 死区边缘
    const app::physics::Params& pr = eng.physics;

    // 物理场吃键中心：ui.keys 是键帽左上角，边长 ui.key_size
    float kx[cfg::kKeyV2Count], ky[cfg::kKeyV2Count];
    const float half = float(uic.keySize) * 0.5f;
    for (int i = 0; i < cfg::kKeyV2Count; ++i) {
        kx[i] = float(uic.keysV2[i].x) + half;
        ky[i] = float(uic.keysV2[i].y) + half;
    }
    const app::physics::Field left{0, 7};    // 左场：键 0..14，零位 d
    const app::physics::Field right{15, 22};  // 右场：键 15..29，零位 k

    int bad = 0;
    auto ck = [&](const char* n, bool ok, const std::string& got, const std::string& exp) {
        if (!ok) ++bad;
        std::printf("  %-24s %s 实得=%s 期望=%s\n", n, ok ? "OK  " : "FAIL", got.c_str(),
                    exp.c_str());
    };
    auto n1 = [](float v) {  // 一位小数的打印格式
        char b[32];
        std::snprintf(b, sizeof b, "%.1f", double(v));
        return std::string(b);
    };
    auto dToHome = [&](const app::physics::Field& f, const app::physics::Pointer& p) {
        const float dx = p.x - kx[f.home], dy = p.y - ky[f.home];
        return std::sqrt(dx * dx + dy * dy);
    };
    auto nRel = [&](const std::vector<app::physics::PhysEventData>& ev) {
        int n = 0;
        for (const auto& e : ev)
            if (e.kind == app::physics::PhysEvent::Release) ++n;
        return n;
    };

    struct Run {
        app::physics::Pointer p;
        std::vector<app::physics::PhysEventData> ev;
        float x0 = 0;  // 起点 = 零位键中心
    };
    // 固定脚本跑 frames 帧：每帧回调给 (ix, iy)，语义同 stickVec（上 = +y）
    auto run = [&](const app::physics::Field& f, int frames,
                   const std::function<void(int, float&, float&)>& in) {
        Run r;
        r.p.x = r.x0 = kx[f.home];
        r.p.y = ky[f.home];
        for (int i = 0; i < frames; ++i) {
            float ix = 0, iy = 0;
            in(i, ix, iy);
            app::physics::step(r.p, f, kx, ky, ix, iy, pr,
                                [&](const app::physics::PhysEventData& e) { r.ev.push_back(e); });
        }
        return r;
    };

    std::printf("引力场（ui.lua 键位 + engine.physics 参数）：\n");

    // 1. 零位静止 → 吸附在 home
    Run still = run(left, 30, [](int, float&, float&) {});
    Run stillR = run(right, 30, [](int, float&, float&) {});
    ck("零位吸附 左 d(7)", still.p.snapped == left.home, std::to_string(still.p.snapped), "7");
    ck("零位吸附 右 k(22)", stillR.p.snapped == right.home, std::to_string(stillR.p.snapped),
       "22");

    // 2. 向右推半程 0.5s → 吸附键下标递增
    Run push = run(left, 30, [](int, float& ix, float& iy) {
        ix = 0.5f;
        iy = 0.f;
    });
    int maxSnap = -1;
    for (const auto& e : push.ev)
        if (e.kind == app::physics::PhysEvent::Snap) maxSnap = std::max(maxSnap, e.key);
    ck("右推 0.5s 下标递增", maxSnap > left.home, "最大 snap=" + std::to_string(maxSnap), ">7");
    ck("右推 0.5s 指针右移", push.p.x > push.x0, "Δx=" + n1(push.p.x - push.x0), ">0");

    // 3. 速度曲线三段（PHYSICS.md §2.1）：中段最快，推杆 0.2 / 1.0 两端都更慢（微操）
    const float threeKeys = kx[left.home + 2] - kx[left.home];
    auto crossFrames = [&](float ix) {
        app::physics::Pointer p;
        p.x = kx[left.home];
        p.y = ky[left.home];
        int n = 0;
        while (p.x - kx[left.home] < threeKeys && n < 900) {
            app::physics::step(p, left, kx, ky, ix, 0.f, pr);
            ++n;
        }
        return n;
    };
    const int tMicro = crossFrames(0.3f), tFast = crossFrames(0.8f), tFull = crossFrames(1.f);
    // 接缝两侧（微操段末端 0.5 附近）用不同推杆量跑，耗时应几乎一样 = 没有速度阶跃
    const int tSeamA = crossFrames(0.48f), tSeamB = crossFrames(0.52f);
    auto fr = [](int n) { return " " + std::to_string(n) + " 帧"; };
    ck("内圈微操慢于快速段", tMicro > tFast, fr(tMicro) + " vs" + fr(tFast), "0.3>0.8 帧");
    ck("快速段越推越快", tSeamA > tFast, fr(tSeamA) + " vs" + fr(tFast), "0.48>0.8 帧");
    ck("推满最快", tFull < tFast, fr(tFull) + " vs" + fr(tFast), "1.0<0.8 帧");
    ck("两段接缝平滑（无速度阶跃）", std::abs(tSeamA - tSeamB) <= 2,
       fr(tSeamA) + " vs" + fr(tSeamB), "差 ≤2 帧");

    // 4. 右推后松手：home_pull > 0 → 松手 0.3s 后自己滑回 home（§5.4）
    Run back = run(left, 12 + 60, [](int i, float& ix, float& iy) {
        ix = i < 12 ? 1.f : 0.f;
        iy = 0.f;
    });
    ck("松手后回 home（home_pull）", dToHome(left, back.p) < 8.f,  // 8px ≈ 键距 70 的 1/9，肉眼看已经归位
       "距 home=" + n1(dToHome(left, back.p)), "<8px");

    // 4.5 吸附力度：关掉 home_pull，从键心右 18px 松手 → 只靠键的引力被拽回键心
    {
        app::physics::Params p0 = pr;
        p0.homePull = 0.f;  // 只看键的引力，不看零位回落
        app::physics::Pointer p;
        p.x = kx[left.home] + 18.f;
        p.y = ky[left.home];
        int n = -1;
        for (int i = 0; i < 90; ++i) {
            app::physics::step(p, left, kx, ky, 0.f, 0.f, p0);
            if (n < 0 && dToHome(left, p) < 2.f) n = i + 1;
        }
        ck("引力把球吸到键心", n > 0, "距键心=" + n1(dToHome(left, p)) + "  " + std::to_string(n) + " 帧到位",
           "<2px");
    }

    // 4.6 不许抖：拨 6 帧再松手，球在键心附近不能一直摆（常量引力会留下 2px 极限环 → 键窗跟着抖）
    {
        app::physics::Params p0 = pr;
        p0.homePull = 0.f;  // 只看引力本身，别让零位回落替它收尾
        app::physics::Pointer p;
        p.x = kx[left.home];
        p.y = ky[left.home];
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < 200; ++i) {
            app::physics::step(p, left, kx, ky, i < 6 ? 0.6f : 0.f, 0.f, p0);
            if (i > 100) {
                lo = std::fmin(lo, p.x);
                hi = std::fmax(hi, p.x);
            }
        }
        ck("松手后不抖（无极限环）", hi - lo < 0.5f, "摆幅=" + n1(hi - lo) + "px", "<0.5px");
    }

    // 5. 向左推再反向 → 触发 release
    Run rev = run(left, 40, [](int i, float& ix, float& iy) {
        ix = i < 20 ? -1.f : 1.f;
        iy = 0.f;
    });
    ck("反向触发 release", nRel(rev.ev) > 0, "release×" + std::to_string(nRel(rev.ev)), ">0");

    // 5.5 不许闪：从 d 一路推到邻键，snap 事件数不能超过“路过几个键 + 1”。
    // 回归目标：以前 release 里有个“推的方向背离吸附键”的条件，与引力对抗，
    // 指针被拉过键心后每帧 release+snap 一次（屏上就是键在闪）。
    {
        Run sweep = run(left, 80, [](int i, float& ix, float& iy) {
            ix = i < 60 ? 0.45f : 0.f;
            iy = 0.f;
        });
        int snaps = 0;
        for (const auto& e : sweep.ev)
            if (e.kind == app::physics::PhysEvent::Snap) ++snaps;
        ck("滑动选键不闪（snap 次数有界）", snaps > 0 && snaps <= 8,
           "snap×" + std::to_string(snaps), "1..8");
    }

    // §5 末条：同一输入序列跑两次，事件与坐标逐位相同
    auto script = [](int i, float& ix, float& iy) {
        if (i < 30) {
            ix = 0.8f, iy = 0.f;
        } else if (i < 45) {
            ix = 0.f, iy = 0.f;
        } else if (i < 75) {
            ix = -0.6f, iy = 0.4f;  // 左上（数学坐标上 = +y）
        } else {
            ix = 0.f, iy = 0.f;
        }
    };
    Run d1 = run(left, 120, script), d2 = run(left, 120, script);
    auto sameF = [](float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; };
    bool same = d1.ev.size() == d2.ev.size() && sameF(d1.p.x, d2.p.x) && sameF(d1.p.y, d2.p.y) &&
                sameF(d1.p.vx, d2.p.vx) && sameF(d1.p.vy, d2.p.vy) &&
                sameF(d1.p.idle, d2.p.idle) && d1.p.snapped == d2.p.snapped &&
                d1.p.hovered == d2.p.hovered;
    for (size_t i = 0; same && i < d1.ev.size(); ++i)
        same = d1.ev[i].kind == d2.ev[i].kind && d1.ev[i].key == d2.ev[i].key &&
               sameF(d1.ev[i].x, d2.ev[i].x) && sameF(d1.ev[i].y, d2.ev[i].y);
    ck("同序列两次逐位相同", same, "事件 " + std::to_string(d1.ev.size()) + " 条", "完全一致");

    // pad.stickVec 与 step 的 ix/iy 是同一套坐标：数学坐标，上 = +y（推上应让指针 y 变小）
    {
        input::PadState pad;
        for (int code : {ABS_X, ABS_Y, ABS_RX, ABS_RY}) pad.setAxisRange(code, 0, 255);
        auto ev = [&](int t, int c, int v) {
            input_event e{};
            e.type = t;
            e.code = c;
            e.value = v;
            pad.onEvent(e, input::PadHandlers{});
        };
        ev(EV_ABS, ABS_X, 255);  // 左摇杆：右上
        ev(EV_ABS, ABS_Y, 0);
        ev(EV_SYN, SYN_REPORT, 0);
        float dx = 0, dy = 0;
        bool vec = pad.stickVec(false, dx, dy) && dx > 0.99f && dy > 0.99f;
        ev(EV_ABS, ABS_RX, 0);  // 右摇杆：左中
        ev(EV_ABS, ABS_RY, 127);
        ev(EV_SYN, SYN_REPORT, 0);
        float rx = 0, ry = 0;
        vec = pad.stickVec(true, rx, ry) && rx < -0.99f && std::fabs(ry) < 0.01f && vec;
        ck("摇杆向量 右=+x 上=+y", vec, "左=" + n1(dx) + "/" + n1(dy) + " 右=" + n1(rx) + "/" + n1(ry),
           "+1/+1 和 -1/0");
    }

    // 输入侧死区：松手时的漂移必须滤成 0，否则回不了零位；死区外原样交给速度曲线
    {
        float ax = 0.05f, ay = 0.03f;
        app::physics::applyDeadzone(ax, ay, eng.deadzone);
        float bx = 1.f, by = 0.f;
        app::physics::applyDeadzone(bx, by, eng.deadzone);
        float cx = eng.deadzone, cy = 0.f;
        app::physics::applyDeadzone(cx, cy, eng.deadzone);
        ck("死区内漂移滤成 0", ax == 0.f && ay == 0.f, n1(ax) + "/" + n1(ay), "0/0");
        ck("死区边界滤成 0", cx == 0.f, n1(cx), "0");
        ck("死区外推杆量不缩放", bx == 1.f, n1(bx), "1");
    }

    std::printf("结果：%s（%d 项失败）\n", bad ? "有偏差" : "全部符合", bad);
    return bad ? 1 : 0;
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
    bool previewKeys = false;   // --preview-keys：只画 v2 键帽，不画顶栏/终端/面板
    int previewLayer = 0;       // --preview-layer：0=ALPHA 1=NUM 2=SYM
    float previewScale = 1.0f;
    std::string out = "frame.png";
    std::string runCmd;
    int tabs = 1;  // --tabs：无输入的环境下靠它截多 tab 的画面

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { usage(); return 0; }
        if (a == "--selftest") return selfTest() + gamepadSelfTest() + imeSelfTest() + fbdevSelfTest() + themeSelfTest() +
               scrollSelfTest() + switchSelfTest() + selectSelfTest() + gamepadEvdevSelfTest() +
               configSelfTest() + uinputSelfTest() + tabsSelfTest() + oscSelfTest();
        if (a == "--list-backends") {
            for (auto& n : disp::DisplayRegistry::instance().names()) std::printf("%s\n", n.c_str());
            return 0;
        }
        if (a == "--list-gamepad") {  // 排障：看清设备被认成什么，尤其 Switch 上左右 Joy-Con
            input::EvdevGamepad gp;
            int n = gp.open();
            std::printf("程序选中了 %d 个手柄设备（认「有 ABS_X/ABS_Y 或 BTN_SOUTH」）：\n", n);
            for (const auto& nm : gp.names()) std::printf("  [已打开] %s\n", nm.c_str());
            gp.close();
            std::printf("\n/dev/input/event* 全部设备：\n");
            for (int k = 0; k < 64; ++k) {
                char p[96];
                std::snprintf(p, sizeof p, "/sys/class/input/event%d/device/name", k);
                FILE* f = std::fopen(p, "r");
                if (!f) continue;
                char buf[160] = {0};
                if (std::fgets(buf, sizeof buf, f)) {
                    buf[std::strcspn(buf, "\n")] = 0;
                    std::printf("  event%-2d %s\n", k, buf);
                }
                std::fclose(f);
            }
            std::printf("\n注意：左右 Joy-Con 是两个独立设备，现在两个都会被打开。\n");
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
        if (a == "--audit-physics") return auditPhysics();
        if (a == "--audit-input") {
            cfg::Ui uic;
            if (!cfg::loadUi(findRes("config/ui.lua").c_str(), uic)) return 1;
            int bad = 0;
            auto ck = [&](const char* n, bool ok, const std::string& got, const std::string& exp) {
                if (!ok) ++bad;
                std::printf("  %-24s %s 实得=%s 期望=%s\n", n, ok ? "OK  " : "FAIL", got.c_str(),
                            exp.c_str());
            };
            // 手柄事件 → PadButton（合成 evdev 事件）
            std::printf("手柄映射：\n");
            input::PadState pad;
            for (int code : {ABS_X, ABS_Y, ABS_RX, ABS_RY}) pad.setAxisRange(code, 0, 255);
            std::string got;
            input::PadHandlers ph{[&](int b) {
                                      static const char* nm[] = {"LB", "LT", "RB", "RT", "A",
                                                                 "B",  "X",  "Y",  "UP",
                                                                 "DOWN", "LEFT", "RIGHT", "L3",
                                                                 "START"};
                                      got = nm[b];
                                  },
                                  [&](int sec) { got = "Lstick" + std::to_string(sec); },
                                  [](int) {}};
            auto ev = [&](int t, int c, int v) {
                input_event e{};
                e.type = t; e.code = c; e.value = v;
                pad.onEvent(e, ph);
            };
            ev(EV_KEY, BTN_EAST, 1);  // Joy-Con 物理 A 在右边 = BTN_EAST
            ck("A 键", got == "A", got, "A");
            ev(EV_KEY, BTN_TL, 1);
            ck("左上肩=L", got == "LB", got, "LB");
            ev(EV_ABS, ABS_HAT0X, -1);
            ck("十字左", got == "LEFT", got, "LEFT");
            got.clear();
            ev(EV_KEY, BTN_DPAD_DOWN, 1);  // joycond 合成设备：没有 HAT 轴
            ck("十字下(BTN_DPAD_DOWN)", got == "DOWN", got, "DOWN");
            got.clear();
            ev(EV_KEY, BTN_THUMBL, 1);  // 摇杆按下 → L3（v2 层循环）
            ck("摇杆按下(BTN_THUMBL)=L3", got == "L3", got, "L3");
            got.clear();
            ev(EV_KEY, BTN_START, 1);  // 右手柄 + → onboard（兜底）
            ck("右手柄 +(BTN_START)=START", got == "START", got, "START");
            got.clear();
            ev(EV_ABS, ABS_X, 255);
            ev(EV_ABS, ABS_Y, 127);
            ev(EV_SYN, SYN_REPORT, 0);
            ck("左摇杆推右→扇区0", got == "Lstick0", got, "Lstick0");

            // keymap.lua 真装配上了：键名/动作名写错只会往 stderr 打一行，这里直接验落表结果
            cfg::KeyMap km;
            const bool kmOk = cfg::loadKeymap(findRes("config/keymap.lua").c_str(), km);
            ck("keymap: START→onboard（+ 号兜底）",
               kmOk && km.single[cfg::kStart] == cfg::kOnboard,
               kmOk ? cfg::actionName(km.single[cfg::kStart]) : "读不到 keymap.lua", "onboard");
            ck("keymap: LB→magic", kmOk && km.single[cfg::kLB] == cfg::kMagic,
               kmOk ? cfg::actionName(km.single[cfg::kLB]) : "读不到 keymap.lua", "magic");
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
            // 2 空格 = 上屏当前选中的候选（默认本页第一个）
            o = run1(' ').bytes;
            check("空格→上屏首候选", !o.empty() && !im.composing(),
                  "'" + o + "' 组字=" + im.preedit(), "(第1候选) 组字清空");
            // 3 回车 → 拼音原样
            im.clear();
            type("nihao");
            o = run1('\r').bytes;
            check("回车→拼音原样", o == "nihao" && !im.composing(), o, "nihao");
            // 4 数字选词
            im.clear();
            type("nihao");
            o = run1('2').bytes;
            // 4b R/ZR 移动选中项后，空格上屏的就是它
            im.clear();
            type("nihao");
            const std::string c1 = im.cands()[0].text;
            const std::string c2 = im.cands()[1].text;
            im.selStep(+1);
            o = run1(' ').bytes;
            check("空格上屏选中项（R/ZR 移过）", o == c2 && c1 != c2, o, c2.c_str());
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
            o = run1('1').bytes;
            check("大写组字后选字（数字键）", o == "你好", o, "你好");
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
            static const char* kn[] = {"LB", "LT", "RB", "RT", "A", "B", "X", "Y",
                                       "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT",
                                       "L3", "START"};
            for (int k = 0; k < cfg::kKeyCount; ++k)
                if (km.single[k] || km.dbl[k]) {
                    std::printf("  %-10s 单击=%s", kn[k], cfg::actionName(km.single[k]));
                    if (km.dbl[k]) std::printf("  双击=%s", cfg::actionName(km.dbl[k]));
                    std::printf("\n");
                }
            std::printf("axis STICK_R mouse_move=%d\n", km.axisMouseMove ? 1 : 0);
            std::printf(
                "engine: scrollback=%d cand_per_page=%d ngram=%d ime_enabled=%d deadzone=%.2f "
                "hysteresis_deg=%.0f double_tap_ms=%d log=%s%s%s sink=%s\n",
                eng.scrollback, eng.candPerPage, eng.ngram, eng.imeEnabled ? 1 : 0,
                eng.deadzone, eng.hysteresisDeg, eng.doubleTapMs, eng.logLevel.c_str(),
                eng.logPath.empty() ? "" : "@", eng.logPath.c_str(), eng.sink.c_str());
            return ok ? 0 : 1;
        }
        if (a == "--type" && i + 1 < argc) { typeText = argv[++i]; continue; }
        if (a == "--ui-preview" && i + 1 < argc) { uiPreview = argv[++i]; continue; }
        if (a == "--preview-keys") { previewKeys = true; continue; }
        if (a == "--preview-layer" && i + 1 < argc) { previewLayer = std::atoi(argv[++i]); continue; }
        if (a == "--scale" && i + 1 < argc) { previewScale = std::atof(argv[++i]); continue; }
        if (a == "--list-themes") {
            for (const char* const* n = pal::names(); *n; ++n)
                std::printf("%s%s\n", *n, std::strcmp(*n, pal::current()) == 0 ? "  (默认)" : "");
            return 0;
        }
        if (a == "--backend" && i + 1 < argc) { backend = argv[++i]; continue; }
        if (a == "--out" && i + 1 < argc) { out = argv[++i]; continue; }
        if (a == "--run" && i + 1 < argc) { runCmd = argv[++i]; continue; }
        if (a == "--tabs" && i + 1 < argc) { tabs = std::atoi(argv[++i]); continue; }
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
    if (!uiPreview.empty() || previewKeys) {  // M0：只画新 UI，验排版；--preview-keys 可直画到 fb0
        cfg::Ui uic;
        if (!cfg::loadUi(findRes("config/ui.lua").c_str(), uic)) {
            std::fprintf(stderr, "读不到 config/ui.lua\n");
            return 1;
        }
        cfg::Engine peng;
        cfg::loadEngine(findRes("config/engine.lua").c_str(), peng);  // 只为拿 display.rotate
        ui::layout::setScale(previewScale);
        ui::Geometry g;
        g.fromConfig(uic, previewScale);

        // --preview-keys 不给 --ui-preview 时走真实后端（可直接画到 Switch 的 fb0）
        const char* pbn = (!previewKeys || !uiPreview.empty()) ? "headless" : backend.c_str();
        disp::DisplayBackend* pb = disp::DisplayRegistry::instance().create(pbn);
        if (!pb || !pb->init(g.screenW, g.screenH)) return 1;
        pb->setCanvasSize(g.screenW, g.screenH);
        pb->setRotation(peng.rotate);
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
        pr.beginFrame();
        pr.fill({0, 0, g.screenW, g.screenH}, ui::c(ui::col::kBg));
        if (previewKeys) {  // 三分区版式预览：左右各一个滑窗键区 + 中终端
            static const char* layerNames[] = {"ALPHA", "NUM", "SYM"};
            const int L = std::clamp(previewLayer, 0, 2);
            const int pw = int(220 * g.scale), tb = int(32 * g.scale);
            // 分区底色（与 mockup 一致：左面板 220 / 右面板 220 / 中间终端 840）
            pr.fill({0, tb, pw, g.screenH - tb}, ui::c(ui::col::kPanel));
            pr.fill({g.screenW - pw, tb, pw, g.screenH - tb}, ui::c(ui::col::kPanel));
            pr.fill({pw, tb, int(840 * g.scale), g.screenH - tb}, ui::c(ui::col::kTerm));
            pr.fill({0, 0, g.screenW, tb}, ui::c(ui::col::kPanel));
            pr.fill({0, tb - 1, g.screenW, 1}, ui::c(ui::col::kLine));
            // 与主循环同一套滑窗：指针钉在各自 home 键（d / k）中心，所以两面都居中吸附
            const float half = float(uic.keySize) * 0.5f;
            ui::drawKeyHand(pr, pfonts, uic, g, L, 0, 0, tb, pw, g.screenH - tb,
                            float(uic.keysV2[7].x) + half, float(uic.keysV2[7].y) + half, 7, -1,
                            float(app::physics::Params{}.attractRadius), 0.f);
            ui::drawKeyHand(pr, pfonts, uic, g, L, 15, g.screenW - pw, tb, pw, g.screenH - tb,
                            float(uic.keysV2[22].x) + half, float(uic.keysV2[22].y) + half, 22, -1,
                            float(app::physics::Params{}.attractRadius), 0.f);
            pr.fill({pw - 1, tb, 1, g.screenH - tb}, ui::c(ui::col::kLine));
            pr.fill({g.screenW - pw, tb, 1, g.screenH - tb}, ui::c(ui::col::kLine));

            auto tx = [&](int x, int y, const char* str, uint32_t colr) {
                pr.textBaseline(x, y, str, pfonts.kd, ui::c(colr));
                return x + pr.textWidth(str, pfonts.kd);
            };
            auto shape = [&](int x, int y, bool plus, uint32_t colr) {
                const int w = int(9 * g.scale), th = std::max(1, int(2 * g.scale));
                pr.fill({x, y - th / 2, w, th}, ui::c(colr));
                if (plus) pr.fill({x + w / 2 - th / 2, y - w / 2, th, w}, ui::c(colr));
                return x + w + int(3 * g.scale);
            };
            // 左手面板：肩键标签 / 键区（drawKeyHand 已画）/ DPAD / 底部按键表
            tx(int(10 * g.scale), int(50 * g.scale), "L 上屏", ui::col::kPurple);
            tx(int(80 * g.scale), int(50 * g.scale), "ZL 空格", ui::col::kCyan);
            tx(int(10 * g.scale), int(486 * g.scale), "// DPAD", ui::col::kCyan);
            ui::drawDpad(pr, pfonts, int(10 * g.scale), int(500 * g.scale), int(26 * g.scale), st,
                         g.scale);
            const int kb = int(640 * g.scale), kl = int(19 * g.scale);
            tx(int(10 * g.scale), kb, "L3 层循环", ui::col::kText);
            int xm = tx(int(10 * g.scale), kb + kl, "", ui::col::kText);
            xm = shape(xm, kb + kl - int(4 * g.scale), false, ui::col::kText);
            tx(xm, kb + kl, " 待定", ui::col::kText);
            tx(int(10 * g.scale), kb + 2 * kl, "DPAD ←Ctrl →Shift", ui::col::kDim);
            tx(int(10 * g.scale), kb + 3 * kl, "↓Alt ↑Tab 双击锁", ui::col::kDim);

            // 右手面板
            tx(int(g.screenW - pw + 10 * g.scale), int(50 * g.scale), "R 光标右", ui::col::kGreen);
            tx(int(g.screenW - pw + 82 * g.scale), int(50 * g.scale), "ZR 光标左", ui::col::kRed);
            tx(int(g.screenW - pw + 10 * g.scale), int(486 * g.scale), "// ABXY", ui::col::kRed);
            ui::drawAbxy(pr, pfonts, int(g.screenW - pw + 10 * g.scale), int(500 * g.scale),
                         int(28 * g.scale), g.scale, -1);
            tx(int(g.screenW - pw + 10 * g.scale), kb, "A 回车", ui::col::kText);
            tx(int(g.screenW - pw + 10 * g.scale), kb + kl, "B 退格", ui::col::kText);
            tx(int(g.screenW - pw + 10 * g.scale), kb + 2 * kl, "X Del", ui::col::kText);
            tx(int(g.screenW - pw + 10 * g.scale), kb + 3 * kl, "Y Esc", ui::col::kText);
            int xp = tx(int(g.screenW - pw + 110 * g.scale), kb, "", ui::col::kDim);
            xp = shape(xp, kb - int(4 * g.scale), true, ui::col::kDim);
            tx(xp, kb, " 待定", ui::col::kDim);
            tx(int(g.screenW - pw + 110 * g.scale), kb + kl, "CAP 待定", ui::col::kDim);
            tx(int(g.screenW - pw + 110 * g.scale), kb + 2 * kl, "HOME 待定", ui::col::kDim);

            // 顶栏
            pr.textInBox({int(16 * g.scale), int(9 * g.scale), 0, int(14 * g.scale)},
                         std::string("SYS.TERM   // LAYER ") + layerNames[L], pfonts.ptitle,
                         ui::c(ui::col::kCyan), render::Align::Left);
        } else {
            ui::drawTopBar(pr, pfonts, uic, g, st);
            ui::drawKeyPanels(pr, pfonts, uic, g, st);
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
            ui::drawBottomBar(pr, pfonts, g, st);
        }
        pr.endFrame();
        bool ok = true;
        if (!uiPreview.empty()) ok = pb->captureFrame(uiPreview);  // fbdev 不支持截图，跳过
        std::printf("ui-preview: %s  %dx%d scale=%.2f backend=%s\n", ok ? "OK" : "FAIL",
                    g.screenW, g.screenH, previewScale, pbn);
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
        int rc = app::run(bd, exeDir + "/..", out, runCmd, tabs);
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
            if (ch == 0x1b && ui::selActive(view)) {  // Esc 取消选区（照样发给 shell）
                ui::selClear(view);
                vt.markDirty();
            }
            // 输入法行为只认 app::routeKey（与新 UI 同一条路），这里不再抄第二份
            app::RouteResult rr = app::routeKey(ime, ch);
            if (rr.langChanged) toggleLang();
            pending += rr.bytes;
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
    // 键位表只有 config/keymap.lua 一份，C++ 不再自带一张
    cfg::KeyMap lkm;
    if (!cfg::loadKeymap(findRes("config/keymap.lua").c_str(), lkm))
        std::fprintf(stderr, "读不到 config/keymap.lua，手柄按键不响应\n");
    input::PadHandlers gph{
        [&](int btn) {
            if (btn >= 0 && btn < cfg::kKeyCount && lkm.single[btn]) doAction(lkm.single[btn]);
            vt.markDirty();
        },
        [&](int sec) {
            if (sec >= 0) routeKeys(input::leftLetterFor(sec));
            vt.markDirty();
        },
        [&](int sec) {
            if (sec >= 0) routeKeys(input::rightLetterFor(sec));
            vt.markDirty();
        }};

    // 布局热重载：layout.lua 的 mtime 变了就重读，不用重启
    auto layoutMTime = [](const std::string& p) -> long long {
        struct stat st {};
        if (::stat(p.c_str(), &st) != 0) return 0;
        return (long long)st.st_mtime * 1000000000LL + (long long)st.st_mtim.tv_nsec;
    };
    [[maybe_unused]] long long layoutStamp = layoutMTime(layoutResolved);
    [[maybe_unused]] long long nextStatMs = 0;

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
        std::vector<disp::ClickEvent> clicks;
        running = bd->pump(keys, clicks);  // 无窗口后端返回 false：只画一帧
        if (useGamepad && gamepad.count() > 0) gamepad.pollPad(gph);
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
            if (c.button != 1) continue;  // 旧 UI 只认左键（新增右键不动老行为）
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
