// 新架构主循环：UI 只认 cfg::Ui，终端只认 TerminalParser，PTY/IME 是可选驱动。
#include "app/app.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
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
#include "input/uinput_kbd.h"
#include "input/uinput_mouse.h"
#include "modules/terminal/terminal_parser.h"
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
const char* const kLayerNames[cfg::kLayerCount] = {"ALPHA", "NUM", "SYM"};

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

// 终端网格：逐格画，宽字符跳过后一格
void drawGrid(render::Renderer& r, const ui::FontBook& f, const ui::Geometry& g,
              term::TerminalParser& tp, int cursorRow, int cursorCol, bool cursorVisible) {
    const int cellW = g.termCellW;
    const int cellH = g.termCellH;
    const int baselineOff = (cellH + f.term->ascent - f.term->descent) / 2;

    for (int row = 0; row < tp.rows(); ++row) {
        int y = g.contentY + g.termPadTop + row * cellH;
        // ① 先铺底色（宽字符两格一起铺，避免后一格的底色盖掉前一个字）
        for (int col = 0; col < tp.cols();) {
            term::Cell c = tp.cellAt(row, col);
            int w = c.wide ? 2 : 1;
            if (c.bg != ui::col::kTerm) {
                int x = g.termX + g.termPadX + col * cellW;
                r.fill({x, y, cellW * w, cellH}, ui::c(c.bg));
            }
            col += w;
        }
        // ② 再画字
        for (int col = 0; col < tp.cols();) {
            term::Cell c = tp.cellAt(row, col);
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
    // 光标
    if (cursorVisible && cursorRow >= 0 && cursorRow < tp.rows() && cursorCol >= 0 &&
        cursorCol < tp.cols()) {
        int x = g.termX + g.termPadX + cursorCol * cellW;
        int y = g.contentY + g.termPadTop + cursorRow * cellH;
        term::Cell c = tp.cellAt(cursorRow, cursorCol);
        r.fill({x, y + cellH - 2, cellW, 2}, ui::c(c.fg));
    }
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

// 应答 fish 的设备属性查询（否则启动会卡住等超时）
void answerDA(term::Pty& pty, const char* buf, long n) {
    for (long i = 0; i + 1 < n; ++i) {
        if (buf[i] == 0x1b && buf[i + 1] == '[' &&
            (i + 2 < n && (buf[i + 2] == 'c' || buf[i + 2] == '0'))) {
            const char* reply = "\x1b[?62;22c";
            pty.writeMaster(reply, std::strlen(reply));
            return;
        }
    }
}

}  // namespace

int run(disp::DisplayBackend* bd, const std::string& resDir, const std::string& outPath,
         const std::string& runCmd, int tabs) {
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
    };
    std::vector<Term> terms;
    int active = 0;
    int tabSeq = 0;

    const char* shell = std::getenv("SHELL");
    std::vector<std::string> argv{shell && *shell ? shell : "/bin/bash"};
    if (!runCmd.empty()) {  // --run：直接 shell -c，输出干净（无欢迎信息干扰）
        argv.push_back("-c");
        argv.push_back(runCmd);
    }

    installPathShims();  // term/quit 命令：必须在第一个 fork 之前

    auto spawnTerm = [&]() -> bool {
        // 防递归：--run 的脚本自己吐 newtab 也不会无限生 tab
        if (terms.size() >= 16) {
            std::fprintf(stderr, "tab: 已有 16 个，拒绝新建\n");
            return false;
        }
        std::unique_ptr<term::TerminalParser> tp(term::createParser("vterm"));
        if (!tp || !tp->init(g.termRows, g.termCols)) return false;
        tp->setHistoryLimit(eng.scrollback);  // engine.terminal.scrollback
        std::unique_ptr<term::Pty> pty(new term::Pty);
        if (!pty->spawn(argv, g.termCols, g.termRows)) return false;
        terms.push_back({std::move(tp), std::move(pty), "TERM " + std::to_string(++tabSeq), {}});
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
    if (!spawnTerm()) return 1;  // 第一个 tab 起不来就没得玩
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
        RouteResult r = eng.imeEnabled ? routeKey(ime, ch, mods) : routeRaw(ch, mods);
        if (routeTrace)
            std::fprintf(stderr, "[route] ch=%02x zh=%d composing=%d consumed=%d bytes=%zu\n",
                         (unsigned char)ch, int(ime.chinese), int(ime.composing()),
                         int(r.consumed), r.bytes.size());
        return r;
    };

    UInputMouse mouse;

    input::EvdevGamepad gamepad;
    gamepad.setStickParams(eng.deadzone, eng.hysteresisDeg);  // engine.input.*
    int pads = gamepad.open();
    for (const auto& nm : gamepad.names()) std::fprintf(stderr, "gamepad: %s\n", nm.c_str());
    if (pads == 0) std::fprintf(stderr, "gamepad: 没有可读的手柄\n");

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

    if (!runCmd.empty()) {  // 抽干输出（等到静默）
        int quiet = 0;
        for (int i = 0; i < 200 && quiet < 8; ++i) {
            struct pollfd pfd{terms[0].pty->masterFd(), POLLIN, 0};
            ::poll(&pfd, 1, 16);
            quiet = (readInto() > 0) ? 0 : quiet + 1;
        }
    }

    if (std::getenv("SIT_DEBUG")) {  // 命令跑完后再看一次网格
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
    const app::physics::Field fieldL{0, 7};    // 左场：键 0..14，零位 d
    const app::physics::Field fieldR{15, 22};  // 右场：键 15..29，零位 k
    app::physics::Pointer ptrL, ptrR;
    int activeSide = 0;  // 0=左 1=右：L 上屏取哪一侧（最近推过、推得更狠的那根摇杆）
    int layerIdx = 0;    // 0=ALPHA 1=NUM 2=SYM（L3 循环）
    float keyCx[cfg::kKeyV2Count], keyCy[cfg::kKeyV2Count];
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
    };
    resetFields();

    // ui.lua 里没写 keys 就是配置缺失：键区会是空的，启动时说一句（没有别的面板可退回了）
    for (const cfg::KeyV2& k : uic.keysV2)
        if (!k.x && !k.y) {
            std::fprintf(stderr, "警告：ui.lua 没有 keys，左右键区是空的\n");
            break;
        }

    ui::AppState st;
    st.imeName = !eng.imeEnabled ? "OFF" : (ime.chinese ? "PINYIN" : "EN");

    // 动作分发：手柄/面板都走这里（映射来自 keymap.lua）
    auto msNow = [] {
        return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    long long lastPress[cfg::kKeyCount] = {0};
    auto doAction = [&](cfg::Action a) {
        switch (a) {
            case cfg::kMagic: {  // L（LB）：只上屏吸附住的键，hover 永不上屏
                const app::physics::Pointer& p = activeSide ? ptrR : ptrL;
                if (p.snapped < 0) {  // 指针在两键之间的缝里：没得可上，也没提示（SIT_DEBUG 才说）
                    if (routeTrace) std::fprintf(stderr, "[magic] 丢弃：snapped=-1，指针在键缝里\n");
                    break;
                }
                const std::string& ch = uic.layers[layerIdx][p.snapped];
                if (ch.empty()) {
                    if (routeTrace) std::fprintf(stderr, "[magic] 丢弃：键 %d 在当前层是空格\n",
                                                 p.snapped);
                    break;
                }
                pending += route(ch[0], bd->sticky | bd->locks).bytes;
                break;
            }
            case cfg::kSpace: pending += route(' ', bd->sticky | bd->locks).bytes; break;
            case cfg::kTab: pending += route('\t', bd->sticky | bd->locks).bytes; break;
            case cfg::kEnter: pending += route('\r', bd->sticky | bd->locks).bytes; break;
            case cfg::kBackspace: pending += route(0x7f, bd->sticky | bd->locks).bytes; break;
            case cfg::kDelete: pending += "\x1b[3~"; break;
            case cfg::kEscape:
                pending += route(0x1b, bd->sticky | bd->locks).bytes;
                break;
            case cfg::kModCtrl: bd->sticky ^= 1; break;
            case cfg::kModAlt: bd->sticky ^= 4; break;
            case cfg::kModShift: bd->sticky ^= 2; break;
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
            case cfg::kCursorLeft: pending += "\x1b[D"; break;
            case cfg::kCursorRight: pending += "\x1b[C"; break;
            // 面板/触摸独有动作（kIme..kLayer）：手柄路径不会产生，只有 --legacy 用
            case cfg::kIme:
            case cfg::kCopy:
            case cfg::kPaste:
            case cfg::kSelLeft:
            case cfg::kSelRight:
            case cfg::kLayer:
            case cfg::kNone: break;
        }
        terms[active].tp->markDirty();
    };

    auto onPadButton = [&](int btn) {
        if (btn < 0 || btn >= cfg::kKeyCount) return;
        long long now = msNow();
        st.pressedKey = btn;  // 提示条闪一下（150ms）
        st.pressedKeyMs = now;
        cfg::Action act = km.single[btn];
        // 同一键窗口内按第二下 = 双击动作（DPAD 锁定修饰用）
        if (km.dbl[btn] && btn == lastBtn && now - lastPress[btn] < eng.doubleTapMs)
            act = km.dbl[btn];
        lastPress[btn] = now;
        lastBtn = btn;
        if (routeTrace)  // 收到没有？收成了什么动作？没收到的键这里一行都不会有
            std::fprintf(stderr, "[pad] btn=%d act=%s\n", btn, cfg::actionName(act));
        if (act != cfg::kNone) doAction(act);
    };
    long long lastBlink = 0;
    bool cursorOn = true;
    bool running = true;

    while (running) {
        std::string keys;
        std::vector<disp::ClickEvent> clicks;
        running = bd->pump(keys, clicks);
        if (bd->uiDirty) {  // 窗口尺寸变了：重排 + 重建字体
            bd->uiDirty = 0;
            float want = bd->scaleHint(ui::layout::kDesignW, ui::layout::kDesignH);
            if (std::fabs(want - ui::layout::kScale) > 0.06f) {
                ui::layout::setScale(want);
                g.fromConfig(uic, ui::layout::kScale);
                bd->setCanvasSize(g.screenW, g.screenH);
                bd->setRotation(eng.rotate);
                fonts.init(fm);
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
                g.fromConfig(uic, ui::layout::kScale);
                bd->setCanvasSize(g.screenW, g.screenH);
                bd->setRotation(eng.rotate);
                fonts.init(fm);
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
        for (char ch : keys) {
            RouteResult rr = route(ch, bd->sticky | bd->locks);
            if (rr.langChanged) {
                st.imeName = ime.chinese ? "PINYIN" : "EN";
                st.lang = ime.chinese ? "ZH" : "EN";
            }
            pending += rr.bytes;
        }
        if (!pending.empty()) {
            if (useUinput)
                ukbd.typeBytes(pending);  // 打给系统；PTY 只收 shell 输出，不再收这些键
            else
                terms[active].pty->writeMaster(pending.data(), pending.size());
            pending.clear();
        }

        // ⑤ 读终端输出
        long n = readInto();
        gamepad.pollPad({onPadButton, [](int) {}, [](int) {}});

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
            app::physics::step(ptrL, fieldL, keyCx, keyCy, lx, ly, eng.physics);
            app::physics::step(ptrR, fieldR, keyCx, keyCy, rx, ry, eng.physics);
        }

        // 鼠标：只处理 tab 条（点「+」新建，点已有 tab 切过去）；键区暂时不接鼠标
        const long long nowMs = msNow();
        if (st.pressedKey >= 0 && nowMs - st.pressedKeyMs >= kFlashMs) st.pressedKey = -1;
        for (const disp::ClickEvent& ck : clicks) {
            int tab = ui::tabAt(g, uic, int(terms.size()), ck.x, ck.y);
            if (tab < 0) continue;
            if (tab == int(terms.size())) spawnTerm();
            else active = tab;
        }

        // ⑥ 画
        int cr = 0, cc = 0;
        bool cv = true;
        terms[active].tp->cursor(cr, cc, cv);
        if (long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
            now - lastBlink > 500) {
            lastBlink = now;
            cursorOn = !cursorOn;
            terms[active].tp->markDirty();
        }

        st.layer = layerIdx;  // L3 循环，不再跟 Shift 锁定走
        st.layerName = kLayerNames[layerIdx];
        st.snapLeft = ptrL.snapped;
        st.snapRight = ptrR.snapped;
        st.hoverLeft = ptrL.hovered;
        st.hoverRight = ptrR.hovered;
        st.ptrLeftX = ptrL.x;
        st.ptrLeftY = ptrL.y;
        st.ptrRightX = ptrR.x;
        st.ptrRightY = ptrR.y;
        st.attractRadius = eng.physics.attractRadius;  // 键窗的点亮半径 = 物理场的引力半径
        st.compose = ime.preedit();
        st.cands.clear();
        for (const auto& cd : ime.cands())
            if (int(st.cands.size()) < eng.candPerPage)
                st.cands.push_back({cd.text, cd.annot});
        st.candPage = ime.page();
        st.candPageCount = std::max(1, ime.pageCount());
        std::string mods;
        if (bd->locks & 1) mods += "CTRL";
        if (bd->locks & 2) mods += mods.empty() ? "SHIFT" : "+SHIFT";
        if (bd->locks & 4) mods += mods.empty() ? "ALT" : "+ALT";
        st.modsText = mods;

        r.beginFrame();
        r.fill({0, 0, g.screenW, g.screenH}, ui::c(ui::col::kBg));
        ui::drawTopBar(r, fonts, uic, g, st);
        std::vector<std::string> titles;  // 每帧现建：tab 会新建/关闭，不留两份真相
        titles.reserve(terms.size());
        for (const Term& t : terms) titles.push_back(t.title);
        ui::drawTabBar(r, fonts, g, uic, titles, active);
        ui::drawKeyPanels(r, fonts, uic, g, st);
        ui::drawTermBackground(r, fonts, g);
        drawGrid(r, fonts, g, *terms[active].tp, cr, cc, cv && cursorOn);
        ui::drawComposeLine(r, fonts, g, st);
        ui::drawBottomBar(r, fonts, g, st);
        r.endFrame();

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
