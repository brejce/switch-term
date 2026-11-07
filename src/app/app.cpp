// 新架构主循环：UI 只认 cfg::Ui，终端只认 TerminalParser，PTY/IME 是可选驱动。
#include "app/app.h"

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <poll.h>

#include "app/ime_route.h"
#include "app/physics.h"
#include "app/tabs.h"
#include "config/lua_config.h"
#include "ime/builtin_ime.h"
#include "input/evdev_gamepad.h"
#include "input/evdev_keys.h"
#include "input/evdev_touch.h"
#include "input/rumble.h"
#include "input/uinput_kbd.h"
#include "input/uinput_mouse.h"
#include "modules/terminal/terminal_parser.h"
#include "platform/damage.h"
#include "platform/x11_overlay.h"
#include "render/font_manager.h"
#include "render/renderer.h"
#include "term/osc_filter.h"
#include "term/pty.h"
#include "ui/layout.h"
#include "ui/screen.h"
#include "ui/theme.h"

namespace app {
namespace {

volatile sig_atomic_t gReload = 0;
void onReload(int) { gReload = 1; }

// 字符键被触发后闪亮的时长（毫秒）
constexpr long long kFlashMs = 150;  // 按键提示条闪一下的时长

// cfg::Ui::layers 的顺序（LAYOUT-v2 §2）：L3 循环把下标换成底部状态栏的名字
const char* const kLayerNames[cfg::kLayerCount] = {"ALPHA", "NUM"};

// 拉起外部程序（onboard 这类），不等待也不算子 tab：setsid 后 exec，失败只说一句
void spawnDetached(const char* prog) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        std::fprintf(stderr, "spawn %s: fork 失败\n", prog);
        return;
    }
    if (pid == 0) {
        ::setsid();  // 脱离本进程的会话，关掉本程序也不带走它
        ::execlp(prog, prog, static_cast<char*>(nullptr));
        std::fprintf(stderr, "spawn %s: exec 失败（没装？）\n", prog);
        ::_exit(127);
    }
}

std::string resolve(const std::string& resDir, const char* rel) {
    // resDir = 可执行文件目录的上一级；从别处启动（桌面快捷方式/装到别的路径）时它可能不是项目根，
    // 所以再试 cwd 和 resDir/..——找不到的后果是静默的（字母全被输入法吞掉），不能只赌一个路径
    for (const std::string& dir : {resDir, std::string("."), resDir + "/.."}) {
        const std::string p = dir + "/" + rel;
        FILE* f = std::fopen(p.c_str(), "r");
        if (f) {
            std::fclose(f);
            return p;
        }
    }
    return rel;  // 都没有：返回原路径，让 ime.load() 报打不开
}

// 终端网格：逐格画，宽字符跳过后一格；scroll>0 时把历史与当前屏拼成可见窗口。
void drawGrid(render::Renderer& r, const ui::FontBook& f, const ui::Geometry& g,
              term::TerminalParser& tp, int cursorRow, int cursorCol, bool cursorVisible,
              int scroll) {
    const int cellW = g.termCellW;
    const int cellH = g.termCellH;
    const int baselineOff = (cellH + f.term->ascent - f.term->descent) / 2;
    const int hist = tp.historySize();
    scroll = std::clamp(scroll, 0, hist);
    auto cellAt = [&](int row, int col) {
        const int virtualRow = hist - scroll + row;
        return virtualRow < hist ? tp.historyAt(virtualRow, col)
                                 : tp.cellAt(virtualRow - hist, col);
    };

    for (int row = 0; row < tp.rows(); ++row) {
        int y = g.contentY + g.termPadTop + row * cellH;
        // ① 先铺底色（宽字符两格一起铺，避免后一格的底色盖掉前一个字）
        for (int col = 0; col < tp.cols();) {
            term::Cell c = cellAt(row, col);
            int w = c.wide ? 2 : 1;
            if (c.bg != ui::col::kTerm) {
                int x = g.termX + g.termPadX + col * cellW;
                r.fill({x, y, cellW * w, cellH}, ui::c(c.bg));
            }
            col += w;
        }
        // ② 再画字
        for (int col = 0; col < tp.cols();) {
            term::Cell c = cellAt(row, col);
            int x = g.termX + g.termPadX + col * cellW;
            int w = c.wide ? 2 : 1;
            if (c.cp != ' ' && c.cp != 0) {
                char buf[8] = {0};
                int n = 0;
                uint32_t cp = c.cp;
                if (cp < 0x80) {
                    buf[n++] = char(cp);
                } else if (cp < 0x800) {
                    buf[n++] = char(0xC0 | (cp >> 6));
                    buf[n++] = char(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    buf[n++] = char(0xE0 | (cp >> 12));
                    buf[n++] = char(0x80 | ((cp >> 6) & 0x3F));
                    buf[n++] = char(0x80 | (cp & 0x3F));
                } else {
                    buf[n++] = char(0xF0 | (cp >> 18));
                    buf[n++] = char(0x80 | ((cp >> 12) & 0x3F));
                    buf[n++] = char(0x80 | ((cp >> 6) & 0x3F));
                    buf[n++] = char(0x80 | (cp & 0x3F));
                }
                r.textBaseline(x, y + baselineOff, std::string(buf, size_t(n)), f.term,
                               ui::c(c.fg));
            }
            if (c.underline) r.fill({x, y + cellH - 2, cellW * w, 1}, ui::c(c.fg));
            col += w;
        }
    }
    if (scroll == 0 && cursorVisible && cursorRow >= 0 && cursorRow < tp.rows() &&
        cursorCol >= 0 && cursorCol < tp.cols()) {
        int x = g.termX + g.termPadX + cursorCol * cellW;
        int y = g.contentY + g.termPadTop + cursorRow * cellH;
        term::Cell c = tp.cellAt(cursorRow, cursorCol);
        r.fill({x, y + cellH - 2, cellW, 2}, ui::c(c.fg));
    }

    if (scroll > 0) {
        const int trackY = g.contentY + g.termPadTop;
        const int trackH = tp.rows() * cellH;
        const int barW = std::max(1, int(std::lround(3.f * g.scale)));
        const int totalRows = hist + tp.rows();
        const int thumbH = std::max(barW, trackH * tp.rows() / std::max(totalRows, 1));
        const int thumbY = trackY + (trackH - thumbH) * (hist - scroll) / std::max(hist, 1);
        r.fill({g.termX + g.termW - barW, thumbY, barW, thumbH},
               disp::withAlpha(ui::c(ui::col::kBlue), 128));
        const std::string arrow = "↕";
        const int arrowW = r.textWidth(arrow, f.footer);
        r.textBaseline(g.termX + g.termW - barW - arrowW - 2, trackY + f.footer->ascent,
                       arrow, f.footer, ui::c(ui::col::kFaint));

        const int statusY = g.contentY + g.termPadTop + tp.rows() * cellH;
        const std::string status =
            "SCROLL " + std::to_string(scroll) + "/" + std::to_string(hist);
        r.textBaseline(g.termX + g.termPadX, statusY + baselineOff, status, f.term,
                       ui::c(ui::col::kDim));
    }
}

// overlay 模式要保留（= 看得见、点得到）的三块：顶栏 + 左右面板。XShape 挖洞和局部刷新
// 用的都是这一份几何，画在哪和能看见哪永远是同一块
std::vector<disp::Rect> overlayRects(const ui::Geometry& g) {
    return {{0, 0, g.screenW, g.topBarH},
            {0, g.contentY, g.panelW, g.contentH},
            {g.screenW - g.panelW, g.contentY, g.panelW, g.contentH}};
}

// 终端里敲 term/quit 就能建/关 tab：往 PATH 最前面塞两个脚本，子进程 execvp 直接敲到
void installPathShims() {
    const std::string dir = "/tmp/sit-bin-" + std::to_string(::getpid());
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        std::fprintf(stderr, "警告：建不了 %s，term/quit 命令不可用\n", dir.c_str());
        return;
    }
    const struct {
        const char* name;
        const char* body;
    } shims[] = {
        {"term", "#!/bin/sh\nprintf '\\033]1337;newtab=%s\\a' \"$*\"\n"},
        {"quit", "#!/bin/sh\nprintf '\\033]1337;quit\\a'\n"},
    };
    for (const auto& s : shims) {
        std::string path = dir + "/" + s.name;
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f || std::fputs(s.body, f) < 0 || std::fclose(f) != 0 ||
            ::chmod(path.c_str(), 0755) != 0)
            std::fprintf(stderr, "警告：%s 写不了，term/quit 命令不可用\n", path.c_str());
    }
    // shortcut: 退出不清理（/tmp 由系统清）；要回收就改成退出时 unlink 这两个文件
    const char* old = std::getenv("PATH");
    std::string p = dir + ":" + (old ? old : "/usr/local/bin:/usr/bin:/bin");
    ::setenv("PATH", p.c_str(), 1);
}

// 中文态下 ASCII 标点走全角，和拼音输入法的行为一致；英文态原样送出。
// 这样 NUM 层就不用再单独摆一份"，。；："——两个输入法状态各自负责自己的标点。
const char* cjkPunct(const std::string& s) {
    static const struct {
        const char* ascii;
        const char* cjk;
    } kMap[] = {
        {",", "，"}, {".", "。"}, {";", "；"}, {":", "："}, {"?", "？"}, {"!", "！"},
        {"(", "（"}, {")", "）"}, {"[", "【"}, {"]", "】"}, {"\\", "、"}, {"<", "《"},
        {">", "》"}, {"'", "‘"}, {"\"", "“"},
    };
    for (const auto& m : kMap)
        if (s == m.ascii) return m.cjk;
    return nullptr;
}

// 应答 fish 的设备属性查询（否则启动会卡住等超时）
// 只认 ESC [ c 和 ESC [ 0 c：以前写成「ESC [ 0」也算，于是每条 \x1b[0m 颜色复位
// 都会换来一次 DA 应答，再被行规程回显成屏上的 "62;22c"
void answerDA(term::Pty& pty, const char* buf, long n) {
    for (long i = 0; i + 2 < n; ++i) {
        if (buf[i] != 0x1b || buf[i + 1] != '[') continue;
        if (buf[i + 2] == 'c' || (buf[i + 2] == '0' && i + 3 < n && buf[i + 3] == 'c')) {
            const char* reply = "\x1b[?62;22c";
            pty.writeMaster(reply, std::strlen(reply));
            return;
        }
    }
}

}  // namespace

int run(disp::DisplayBackend* bd, const std::string& resDir, const std::string& outPath,
        const std::string& runCmd, const std::string& cwd, int tabs) {
    // ① 装配：config/ 下三个 Lua 一次读完（ui + keymap + engine），SIGUSR1 也走同一段
    auto loadProfile = [&](cfg::Profile& p) {
        return cfg::loadProfile((resDir + "/config").c_str(), p) ||
               cfg::loadProfile("config", p);  // 退回相对路径（从项目根运行）
    };
    cfg::Profile prof;
    if (!loadProfile(prof)) std::fprintf(stderr, "警告：config/*.lua 读不到，用内置默认值\n");
    cfg::Ui uic = prof.ui;
    cfg::KeyMap km = prof.keymap;
    cfg::Engine eng = prof.engine;
    // 速度曲线要从死区边缘起算才对得平滑（physics 不读 config，热重载处也要同步这一行）
    eng.physics.deadzone = eng.deadzone;
    // engine.log 目前只装配+打印；engine.sink / engine.ime.enabled 真实生效
    std::fprintf(stderr, "engine: sink=%s log=%s%s%s ngram=%d ime=%s rotate=%d\n", eng.sink.c_str(),
                 eng.logLevel.c_str(), eng.logPath.empty() ? "" : "@", eng.logPath.c_str(),
                 eng.ngram, eng.imeEnabled ? "on" : "off", eng.rotate);

    // 输出口：uinput = 当虚拟键盘打给系统；打不开就回落 pty（不抢键、不少一条警告）
    input::UinputKeyboard ukbd;
    if (eng.sink == "uinput" && !ukbd.open()) {
        std::fprintf(stderr,
                     "警告：/dev/uinput 打不开，回落到 pty（需要 root 或 input 组 / udev 规则）\n");
        eng.sink = "pty";
    }
    const bool useUinput = eng.sink == "uinput";
    if (useUinput)
        std::fprintf(stderr, "sink: uinput（打到前台程序；PTY 仅作监视器，按键不写进去）\n");

    ui::Geometry g;
    g.fromConfig(uic, ui::layout::kScale);
    bd->setCanvasSize(g.screenW, g.screenH);
    bd->setRotation(eng.rotate);  // engine.display.rotate（仅 fbdev 生效）

    // overlay：只当压在 openbox 上面的覆盖层，中间挖洞让给下面的 alacritty / chromium。
    // 形态只在启动时定：运行中换要重建窗口和 PTY，热重载那里只提示不切
    // SIT_OVERLAY=1 强制 overlay：试跑用，免得为了试一次去改 config（设备上的 git 工作区要保持干净才能 push）
    const bool overlayMode = uic.mode == "overlay" || std::getenv("SIT_OVERLAY") != nullptr;
    disp::X11Overlay overlay;
    if (overlayMode &&
        !overlay.init(static_cast<Display*>(bd->nativeDisplay()),
                      static_cast<Window>(bd->nativeWindow())))
        std::fprintf(stderr, "ui: mode=overlay 但拿不到 X 窗口（不是 X11 会话？），中间不会挖洞\n");
    // 窗口尺寸变了就得重算洞的位置：几何一变（启动/缩放/热重载）都走这里
    auto applyOverlayShape = [&] {
        if (overlayMode) {
            overlay.setAlwaysOnTop();  // 只发一次：WM 会记住；窗口重建才需要再发
            overlay.applyShape(overlayRects(g), g.screenW, g.screenH);
        }
    };
    applyOverlayShape();

    render::FontManager fm;
    if (!fm.init()) return 1;
    ui::FontBook fonts;
    if (!fonts.init(fm)) return 1;
    render::Renderer r;
    if (!r.init(bd, &fm)) return 1;

    // ② 终端：只认 TerminalParser 接口。一个 tab 一套 parser + pty，互相不影响
    struct Term {
        // 用 unique_ptr：vector 扩容时元素会搬，erase 时 Pty 析构自动 SIGHUP 子进程
        std::unique_ptr<term::TerminalParser> tp;
        std::unique_ptr<term::Pty> pty;
        std::string title;
        std::string pend;  // OSC 跨 read 的残余（见 term/osc_filter.h）
        int scroll = 0;    // 回看行数；0 = 跟随最新输出
    };
    std::vector<Term> terms;
    int active = 0;
    int tabSeq = 0;

    const char* shell = std::getenv("SHELL");
    std::vector<std::string> argv{shell && *shell ? shell : "/bin/bash"};
    const passwd* pw = ::getpwuid(::getuid());
    const std::string home = pw && pw->pw_dir ? pw->pw_dir : "";
    if (home.empty()) std::fprintf(stderr, "警告：按 uid 查不到 home，终端沿用当前 cwd\n");
    // 交互 shell 默认从登录用户 home 起；--run 保留调用方 cwd，--cwd 始终优先。
    const std::string termCwd = !cwd.empty() ? cwd : (runCmd.empty() ? home : "");
    if (!runCmd.empty()) {  // --run：直接 shell -c，输出干净（无欢迎信息干扰）
        argv.push_back("-c");
        argv.push_back(runCmd);
    }

    installPathShims();  // term/quit 命令：必须在第一个 fork 之前

    auto spawnTerm = [&]() -> bool {
        if (overlayMode) return false;  // overlay 模式没有终端：PTY 一律不 spawn
        // 防递归：--run 的脚本自己吐 newtab 也不会无限生 tab
        if (terms.size() >= 16) {
            std::fprintf(stderr, "tab: 已有 16 个，拒绝新建\n");
            return false;
        }
        std::unique_ptr<term::TerminalParser> tp(term::createParser("vterm"));
        if (!tp || !tp->init(g.termRows, g.termCols)) return false;
        tp->setHistoryLimit(eng.scrollback);  // engine.terminal.scrollback
        std::unique_ptr<term::Pty> pty(new term::Pty);
        if (!pty->spawn(argv, g.termCols, g.termRows, termCwd, home)) return false;
        terms.push_back(
            {std::move(tp), std::move(pty), "TERM " + std::to_string(++tabSeq), {}, 0});
        return true;
    };

    // 关第 i 个 tab；只剩一个时原地重启（像 xterm，窗口不空）
    auto closeTab = [&](int i) {
        if (i < 0 || i >= int(terms.size())) return;
        if (terms.size() > 1) {
            int a = tabAfterClose(active, int(terms.size()));
            if (i < active) a = active - 1;  // 关的是活动 tab 前面的：后面的全体前移一位
            terms.erase(terms.begin() + i);
            active = a;
            return;
        }
        std::string title = terms[0].title;
        Term old = std::move(terms[0]);
        terms.clear();
        if (!spawnTerm()) {  // 重开失败：把旧的塞回去，至少不崩
            terms.push_back(std::move(old));
            active = 0;
            return;
        }
        terms[0].title = title;  // 同一个 title 重新 spawn
        active = 0;
    };
    if (!overlayMode && !spawnTerm()) return 1;  // 第一个 tab 起不来就没得玩
    for (int i = 1; i < tabs; ++i)  // --tabs：无输入的环境下一帧就能看到多个 tab
        if (!spawnTerm()) break;

    // ③ 输入法：内置全拼（组字 + 候选 + n-gram 重排）
    ime::BuiltinIme ime;
    const std::string idx = resolve(resDir, "data/pinyin.idx");
    const std::string ng = resolve(resDir, "data/ngram.bin");
    if (!ime.load(idx, ng)) {  // 没词库就关掉输入法：否则字母全被吃掉（组字永远没有候选），只剩标点能上
        std::fprintf(stderr, "警告：打不开词库（%s），中文输入关闭；字母走直通\n", idx.c_str());
        eng.imeEnabled = false;
    }
    ime.setPageSize(eng.candPerPage);  // engine.ime.cand_per_page：IME 内部翻页跨度


    // 按键入口：
    // eng.imeEnabled = true  → 内置输入法（routeKey，组字/选词）
    // eng.imeEnabled = false → 直通 routeRaw（只转 Ctrl/Alt/Shift，完全不组字）
    const bool routeTrace = std::getenv("SIT_DEBUG") != nullptr;  // 排障：每个键被吃成什么
    auto route = [&, routeTrace](char ch, int mods) -> RouteResult {
        if ((mods & 1) && ch == ' ') ch = 0x00;  // 手柄 Ctrl+空格 = 切中英（实体键盘终端本来就给 0x00）
        RouteResult r = eng.imeEnabled ? routeKey(ime, ch, mods) : routeRaw(ch, mods);
        if (routeTrace)
            std::fprintf(stderr, "[route] ch=%02x zh=%d composing=%d consumed=%d bytes=%zu mods=%d\n",
                         (unsigned char)ch, int(ime.chinese), int(ime.composing()),
                         int(r.consumed), r.bytes.size(), mods);
        return r;
    };

    UInputMouse mouse;

    input::EvdevGamepad gamepad;
    gamepad.setStickParams(eng.deadzone, eng.hysteresisDeg);  // engine.input.*
    int pads = gamepad.open();
    for (const auto& nm : gamepad.names()) std::fprintf(stderr, "gamepad: %s\n", nm.c_str());
    if (pads == 0) std::fprintf(stderr, "gamepad: 没有可读的手柄\n");

    // 电源键/音量键不在手柄设备上，另开一份只读 fd（不开 EVIOCGRAB，logind 还要看它们）
    input::EvdevSysKeys syskeys;
    syskeys.open();
    for (const auto& nm : syskeys.names()) std::fprintf(stderr, "syskeys: %s\n", nm.c_str());

    // 触屏也归输入层：fbdev 与 SDL(X) 共用这一份（拖动翻页 / 轻点），后端不再管
    input::EvdevTouch evTouch;
    evTouch.setRotation(bd->displayRotation());
    evTouch.open();
    if (evTouch.count() > 0) std::fprintf(stderr, "touch: %s\n", evTouch.name().c_str());

    // 分侧震动：左右 Joy-Con 各自是一个 FF 设备，事件只打到对应那一边
    input::Rumble rumbleL, rumbleR;
    bool rumbleShared = false;  // 两侧解析到同一台设备：走 strong=左 / weak=右 的双通道模式
    // 开关震动设备：先按左右各找一台；找不到两台、或两台解析到同一节点（一体化手柄）→
    // 走双通道模式（strong=左 / weak=右）。重扫也走这里，避免把第二个 fd 又打开
    auto openRumble = [&](bool selfTest) {
        rumbleL.close();
        rumbleR.close();
        rumbleShared = false;
        rumbleL.setGain(eng.rumble.gainLeft);
        rumbleR.setGain(eng.rumble.gainRight);
        if (!rumbleL.open("Left Joy-Con")) std::fprintf(stderr, "rumble: 左手柄没有 FF_RUMBLE\n");
        if (!rumbleR.open("Right Joy-Con")) std::fprintf(stderr, "rumble: 右手柄没有 FF_RUMBLE\n");
        if (rumbleL.ok() && rumbleR.ok() && rumbleL.device() == rumbleR.device()) {
            // 一体化手柄：一个节点带两个马达，实测 strong=左手 weak=右手。
            // 关掉第二个 fd：省 FF 槽位、也不让两份 effect 抢同一颗马达
            std::fprintf(stderr, "rumble: 左右共用 %s，按 strong=左 / weak=右 分侧\n",
                         rumbleL.device().c_str());
            rumbleR.close();
            rumbleL.setGainLR(eng.rumble.gainLeft, eng.rumble.gainRight);  // strong/weak 各用各的
            rumbleShared = true;
        }
        // 排障：起来先打一发，日志里能看出 EVIOCSFF 到底成不成（摸不到震动就是硬件/驱动）
        if (selfTest && std::getenv("SIT_DEBUG")) {
            if (rumbleShared) {
                rumbleL.thunkLR(0.9f, 0.9f, 250);
            } else {
                rumbleL.thunk(0.9f, 0.9f, 250);
                rumbleR.thunk(0.9f, 0.9f, 250);
            }
        }
    };
    if (eng.rumble.enabled) openRumble(true);
    long long gpRescanAt = 0;      // 手柄没打开时下次重扫的时刻（ms）
    long long rumbleRescanAt = 0;  // 震动设备掉线时下次重开的时刻（ms）

    const bool answerDa = runCmd.empty();
    // 后台 tab 也得读：没人读的话 shell 写满 PTY 缓冲就卡死了
    // 每个 tab 读一块 → 应答 DA → 剥掉 term/quit/标题标记 → 喂各自的解析器；返回总字节数
    auto readInto = [&]() -> long {
        char b[8192];
        long total = 0;
        bool alive = false;
        std::vector<int> closing;          // 要关的 tab（shell 退出 + quit）
        std::vector<std::string> opening;  // 要新建的 tab 名
        std::vector<term::OscEvent> evs;
        for (int i = 0; i < int(terms.size()); ++i) {
            Term& t = terms[i];
            long got = t.pty->readMaster(b, sizeof b);
            if (got < 0) {  // 这个 tab 的 shell 已退出（-1 = EOF）
                // --run 时保持旧语义（死 tab 留着，全死就是退出信号），交互时关掉
                if (runCmd.empty()) closing.push_back(i);
                continue;
            }
            alive = true;
            if (got == 0) continue;
            if (answerDa) answerDA(*t.pty, b, got);  // --run 时对方不读 stdin，回复会被回显成垃圾
            evs.clear();
            std::string clean = term::oscFilter(t.pend, b, size_t(got), &evs);
            t.tp->feed(clean.data(), clean.size());
            for (const term::OscEvent& e : evs) {
                if (e.kind == term::OscEvent::Title) {
                    t.title = e.arg;
                } else if (e.kind == term::OscEvent::Quit) {
                    closing.push_back(i);
                } else if (runCmd.empty()) {  // --run 的脚本吐 newtab：忽略，不然自己生自己
                    opening.push_back(e.arg);
                }
            }
            total += got;
        }
        // 遍历中不动 terms：先按倒序关（erase 不影响前面的下标），再新建（放最后）
        std::sort(closing.begin(), closing.end());
        closing.erase(std::unique(closing.begin(), closing.end()), closing.end());
        for (auto it = closing.rbegin(); it != closing.rend(); ++it) closeTab(*it);
        for (const std::string& name : opening) {
            if (!spawnTerm()) break;
            if (!name.empty()) terms.back().title = name;
        }
        // --run 保持旧语义：全死光才当退出信号；交互时 tab 会重开，不因 shell 退出就整个退出
        return (!runCmd.empty() && !alive) ? -1 : total;
    };

    if (!overlayMode && !runCmd.empty()) {  // 抽干输出（等到静默）
        int quiet = 0;
        for (int i = 0; i < 200 && quiet < 8; ++i) {
            struct pollfd pfd{terms[0].pty->masterFd(), POLLIN, 0};
            ::poll(&pfd, 1, 16);
            quiet = (readInto() > 0) ? 0 : quiet + 1;
        }
    }

    if (!overlayMode && std::getenv("SIT_DEBUG")) {  // 命令跑完后再看一次网格
        for (int row = 0; row < 5 && row < terms[0].tp->rows(); ++row) {
            std::string line;
            for (int col = 0; col < 50 && col < terms[0].tp->cols(); ++col) {
                term::Cell cc = terms[0].tp->cellAt(row, col);
                line += (cc.cp >= 0x20 && cc.cp < 0x80) ? char(cc.cp) : '.';
            }
            std::fprintf(stderr, "[after %d] %s\n", row, line.c_str());
        }
    }

    std::signal(SIGHUP, onReload);
    std::signal(SIGUSR1, onReload);

    std::string pending;   // 待写 PTY
    int lastBtn = -1;      // 上一颗按下的键（双击锁定用）

    // 物理场（docs/PHYSICS.md）：两个摇杆各驱动一个指针。坐标是设计像素，与渲染缩放无关，
    // 所以键中心只在 ui.lua 变化（启动/热重载）时重算一次。
    // 每场 = 一手 5 列 × 4 行 = 20 键；零位 d / k（掌心位置，摇杆回中的落点最顺手）
    const app::physics::Field fieldL{0, 12};   // 左场：键 0..19，零位 d
    const app::physics::Field fieldR{20, 32};  // 右场：键 20..39，零位 k
    app::physics::Pointer ptrL, ptrR;
    int activeSide = 0;  // 0=左 1=右：L 上屏取哪一侧（最近推过、推得更狠的那根摇杆）
    int layerIdx = 0;    // 0=ALPHA 1=NUM 2=SYM（L3 循环）
    float keyCx[cfg::kKeyV2Count], keyCy[cfg::kKeyV2Count];
    // 摄像机：屏幕上那个「窗口中心」对准的世界坐标。
    // 吸附住某键时对准该键中心 —— 选中的键就钉死在面板正中，只有周围的键在动；
    // 没吸附时对准指针（否则自由移动时会拖影）。
    float camLx = 0, camLy = 0, camRx = 0, camRy = 0;
    // 摄像机 = 「按键画布」的位置，面板中心是固定的选中区：
    // 目标 = 吸附键中心（没吸附时 = 指针），所以画布最多只能走到让某个键居中。
    // 弹簧带一点欠阻尼：推过头会多走一点，然后自己弹回来。
    float camVx = 0, camVy = 0, camRVx = 0, camRVy = 0;
    float camHomeOffY = 0;  // 准心偏置：home 行相对键场中线的纵向差
    int prevSnapL = -2, prevSnapR = -2;  // 上一帧的吸附键（换键时触发"弹一下"）
    auto resetFields = [&] {
        const float half = float(uic.keySize) * 0.5f;  // ui.keys 是键帽左上角
        for (int i = 0; i < cfg::kKeyV2Count; ++i) {
            keyCx[i] = float(uic.keysV2[i].x) + half;
            keyCy[i] = float(uic.keysV2[i].y) + half;
        }
        ptrL = app::physics::Pointer{};
        ptrR = app::physics::Pointer{};
        ptrL.x = keyCx[fieldL.home];
        ptrL.y = keyCy[fieldL.home];
        ptrR.x = keyCx[fieldR.home];
        ptrR.y = keyCy[fieldR.home];
        camLx = ptrL.x;
        camLy = ptrL.y;
        camRx = ptrR.x;
        camRy = ptrR.y;
        // 摄像机 = 绝对跟随选中键 ⇒ 选中的键永远落在屏幕上同一个点（固定准心），
        // 准心纵向偏移 = 0：相机对准选中键本身，home 行正好落在准心上。
        // 原来取「home 行比键场中线低多少」（31px），那是 4 行窗口时代的算法——那时世界和窗口
        // 一样高，相机对世界中线才齐平。换成 3 行窗口（172 高）后，抬这 31px 会把底部那一行
        // （zxcvb / nm,./）推出窗口，被窗口下沿硬切一半。
        camHomeOffY = 0.f;
        camVx = camVy = camRVx = camRVy = 0.f;
    };
    resetFields();

    // ui.lua 里没写 keys 就是配置缺失：键区会是空的，启动时说一句（没有别的面板可退回了）。
    // 判最后一颗而不是第一颗：第一颗的局部坐标本来就是 (0,0)，那是合法值
    if (!uic.keysV2[cfg::kKeyV2Count - 1].x && !uic.keysV2[cfg::kKeyV2Count - 1].y)
        std::fprintf(stderr, "警告：ui.lua 没有 keys，左右键区是空的\n");

    ui::AppState st;
    st.imeName = eng.imeEnabled && ime.chinese ? "拼音" : "英文";
    st.overlay = overlayMode;
    bool uiDirty = false;  // 这一帧有没有动过屏幕：静止就不重画靠它

    // overlay：顶栏的窗口列表（_NET_CLIENT_LIST 的顺序 = 任务栏顺序）。每 500ms 重读一次：
    // 新开/关的窗口、标题、谁拿到焦点都会变；返回 true = 内容变了，要重画顶栏
    auto refreshWindows = [&]() -> bool {
        std::vector<ui::SysWin> wins;
        for (const disp::X11Overlay::Win& w : overlay.listWindows()) wins.push_back({w.id, w.title});
        const unsigned long act = overlay.activeWindow();
        bool diff = act != st.activeWin || wins.size() != st.windows.size();
        for (size_t i = 0; !diff && i < wins.size(); ++i)
            diff = wins[i].id != st.windows[i].id || wins[i].title != st.windows[i].title;
        st.windows = std::move(wins);
        st.activeWin = act;
        return diff;
    };
    long long winListAt = 0;  // 上次重读窗口列表的时刻（ms）
    if (overlayMode) refreshWindows();


    // 动作分发：手柄/面板都走这里（映射来自 keymap.lua）
    auto msNow = [] {
        return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    long long lastPress[cfg::kKeyCount] = {0};
    // 顶栏：时钟 + 电量。每 2s 重读一次（每帧 open sysfs 太浪费）。
    // 电池路径＝Switch/switchroot 的 max17042 → /sys/class/power_supply/battery；换机器改这里。
    long long topBarAt = 0;
    auto refreshTopBar = [&] {
        const long long now = msNow();
        if (now - topBarAt < 2000) return;
        topBarAt = now;
        if (const std::time_t t = std::time(nullptr)) {
            std::tm tm{};
            localtime_r(&t, &tm);
            char hm[8] = "--:--";
            std::strftime(hm, sizeof hm, "%H:%M", &tm);
            st.clock = hm;
        }
        const char* kBat = "/sys/class/power_supply/battery";
        int pct = -1;
        if (std::FILE* f = std::fopen((std::string(kBat) + "/capacity").c_str(), "r")) {
            if (std::fscanf(f, "%d", &pct) != 1) pct = -1;
            std::fclose(f);
        }
        if (pct >= 0) {
            st.battery = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
            char stt[32] = "";
            if (std::FILE* f = std::fopen((std::string(kBat) + "/status").c_str(), "r")) {
                if (std::fgets(stt, sizeof stt, f)) stt[std::strcspn(stt, "\r\n")] = 0;
                std::fclose(f);
            }
            st.charging = std::strcmp(stt, "Charging") == 0 || std::strcmp(stt, "Full") == 0;
        }
    };

    // Tab 前缀窗口：DPAD_UP 按一下开这么久，窗口里 R/ZR 都是切 tab，每按一次顺延
    // （所以按着 Tab 连按 R/ZR 能一直翻）。做其它任何动作立刻关窗口。
    // 设备只报按下不报松开，用时间窗口近似「按住 Tab」。
    constexpr long long kTabArmMs = 1200;
    long long tabArmedAt = 0;
    // 当前生效的修饰键 = 一次性(sticky) | 锁定(locks) | 物理按住(holding)。
    // 顺带记下"按住期间被用过"——松手时就不该再留一个一次性修饰，否则 Ctrl 会永久粘着
    auto modMask = [&]() {
        const int m = bd->sticky | bd->locks | bd->holding;
        bd->heldUsed |= (m & bd->holding);
        return m;
    };

    auto doAction = [&](cfg::Action a) {
        if (a != cfg::kTab && a != cfg::kCursorLeft && a != cfg::kCursorRight) tabArmedAt = 0;
        switch (a) {
            case cfg::kMagic: {  // L（LB）：只上屏吸附住的键，hover 永不上屏
                const app::physics::Pointer& p = activeSide ? ptrR : ptrL;
                if (p.snapped < 0) {  // 指针在两键之间的缝里：没得可上，也没提示（SIT_DEBUG 才说）
                    if (routeTrace) std::fprintf(stderr, "[magic] 丢弃：snapped=-1，指针在键缝里\n");
                    break;
                }
                // Shift（一次性或锁定）优先取上档字符；该键没有上档就退回下档
                const bool sh = (modMask() & 2) != 0;
                const std::string& ch = (sh && !uic.shifts[layerIdx][p.snapped].empty())
                                            ? uic.shifts[layerIdx][p.snapped]
                                            : uic.layers[layerIdx][p.snapped];
                if (ch.empty()) {
                    if (routeTrace) std::fprintf(stderr, "[magic] 丢弃：键 %d 在当前层是空格\n",
                                                 p.snapped);
                    break;
                }
                // 中文态：ASCII 标点先换成全角（。，；：？！（）【】、 等），英文态原样
                std::string out = ch;
                if (ime.chinese) {
                    if (const char* cj = cjkPunct(ch)) out = cj;
                }
                if (out.size() == 1 && (unsigned char)out[0] < 0x80) {
                    pending += route(out[0], modMask()).bytes;
                } else {  // 多字节（中文标点）：先提交组字串，再把整串送出去
                    if (ime.composing()) pending += ime.commit(-1);
                    pending += out;
                }
                break;
            }
            case cfg::kSpace: pending += route(' ', modMask()).bytes; break;
            case cfg::kTab:
                pending += route('\t', modMask()).bytes;
                tabArmedAt = msNow();  // 窗口内的 R/ZR = 上一个/下一个 tab
                break;
            case cfg::kEnter: pending += route('\r', modMask()).bytes; break;
            case cfg::kBackspace: pending += route(0x7f, modMask()).bytes; break;
            case cfg::kDelete: pending += "\x1b[3~"; break;
            case cfg::kEscape:
                pending += route(0x1b, modMask()).bytes;
                break;
            case cfg::kModCtrl: bd->holding |= 1; bd->sticky ^= 1; break;
            case cfg::kModAlt: bd->holding |= 4; bd->sticky ^= 4; break;
            case cfg::kModShift: bd->holding |= 2; bd->sticky ^= 2; break;
            case cfg::kLockCtrl: bd->locks ^= 1; break;
            case cfg::kLockAlt: bd->locks ^= 4; break;
            case cfg::kLockShift: bd->locks ^= 2; break;
            case cfg::kMouseLeft: mouse.click(BTN_LEFT); break;    // 只点不移动
            case cfg::kMouseRight: mouse.click(BTN_RIGHT); break;
            // 多 tab：只动 terms 里的下标，不产生任何字节
            case cfg::kNextTab: active = tabStep(active, int(terms.size()), 1); break;
            case cfg::kPrevTab: active = tabStep(active, int(terms.size()), -1); break;
            case cfg::kNewTab: spawnTerm(); break;
            case cfg::kCloseTab: closeTab(active); break;
            case cfg::kLayerNext: layerIdx = (layerIdx + 1) % cfg::kLayerCount; break;
            case cfg::kOnboard:  // 右手柄 +：KDE 抢走手柄/自己打不出字时，叫 onboard 来打
                spawnDetached("onboard");
                break;
            // 右手肩键，优先级从上到下：
            // ① 组字出候选 → 在候选条里移动选中项（空格上屏）
            // ② Tab 窗口内（DPAD_UP 后 1.2s，按一次顺延）且有 ≥2 个 tab → 上一个/下一个，循环
            // ③ 带 Shift（单击一次 / 双击锁定）→ 光标上/下
            // ④ 其余 → 光标左/右
            case cfg::kCursorLeft:
                if (ime.composing() && !ime.cands().empty()) ime.selStep(-1);
                else if (terms.size() >= 2 && msNow() - tabArmedAt < kTabArmMs) {
                    tabArmedAt = msNow();  // 顺延，按住 Tab 连按能一直翻
                    active = tabStep(active, int(terms.size()), -1);
                } else pending += (modMask() & 2) ? "\x1b[A" : "\x1b[D";
                break;
            case cfg::kCursorRight:
                if (ime.composing() && !ime.cands().empty()) ime.selStep(+1);
                else if (terms.size() >= 2 && msNow() - tabArmedAt < kTabArmMs) {
                    tabArmedAt = msNow();  // 顺延
                    active = tabStep(active, int(terms.size()), 1);
                } else pending += (modMask() & 2) ? "\x1b[B" : "\x1b[C";
                break;
            // 面板/触摸独有动作（kIme..kLayer）：手柄路径不会产生，只有 --legacy 用
            case cfg::kIme:
            case cfg::kCopy:
            case cfg::kPaste:
            case cfg::kSelLeft:
            case cfg::kSelRight:
            case cfg::kLayer:
            case cfg::kNone: break;
        }
        if (!terms.empty()) terms[active].tp->markDirty();
    };

    // 一次性修饰（sticky）：用掉一颗真按键就清，不然 Ctrl 会永久粘着（空格全变成切中英）
    auto clearSticky = [&](cfg::Action a) {
        switch (a) {
            case cfg::kModCtrl:
            case cfg::kModAlt:
            case cfg::kModShift:
            case cfg::kLockCtrl:
            case cfg::kLockAlt:
            case cfg::kLockShift:
            case cfg::kNone: return;
            default: bd->sticky = 0; return;
        }
    };

    // 抬起：按住期间没被用过 → 保留一次性修饰（单击 = 下一颗键带上它）；
    // 用过 → 清掉，避免"按住 Shift 再按 R/ZR"之后 Shift 永久粘着
    auto onPadRelease = [&](int btn) {
        if (btn < 0 || btn >= cfg::kKeyCount) return;
        int bit = 0;
        switch (km.single[btn]) {
            case cfg::kModCtrl: bit = 1; break;
            case cfg::kModShift: bit = 2; break;
            case cfg::kModAlt: bit = 4; break;
            default: break;
        }
        if (bit == 0) return;
        bd->holding &= ~bit;
        if (bd->heldUsed & bit) bd->sticky &= ~bit;
        bd->heldUsed &= ~bit;
    };

    auto onPadButton = [&](int btn) {
        if (btn < 0 || btn >= cfg::kKeyCount) return;
        uiDirty = true;
        long long now = msNow();
        st.pressedKey = btn;  // 提示条闪一下（150ms）
        st.pressedKeyMs = now;
        // overlay：Alt（按住或锁定）时 R/ZR 合成 Alt+↑ / Alt+↓ 打给系统，openbox 的 A-Up / A-Down
        // 就这么切窗口。为什么不是 Tab+↑：X 里 Tab 不是修饰键，openbox 绑不出那个组合，Alt 才是。
        // 这里现算修饰态（和 st.modAlt 同一套 locks|holding|sticky，但不晚一帧），Alt 优先于 Shift；
        // 没按 Alt 时 R/ZR 原样走 keymap（光标左/右，带 Shift 时上/下）
        const int modsLive = bd->locks | bd->holding | bd->sticky;
        if (overlayMode && (modsLive & 4) && (btn == cfg::kRB || btn == cfg::kRT)) {
            if (btn == cfg::kRB) ukbd.tapAltUp();
            else ukbd.tapAltDown();
            return;
        }
        cfg::Action act = km.single[btn];
        // 同一键窗口内按第二下 = 双击动作（DPAD 锁定修饰用）
        if (km.dbl[btn] && btn == lastBtn && now - lastPress[btn] < eng.doubleTapMs)
            act = km.dbl[btn];
        lastPress[btn] = now;
        lastBtn = btn;
        if (routeTrace)  // 收到没有？收成了什么动作？没收到的键这里一行都不会有
            std::fprintf(stderr, "[pad] btn=%d act=%s\n", btn, cfg::actionName(act));
        if (act != cfg::kNone) {
            doAction(act);
            clearSticky(act);
        }
    };
    long long lastBlink = 0;
    bool cursorOn = true;
    bool running = true;
    bool prevAnimating = false;  // 上一帧在动？用来补画动画收尾那一次
    int lastActiveSeen = -1;     // 上一帧画的是哪个 tab：切换要整屏重画
    bool fullNext = false;       // 布局/尺寸/配置变了：下一帧必须整屏重画
    bool lastComposing = false;  // 上一帧画过组字/候选行：退出组字要把它擦掉
    int lastScrollSeen = 0;      // 上一帧终端是不是在看历史：退出回看要擦掉滚动条
    std::string lastBarSig;      // 上一帧顶栏/底栏/tab 条的内容签名：没变就不重画

    while (running) {
        std::string keys;
        std::vector<disp::ClickEvent> clicks;
        running = bd->pump(keys, clicks);
        // 触屏事件每帧取一次：clicks 追加轻点，bd->wheel 累加翻页行数（后面统一消费并清零）
        evTouch.pump(clicks, bd->wheel, g.screenW, g.screenH);
        if (bd->uiDirty) {  // 窗口尺寸变了：重排 + 重建字体
            bd->uiDirty = 0;
            float want = bd->scaleHint(ui::layout::kDesignW, ui::layout::kDesignH);
            if (std::fabs(want - ui::layout::kScale) > 0.06f) {
                ui::layout::setScale(want);
                g.fromConfig(uic, ui::layout::kScale);
                bd->setCanvasSize(g.screenW, g.screenH);
                bd->setRotation(eng.rotate);
                applyOverlayShape();  // 窗口尺寸变了：洞跟着重算
                fonts.init(fm);
                fullNext = true;
                for (Term& t : terms) {  // 每个 tab 的网格和 PTY 都得改尺寸
                    t.tp->resize(g.termRows, g.termCols);
                    t.pty->resize(g.termCols, g.termRows);
                    t.tp->markDirty();
                }
            }
        }
        if (gReload || bd->reloadReq) {  // kill -USR1 / SIGHUP，或窗口内 Ctrl+Shift+R
            gReload = 0;
            bd->reloadReq = 0;
            cfg::Profile p;
            if (!loadProfile(p)) {
                std::fprintf(stderr, "ui: 重载失败（config/*.lua 读不到），保持旧配置\n");
            } else {
                uic = p.ui;      // uic/km/eng 都是可重新赋值的局部变量，lambda 按引用捕获
                km = p.keymap;
                eng = p.engine;
                eng.physics.deadzone = eng.deadzone;  // 同上：曲线起点 = 死区边缘
                eng.sink = useUinput ? "uinput" : "pty";  // 输出口开机时定下，热重载不换设备
                if ((p.ui.mode == "overlay") != overlayMode)  // 形态只在启动时定
                    std::fprintf(stderr, "ui: ui.mode 只在启动时生效，重启后换形态\n");
                g.fromConfig(uic, ui::layout::kScale);
                bd->setCanvasSize(g.screenW, g.screenH);
                bd->setRotation(eng.rotate);
                applyOverlayShape();
                evTouch.setRotation(bd->displayRotation());
                fonts.init(fm);
                fullNext = true;  // 布局可能变了：整屏重画一次，别拿旧脏区套新几何
                for (Term& t : terms) {
                    t.tp->resize(g.termRows, g.termCols);
                    t.tp->setHistoryLimit(eng.scrollback);
                    t.pty->resize(g.termCols, g.termRows);
                    t.tp->markDirty();
                }
                // 瞬态状态复位：组字/候选/上一颗键
                ime.setPageSize(eng.candPerPage);  // 翻页跨度跟着新配置走（组字缓冲不丢，页号夹住）
                st.compose.clear();
                st.cands.clear();
                st.candPage = 0;
                lastBtn = -1;
                resetFields();  // 键中心跟着新 ui.lua 走，指针回零位
                activeSide = 0;
                layerIdx = 0;
                std::fprintf(stderr,
                             "ui: 重载 config/{ui,keymap,engine}.lua → %dx%d "
                             "scrollback=%d cand_per_page=%d double_tap=%dms\n",
                             g.screenW, g.screenH, eng.scrollback, eng.candPerPage,
                             eng.doubleTapMs);
            }
        }

        // ④ 输入路由：行为都在 app::routeKey / routeRaw 里（可单测）
        // overlay 模式没有终端：键盘字节没人收，这层路由整段跳过
        if (!overlayMode) {
            for (char ch : keys) {
                RouteResult rr = route(ch, modMask());
                pending += rr.bytes;
                if (bd->sticky) bd->sticky = 0;  // 一次性修饰：这颗键用过就清
            }
        }
        if (!pending.empty()) {
            if (useUinput)
                ukbd.typeBytes(pending);  // 打给系统；PTY 只收 shell 输出，不再收这些键
            else if (!terms.empty())
                terms[active].pty->writeMaster(pending.data(), pending.size());
            pending.clear();  // overlay 模式没有输出口：键区产出的字节直接丢
        }

        // ⑤ 读终端输出（overlay 模式没起 PTY）
        long n = overlayMode ? 0 : readInto();
        // 手柄没打开就每 2s 重扫：Joy-Con 走串口握手，开机后几十秒才出设备，启动时扫不到这里补上
        // 电源键/音量键设备也复用这个时机（它们掉线同样要重开，但没必要多一个定时器）
        if ((gamepad.count() == 0 || gamepad.lost() || syskeys.count() == 0 || syskeys.lost() ||
             evTouch.count() == 0 || evTouch.lost()) &&
            msNow() - gpRescanAt >= 2000) {
            gpRescanAt = msNow();
            if (gamepad.count() == 0 || gamepad.lost()) {
                if (gamepad.open() > 0)
                    for (const auto& nm : gamepad.names())
                        std::fprintf(stderr, "gamepad: (重扫) %s\n", nm.c_str());
            }
            if (syskeys.count() == 0 || syskeys.lost()) {
                if (syskeys.open() > 0)
                    for (const auto& nm : syskeys.names())
                        std::fprintf(stderr, "syskeys: (重扫) %s\n", nm.c_str());
            }
            if ((evTouch.count() == 0 || evTouch.lost()) && evTouch.open() > 0)
                std::fprintf(stderr, "touch: (重扫) %s\n", evTouch.name().c_str());
        }
        // Joy-Con 重连会重建 event6/event7，旧 fd 就废了：每 2s 试着补开
        // 共享模式只认 rumbleL 是否活着（rumbleR 是故意关掉的）
        if (eng.rumble.enabled && (!rumbleL.ok() || (!rumbleShared && !rumbleR.ok())) &&
            msNow() - rumbleRescanAt >= 2000) {
            rumbleRescanAt = msNow();
            openRumble(false);
        }
        gamepad.pollPad({onPadButton, [](int) {}, [](int) {}, onPadRelease});
        // 系统键走同一条 keymap 路径：单击/双击/锁定/magic 都自动生效（默认没绑 = 按了不动）
        syskeys.poll([&](int key, bool pressed) {
            int btn;
            switch (key) {
                case input::kSysVolDown: btn = input::kPadVolDown; break;
                case input::kSysVolUp: btn = input::kPadVolUp; break;
                case input::kSysPower: btn = input::kPadPower; break;
                default: return;
            }
            if (pressed)
                onPadButton(btn);
            else
                onPadRelease(btn);
        });

        // 摇杆向量 → 引力场。dt 写死在 physics.cpp（1/60），所以这里每帧步进一次就够。
        {
            float lx = 0, ly = 0, rx = 0, ry = 0;
            gamepad.stickVec(false, lx, ly);
            gamepad.stickVec(true, rx, ry);
            // 死区必须在进物理场之前滤：摇杆松手后的读数通常不是 0（轴中点 ≠ 真实回中位，还有漂移），
            // 不滤的话推杆量永远大于 idle 阈值 → 松手也不回零位（PHYSICS §2.7）
            app::physics::applyDeadzone(lx, ly, eng.deadzone);
            app::physics::applyDeadzone(rx, ry, eng.deadzone);
            const float lm = std::sqrt(lx * lx + ly * ly), rm = std::sqrt(rx * rx + ry * ry);
            if (std::fmax(lm, rm) > 0.15f) activeSide = lm >= rm ? 0 : 1;  // 死区外才换活跃侧
            // 本层哪些键有字符：空的（切到 NUM 层时大量空格）不参与吸附/引力，
            // 否则摇杆会吸在一堆看不见的空白键上
            bool validL[cfg::kKeyV2Count / 2], validR[cfg::kKeyV2Count / 2];
            for (int i = 0; i < cfg::kKeyV2Count / 2; ++i) {
                validL[i] = !uic.layers[layerIdx][i].empty();
                validR[i] = !uic.layers[layerIdx][i + cfg::kKeyV2Count / 2].empty();
            }
            // 吸附事件 → 震动（"咚"）。分侧计：左手的咚只震左手柄
            int snapsL = 0, snapsR = 0;
            auto onPhysL = [&snapsL](const app::physics::PhysEventData& e) {
                if (e.kind == app::physics::PhysEvent::Snap) ++snapsL;
            };
            auto onPhysR = [&snapsR](const app::physics::PhysEventData& e) {
                if (e.kind == app::physics::PhysEvent::Snap) ++snapsR;
            };
            app::physics::step(ptrL, fieldL, keyCx, keyCy, lx, ly, eng.physics, onPhysL, validL);
            app::physics::step(ptrR, fieldR, keyCx, keyCy, rx, ry, eng.physics, onPhysR, validR);

            // 震动：底噪跟指针速度走（有"在滚"的感觉），吸附那一下盖一个短促的"咚"。
            // 但只在摇杆真的被推着时震：松手后的惯性滑行和自动回中（home_pull）是系统自己
            // 在动，不该有反馈，否则手离开摇杆了它还一直在嗡。
            {
                const float ref = eng.rumble.moveSpeedRef > 1.f ? eng.rumble.moveSpeedRef : 500.f;
                const float vl = std::sqrt(ptrL.vx * ptrL.vx + ptrL.vy * ptrL.vy);
                const float vr = std::sqrt(ptrR.vx * ptrR.vx + ptrR.vy * ptrR.vy);
                // 每侧独立，而且**只有那根摇杆真的被推着才震**：
                // 松手后的惯性滑行、0.3s 后的 home_pull 自动回中，都是系统自己在动，
                // 不该有反馈——否则手离开摇杆了，回中路过键还会咚咚咚（2026-11-04 回归过一次）
                const bool pushedL = lm > 0.f, pushedR = rm > 0.f;
                const float magL = pushedL ? std::fmin(vl / ref, 1.f) * eng.rumble.moveMax : 0.f;
                const float magR = pushedR ? std::fmin(vr / ref, 1.f) * eng.rumble.moveMax : 0.f;
                if (rumbleShared) {  // 一体化手柄：strong=左、weak=右，一个 effect 管两侧
                    rumbleL.setMoveLR(magL, magR, eng.rumble.rateHz, eng.rumble.moveSmoothMs);
                    const bool sl = snapsL > 0 && pushedL, sr = snapsR > 0 && pushedR;
                    if (sl || sr)  // 哪一侧换键就只震哪一侧
                        rumbleL.thunkLR(sl ? eng.rumble.snapStrong : 0.f,
                                        sr ? eng.rumble.snapStrong : 0.f, eng.rumble.snapMs);
                } else {
                    rumbleL.setMove(magL, eng.rumble.rateHz, eng.rumble.moveSmoothMs);
                    rumbleR.setMove(magR, eng.rumble.rateHz, eng.rumble.moveSmoothMs);
                    if (snapsL > 0 && pushedL)
                        rumbleL.thunk(eng.rumble.snapStrong, eng.rumble.snapWeak, eng.rumble.snapMs);
                    if (snapsR > 0 && pushedR)
                        rumbleR.thunk(eng.rumble.snapStrong, eng.rumble.snapWeak, eng.rumble.snapMs);
                }
            }
        }

        // 滚轮、Shift+PgUp/PgDn 与触摸滑动都按“行”累计：正数向上回看。
        if (bd->wheel != 0) {
            if (std::getenv("SIT_DEBUG")) std::fprintf(stderr, "[touch] wheel=%d\n", bd->wheel);
            // overlay 模式没有终端：洞里的滚动直接进下面的窗口，这里只清累计量
            if (!terms.empty()) {
                Term& t = terms[active];
                const int w = bd->wheel;
                bd->wheel = 0;
                if (t.tp->altScreen()) {
                    // 备用屏（vim / less / pi 这类全屏 TUI）没有 scrollback，本地滚就是滚空气：
                    // 得把滚轮转发给里面的程序。开了鼠标上报就发 SGR 滚轮（xterm 同款），
                    // 没开就退成方向键——这是各终端模拟器的通用兜底
                    const int n = std::min(std::abs(w), 10);  // 一帧最多 10 下，别糊屏
                    std::string seq;
                    char buf[40];
                    for (int i = 0; i < n; ++i) {
                        if (t.tp->mouseReports() && t.tp->mouseSgr()) {
                            // 指针位置我们用终端中心（没有真实指针坐标，程序只关心方向）
                            std::snprintf(buf, sizeof buf, "\x1b[<%d;%d;%dM", w > 0 ? 64 : 65,
                                          t.tp->cols() / 2 + 1, t.tp->rows() / 2 + 1);
                            seq += buf;
                        } else {
                            seq += w > 0 ? "\x1b[A" : "\x1b[B";
                        }
                    }
                    if (!seq.empty()) t.pty->writeMaster(seq.data(), seq.size());
                } else {
                    t.scroll = std::clamp(t.scroll + w, 0, t.tp->historySize());
                    t.tp->markDirty();
                }
            } else {
                bd->wheel = 0;  // 没有终端：滚轮事件没人接手，清掉就行
            }
        }

        // 鼠标：只处理 tab 条（点「+」新建，点已有 tab 切过去）；键区暂时不接鼠标
        const long long nowMs = msNow();
        if (st.pressedKey >= 0 && nowMs - st.pressedKeyMs >= kFlashMs) st.pressedKey = -1;
        for (const disp::ClickEvent& ck : clicks) {
            // 排障：触屏坐标换算错位时一眼能看出来（SIT_DEBUG 才打）
            if (std::getenv("SIT_DEBUG"))
                std::fprintf(stderr, "[touch] click fb=(%d,%d)\n", ck.x, ck.y);
            int tab = ui::tabAt(g, uic, int(terms.size()), ck.x, ck.y);
            if (tab < 0) continue;
            if (tab == int(terms.size())) spawnTerm();
            else active = tab;
        }

        // ⑥ 画
        int cr = 0, cc = 0;
        bool cv = true;
        if (!terms.empty()) terms[active].tp->cursor(cr, cc, cv);
        if (long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
            now - lastBlink > 500) {
            lastBlink = now;
            cursorOn = !cursorOn;
            // 程序自己把光标藏了（pi 这类全屏 TUI）就不用闪，也别为此重画；
            // 光标只动一行，别把整屏标脏
            if (cv && !terms.empty()) terms[active].tp->markRowDirty(cr);
        }

        const std::string clockBefore = st.clock;
        const int batteryBefore = st.battery;
        refreshTopBar();
        if (st.clock != clockBefore || st.battery != batteryBefore) uiDirty = true;
        // overlay：系统窗口列表每 500ms 重读一次（新开/关的窗口、标题、谁拿到焦点都会变）
        if (overlayMode && nowMs - winListAt >= 500) {
            winListAt = nowMs;
            uiDirty = refreshWindows() || uiDirty;
        }
        // 底栏语言/输入法每帧从 ime 现算：按键路径（键盘 keys 循环、手柄 doAction）都会改
        // ime.chinese，但只有前者的 RouteResult 带了 langChanged，手柄那条路以前永远不更新底栏
        st.imeName = eng.imeEnabled && ime.chinese ? "拼音" : "英文";
        st.layer = layerIdx;  // L3 循环，不再跟 Shift 锁定走
        st.layerName = kLayerNames[layerIdx];
        // 换键那一下的弹跳：换键瞬间把脉冲拉到 1，150ms 内衰减到 0（渲染侧拿它放大键块）。
        // 尺寸最后会落回静止值，这就是"弹簧"的观感来源。
        {
            constexpr float kPulseSec = 0.15f;
            const float dec = (1.f / 60.f) / kPulseSec;
            st.snapPulseL = std::fmax(0.f, st.snapPulseL - dec);
            st.snapPulseR = std::fmax(0.f, st.snapPulseR - dec);
            if (ptrL.snapped != prevSnapL) {
                if (ptrL.snapped >= 0) st.snapPulseL = 1.f;
                prevSnapL = ptrL.snapped;
            }
            if (ptrR.snapped != prevSnapR) {
                if (ptrR.snapped >= 0) st.snapPulseR = 1.f;
                prevSnapR = ptrR.snapped;
            }
        }
        st.snapLeft = ptrL.snapped;
        st.snapRight = ptrR.snapped;
        st.hoverLeft = ptrL.hovered;
        st.hoverRight = ptrR.hovered;
        // 摄像机（按键画布）跟随：目标 = 吸附键中心，没吸附时 = 指针（都夹在键中心范围内，
        // 所以画布不会滚到没有键的地方）。用欠阻尼弹簧 ⇒ 允许被推过头一点，再自己弹回来。
        {
            constexpr float kDt = 1.f / 60.f;
            constexpr float kOmega = 34.f;   // 自然频率：约 0.15s 到位
            constexpr float kZeta = 0.45f;   // 阻尼比 <1：过冲约 18%
            // 视差权重：画布目标 = 吸附键中心 + 这个系数 × (指针 - 键心)。
            // 光对准键心的话，画布是"一档一档"整格跳的（微小推杆完全不动），看着重；
            // 掺一点指针就能连续跟手。指针被引力收进键心后这一项自然归零，
            // 所以选中的键最终还是正躺在中间。
            constexpr float kParallax = 0.30f;
            const float k = kOmega * kOmega, cDamp = 2.f * kZeta * kOmega;
            // 相机不夹取：选中键永远钉在准心上（"块固定"），边缘该留白就留白。
            // 弹性只靠弹簧本身（ζ=0.45 → 过冲 18%），换键时那一下"吸"就是它。
            auto camStep = [&](float& pos, float& vel, float target) {
                const float a = k * (target - pos) - cDamp * vel;
                vel += a * kDt;
                pos += vel * kDt;
            };

            // 稀疏层（这一层用到的格子装得进键窗）→ 相机对准整块中心，不对准选中键。
            // 否则块占 3 列时选中边列，窗口一挪必有 1 列被挤出屏幕（NUM 层"右边看不见"就是这么来的）；
            // 只占 2 列时又总有一列空白。装不下（ALPHA 满 5 列）就还是跟选中键。
            auto blockCenter = [&](int base, float& bx, float& by) -> bool {
                int c0 = 9, c1 = -1, r0 = 9, r1 = -1;
                for (int k = 0; k < 20; ++k) {
                    if (uic.layers[st.layer][base + k].empty()) continue;
                    const int col = k % 5, row = k / 5;
                    c0 = std::min(c0, col);
                    c1 = std::max(c1, col);
                    r0 = std::min(r0, row);
                    r1 = std::max(r1, row);
                }
                if (c1 < 0) return false;
                if (c1 - c0 + 1 > uic.keyWindowCols || r1 - r0 + 1 > uic.keyWindowRows) return false;
                const int i0 = base + r0 * 5 + c0, i1 = base + r1 * 5 + c1;
                bx = (keyCx[i0] + keyCx[i1]) * 0.5f;
                by = (keyCy[i0] + keyCy[i1]) * 0.5f;
                return true;
            };
            // 目标 = 选中键中心（未吸附时 = 最近的高亮键，绝不对准空白）
            auto camTarget = [&](const app::physics::Pointer& p, int base, float& tx, float& ty) {
                float bx, by;
                if (blockCenter(base, bx, by)) {
                    tx = bx;
                    ty = by - camHomeOffY;
                    return;
                }
                if (p.snapped < 0) {
                    // 没吸附：贴最近的高亮键；连高亮都没有（指针在空槽里）就原地不动，
                    // 绝不把画布对准空白（"吸住空气"就是这么来的）
                    const int h = p.hovered;
                    tx = h >= 0 ? keyCx[h] : camLx;
                    ty = (h >= 0 ? keyCy[h] : camLy) - camHomeOffY;
                } else {
                    const float kx = keyCx[p.snapped], ky = keyCy[p.snapped];
                    tx = kx + (p.x - kx) * kParallax;
                    ty = ky + (p.y - ky) * kParallax - camHomeOffY;
                }
            };
            float tlx, tly, trx, try_;
            camTarget(ptrL, 0, tlx, tly);
            camTarget(ptrR, 20, trx, try_);
            camStep(camLx, camVx, tlx);
            camStep(camLy, camVy, tly);
            camStep(camRx, camRVx, trx);
            camStep(camRy, camRVy, try_);
        }
        st.ptrLeftX = camLx;
        st.ptrLeftY = camLy;
        st.ptrRightX = camRx;
        st.ptrRightY = camRy;
        st.pullLeftX = ptrL.x;
        st.pullLeftY = ptrL.y;
        st.pullRightX = ptrR.x;
        st.pullRightY = ptrR.y;
        st.attractRadius = eng.physics.attractRadius;  // 键窗的点亮半径 = 物理场的引力半径
        st.compose = ime.preedit();
        st.cands.clear();
        // 候选取当前页那一段：ime.cands() 是整页池，不切片的话 Tab 翻页会显示同一批
        const int pageBase = ime.page() * ime.pageSize();
        for (int i = pageBase;
             i < int(ime.cands().size()) && int(st.cands.size()) < eng.candPerPage; ++i)
            st.cands.push_back({ime.cands()[size_t(i)].text, ime.cands()[size_t(i)].annot});
        st.candPage = ime.page();
        st.candSel = ime.sel();
        st.candPageCount = std::max(1, ime.pageCount());
        // 修饰键状态 = 锁定 | 物理按住 | 一次性：DPAD 那颗变实心、右肩键提示变 ↑/↓ 都看它
        const int modsOn = bd->locks | bd->holding | bd->sticky;
        st.modCtrl = (modsOn & 1) != 0;
        st.modShift = (modsOn & 2) != 0;
        st.modAlt = (modsOn & 4) != 0;
        std::string mods;  // 底栏 "[SHIFT]" 那串：按住/锁定/一次性都算（modsOn）
        if (modsOn & 1) mods += "CTRL";
        if (modsOn & 2) mods += mods.empty() ? "SHIFT" : "+SHIFT";
        if (modsOn & 4) mods += mods.empty() ? "ALT" : "+ALT";
        st.modsText = mods;

        // 没有变化就不画：空闲时这一屏一个像素都不会变，照旧整屏重画 = 白烧一个核
        // （fbdev 后端每帧还要软件旋转 92 万像素）。动画看速度/脉冲，输入看 uiDirty /
        // PTY 输出 / 终端脏标记；宁可多画一帧，也别漏掉刷新。
        const bool animating =
            st.snapPulseL > 0.004f || st.snapPulseR > 0.004f || st.pressedKey >= 0 ||
            std::fabs(ptrL.vx) + std::fabs(ptrL.vy) + std::fabs(ptrR.vx) + std::fabs(ptrR.vy) >
                0.5f ||
            std::fabs(camVx) + std::fabs(camVy) + std::fabs(camRVx) + std::fabs(camRVy) > 0.5f;
        const bool changed = uiDirty || n > 0 || (!terms.empty() && terms[active].tp->dirty()) ||
                             active != lastActiveSeen;
        const bool forced = prevAnimating && !animating;  // 动画收尾补画一帧，别停在半路
        prevAnimating = animating;
        if (animating || changed || forced) {
            // 脏区只有四类：整屏兜底 / 顶栏 / 底栏 / 终端脏行。动画、窗口/布局变化、
            // 面板类输入（uiDirty 可能改键区任意像素）一律整屏；纯终端输出走行级。
            const bool full = fullNext || uiDirty || active != lastActiveSeen;
            const bool composing = !st.compose.empty() || !st.cands.empty();
            uiDirty = false;
            fullNext = false;
            lastActiveSeen = active;

            std::vector<std::string> titles;  // 每帧现建：tab 会新建/关闭，不留两份真相
            titles.reserve(terms.size());
            for (const Term& t : terms) titles.push_back(t.title);
            // 状态栏签名（实现见 ui::barSignature，挨着两个 draw 函数放）：
            // 变了才重画顶栏/底栏，省掉每次非整屏帧的 74KB
            const std::string barSig = ui::barSignature(st, titles, active);
            const bool barsChanged = barSig != lastBarSig;
            lastBarSig = barSig;

            disp::DamageList dmg;
            if (overlayMode) {
                // overlay：屏幕中间是洞（那一块归下面的窗口画），要刷的只有我们自己那三块
                for (const disp::Rect& box : overlayRects(g)) dmg.push(box);
            } else if (full) {
                dmg.push({0, 0, g.screenW, g.screenH});
            } else {
                // 摇杆/吸附动画只动两块键面板：键只画在窗口里，但径向遮罩铺满整个面板、
                // 而且圆心跟着准心走（drawKeyHand 里 clearClip 之后那段），所以得推整块面板
                if (animating || forced) {
                    dmg.push({0, g.contentY, g.panelW, g.contentH});
                    dmg.push({g.screenW - g.panelW, g.contentY, g.panelW, g.contentH});
                }
                // 顶栏带 tab 条、底栏带输入法状态：内容真变了才重画
                if (barsChanged) {
                    dmg.push({0, 0, g.screenW, g.topBarH});
                    dmg.push({0, g.contentY + g.contentH, g.screenW, g.botBarH});
                }
                if (terms[active].scroll > 0 || lastScrollSeen > 0) {
                    // 回看历史：滚动条画在行网格外，整块终端兜底
                    dmg.push({g.termX, g.contentY, g.termW, g.contentH});
                } else {
                    for (int row = 0; row < terms[active].tp->rows(); ++row) {
                        if (!terms[active].tp->rowDirty(row)) continue;
                        dmg.push({g.termX, g.contentY + g.termPadTop + row * g.termCellH,
                                  g.termW, g.termCellH});
                    }
                    // 组字串和候选固定画在终端底部 ime_rows 行：只推那两行，
                    // 别像原来那样整块终端兜底（928×662 = 67%，直接顶到整屏阈值）
                    if (composing || lastComposing) {
                        const int y = g.contentY + g.termPadTop + g.termRows * g.termCellH;
                        dmg.push({g.termX, y, g.termW, g.imeRows * g.termCellH});
                    }
                }
            }
            if (!terms.empty()) {
                lastComposing = composing;
                lastScrollSeen = terms[active].scroll;
            }
            dmg.clip(g.screenW, g.screenH);

            // 同一份绘制代码按脏区跑：base clip 外的像素一个都不写，只拷脏区到 fb
            auto drawFrame = [&]() {
                r.fill({0, 0, g.screenW, g.screenH}, ui::c(ui::col::kBg));
                ui::drawTopBar(r, fonts, uic, g, st);
                if (overlayMode) {  // 中间那块是洞：终端网格、tab 条、底栏都不存在
                    ui::drawKeyPanels(r, fonts, uic, g, st);
                    return;
                }
                ui::drawTabBar(r, fonts, g, uic, titles, active);
                ui::drawKeyPanels(r, fonts, uic, g, st);
                ui::drawTermBackground(r, fonts, g);
                drawGrid(r, fonts, g, *terms[active].tp, cr, cc, cv && cursorOn,
                         terms[active].scroll);
                ui::drawComposeLine(r, fonts, g, st, cr, cc, terms[active].scroll == 0);
                ui::drawBottomBar(r, fonts, g, st);
            };
            r.beginFrame(dmg);
            for (int i = 0; i < dmg.size(); ++i) {
                r.setBaseClip(dmg.data()[i]);
                drawFrame();
            }
            r.endFrame(dmg);
            if (std::getenv("SIT_DEBUG")) {  // 量一下这次实际搬了多少像素
                long px = 0;
                for (int i = 0; i < dmg.size(); ++i)
                    px += long(dmg.data()[i].w) * long(dmg.data()[i].h);
                std::fprintf(stderr, "[damage] %d 块 %ld 像素 = %ld B\n", dmg.size(), px,
                             px * 4);
            }
            // 画过了：下一次变化由 vterm 的 damage 重新置位
            if (!terms.empty()) terms[active].tp->clearDirty();
        }

        if (n < 0) running = false;
    }

    if (ukbd.dropped())
        std::fprintf(stderr, "uinput: 丢弃 %zu 个非 ASCII 字符（中文需要 fcitx5/wtype，见 DESIGN §4.4）\n",
                     ukbd.dropped());
    if (!outPath.empty() && !bd->captureFrame(outPath))
        std::fprintf(stderr, "截图失败：%s\n", outPath.c_str());
    return 0;
}

}  // namespace app
