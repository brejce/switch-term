// 新架构主循环：UI 只认 cfg::Ui，终端只认 TerminalParser，PTY/IME 是可选驱动。
#include "app/app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <vector>
#include <poll.h>

#include "app/ime_route.h"
#include "config/lua_config.h"
#include "ime/builtin_ime.h"
#include "input/evdev_gamepad.h"
#include "input/uinput_kbd.h"
#include "input/uinput_mouse.h"
#include "modules/ime/t9_ime.h"
#include "modules/terminal/terminal_parser.h"
#include "render/font_manager.h"
#include "render/renderer.h"
#include "term/pty.h"
#include "ui/layout.h"
#include "ui/screen.h"
#include "ui/t9_popup.h"
#include "ui/theme.h"

namespace app {
namespace {

volatile sig_atomic_t gReload = 0;
void onReload(int) { gReload = 1; }

// 字符键被触发后闪亮的时长（毫秒）
constexpr long long kPopupFlashMs = 150;

std::string resolve(const std::string& resDir, const char* rel) {
    std::string p = resDir + "/" + rel;
    FILE* f = std::fopen(p.c_str(), "r");
    if (f) {
        std::fclose(f);
        return p;
    }
    return rel;  // 退回相对路径（从项目根运行）
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
         const std::string& runCmd) {
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
    // engine.log 目前只装配+打印；engine.sink / engine.ime.enabled 真实生效
    std::fprintf(stderr, "engine: sink=%s log=%s%s%s ngram=%d t9=%d ime=%s\n", eng.sink.c_str(),
                 eng.logLevel.c_str(), eng.logPath.empty() ? "" : "@", eng.logPath.c_str(),
                 eng.ngram, eng.t9, eng.imeEnabled ? "on" : "off");

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

    render::FontManager fm;
    if (!fm.init()) return 1;
    ui::FontBook fonts;
    if (!fonts.init(fm)) return 1;
    render::Renderer r;
    if (!r.init(bd, &fm)) return 1;

    // ② 终端：只认 TerminalParser 接口
    term::TerminalParser* tp = term::createParser("vterm");
    if (!tp->init(g.termRows, g.termCols)) return 1;
    tp->setHistoryLimit(eng.scrollback);  // engine.terminal.scrollback

    term::Pty pty;
    const char* shell = std::getenv("SHELL");
    std::vector<std::string> argv{shell && *shell ? shell : "/bin/bash"};
    if (!runCmd.empty()) {  // --run：直接 shell -c，输出干净（无欢迎信息干扰）
        argv.push_back("-c");
        argv.push_back(runCmd);
    }
    if (!pty.spawn(argv, g.termCols, g.termRows)) return 1;

    // ③ 输入法（T9 状态机之前，先用内置拼音把中文打通）
    ime::BuiltinIme ime;
    const std::string idx = resolve(resDir, "data/pinyin.idx");
    const std::string ng = resolve(resDir, "data/ngram.bin");
    if (!ime.load(idx, ng)) std::fprintf(stderr, "警告：没有 pinyin.idx，中文输入不可用\n");
    ime.setPageSize(eng.candPerPage);  // engine.ime.cand_per_page：IME 内部翻页跨度

    ime::T9Ime t9;
    t9.setKeys(uic.t9Alpha);

    // 按键入口：
    // eng.imeEnabled = true  → 内置输入法（routeKey，组字/选词）
    // eng.imeEnabled = false → 直通 routeRaw（只转 Ctrl/Alt/Shift，完全不组字）
    auto route = [&](char ch, int mods) -> RouteResult {
        return eng.imeEnabled ? routeKey(ime, ch, mods) : routeRaw(ch, mods);
    };

    UInputMouse mouse;

    input::EvdevGamepad gamepad;
    gamepad.setStickParams(eng.deadzone, eng.hysteresisDeg);  // engine.input.*
    int pads = gamepad.open();
    for (const auto& nm : gamepad.names()) std::fprintf(stderr, "gamepad: %s\n", nm.c_str());
    if (pads == 0) std::fprintf(stderr, "gamepad: 没有可读的手柄\n");

    const bool answerDa = runCmd.empty();
    auto readInto = [&]() -> long {  // 读一块→应答 DA→喂解析器
        char b[8192];
        long got = pty.readMaster(b, sizeof b);
        if (got > 0) {
            if (answerDa) answerDA(pty, b, got);  // --run 时对方不读 stdin，回复会被回显成垃圾
            tp->feed(b, size_t(got));
        }
        return got;
    };

    if (!runCmd.empty()) {  // 抽干输出（等到静默）
        int quiet = 0;
        for (int i = 0; i < 200 && quiet < 8; ++i) {
            struct pollfd pfd{pty.masterFd(), POLLIN, 0};
            ::poll(&pfd, 1, 16);
            quiet = (readInto() > 0) ? 0 : quiet + 1;
        }
    }

    if (std::getenv("SIT_DEBUG")) {  // 命令跑完后再看一次网格
        for (int row = 0; row < 5 && row < tp->rows(); ++row) {
            std::string line;
            for (int col = 0; col < 50 && col < tp->cols(); ++col) {
                term::Cell cc = tp->cellAt(row, col);
                line += (cc.cp >= 0x20 && cc.cp < 0x80) ? char(cc.cp) : '.';
            }
            std::fprintf(stderr, "[after %d] %s\n", row, line.c_str());
        }
    }

    std::signal(SIGHUP, onReload);
    std::signal(SIGUSR1, onReload);

    std::string pending;   // 待写 PTY
    int lastBtn = -1;      // 上一颗按下的键（双击锁定用）

    ui::AppState st;
    st.imeName = !eng.imeEnabled ? "OFF" : (ime.chinese ? "PINYIN" : "EN");

    // 动作分发：手柄/面板都走这里（映射来自 keymap.lua）
    auto msNow = [] {
        return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    long long lastPress[cfg::kKeyCount] = {0};
    // 九宫格命中：帧缓冲坐标 → cell 0..8（-1 = 不在九宫格里）。与绘制共用 t9CellRect
    auto t9Hit = [&](int x, int y) {
        if (x < 0 || y < 0) return -1;
        for (int i = 0; i < 9; ++i) {
            disp::Rect box = ui::t9CellRect(g, i);
            if (x >= box.x && x < box.right() && y >= box.y && y < box.bottom()) return i;
        }
        return -1;
    };
    auto doAction = [&](cfg::Action a) {
        switch (a) {
            case cfg::kMagic: {
                std::string letter = t9.magic();  // Grid→Popup 返回空；Popup→返回字母
                if (!letter.empty())
                    pending += route(letter[0], bd->sticky | bd->locks).bytes;
                else  // 刚弹开：默认选中锁定格自己承载的字符，不动就能直接上屏
                    t9.setLetter(ui::popupLetterIdx(t9.focus(), t9.focus()));
                break;
            }
            case cfg::kSpace: {
                // 网格态（不在弹窗）：空格 = 敲当前格的数字，走 T9 九键；
                // 已用字母组字或弹窗里：空格 = 上屏首候选
                bool t9Input = eng.imeEnabled && ime.chinese && !t9.popup() &&
                               (!ime.composing() || ime.digitsBuffer());
                if (t9Input) {
                    char d = t9.focusDigit();
                    if (d >= '2' && d <= '9') {
                        ime.push(d);
                        break;
                    }
                }
                pending += route(' ', bd->sticky | bd->locks).bytes;
                break;
            }
            case cfg::kTab: pending += route('\t', bd->sticky | bd->locks).bytes; break;
            case cfg::kEnter: pending += route('\r', bd->sticky | bd->locks).bytes; break;
            case cfg::kBackspace: pending += route(0x7f, bd->sticky | bd->locks).bytes; break;
            case cfg::kDelete: pending += "\x1b[3~"; break;
            case cfg::kEscape:
                pending += route(0x1b, bd->sticky | bd->locks).bytes;
                t9.leavePopup();
                break;
            case cfg::kModCtrl: bd->sticky ^= 1; break;
            case cfg::kModAlt: bd->sticky ^= 4; break;
            case cfg::kModShift: bd->sticky ^= 2; break;
            case cfg::kLockCtrl: bd->locks ^= 1; break;
            case cfg::kLockAlt: bd->locks ^= 4; break;
            case cfg::kLockShift: bd->locks ^= 2; break;
            case cfg::kMouseLeft: mouse.click(BTN_LEFT); break;    // 只点不移动
            case cfg::kMouseRight: mouse.click(BTN_RIGHT); break;
            // 面板/触摸独有动作（kIme..kCursorRight）：手柄路径不会产生，只有 --legacy 用
            case cfg::kIme:
            case cfg::kCopy:
            case cfg::kPaste:
            case cfg::kSelLeft:
            case cfg::kSelRight:
            case cfg::kLayer:
            case cfg::kCursorLeft:
            case cfg::kCursorRight:
            case cfg::kNone: break;
        }
        tp->markDirty();
    };

    auto onPadButton = [&](int btn) {
        if (btn < 0 || btn >= cfg::kKeyCount) return;
        long long now = msNow();
        cfg::Action act = km.single[btn];
        // 同一键窗口内按第二下 = 双击动作（DPAD 锁定修饰用）
        if (km.dbl[btn] && btn == lastBtn && now - lastPress[btn] < eng.doubleTapMs)
            act = km.dbl[btn];
        lastPress[btn] = now;
        lastBtn = btn;
        if (act != cfg::kNone) doAction(act);
    };
    auto onLeftStick = [&](int sector) {
        if (sector < 0) return;
        t9.stick(sector);
        tp->markDirty();
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
                fonts.init(fm);
                tp->resize(g.termRows, g.termCols);
                pty.resize(g.termCols, g.termRows);
                tp->markDirty();
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
                eng.sink = useUinput ? "uinput" : "pty";  // 输出口开机时定下，热重载不换设备
                g.fromConfig(uic, ui::layout::kScale);
                bd->setCanvasSize(g.screenW, g.screenH);
                fonts.init(fm);
                tp->resize(g.termRows, g.termCols);
                tp->setHistoryLimit(eng.scrollback);
                pty.resize(g.termCols, g.termRows);
                // 瞬态状态复位：焦点/弹窗/组字/候选/上一颗键
                t9.reset();
                ime.setPageSize(eng.candPerPage);  // 翻页跨度跟着新配置走（组字缓冲不丢，页号夹住）
                st.focusIdx = 4;
                st.popup = false;
                st.pressedCell = -1;
                st.compose.clear();
                st.cands.clear();
                st.candPage = 0;
                lastBtn = -1;
                tp->markDirty();
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
                pty.writeMaster(pending.data(), pending.size());
            pending.clear();
        }

        // ⑤ 读终端输出
        long n = readInto();
        gamepad.pollPad({onPadButton, onLeftStick, [](int) {}});

        // 鼠标 demo 路径：只改 UI 状态（焦点/选中/动画），绝不往 pending 里塞字节、不碰 PTY
        const long long nowMs = msNow();
        if (st.pressedCell >= 0 && nowMs - st.pressedMs >= kPopupFlashMs) st.pressedCell = -1;
        st.hoverCell = t9Hit(bd->mouseX, bd->mouseY);
        if (t9.popup() && st.hoverCell >= 0) {  // 悬停即选中
            int li = ui::popupLetterIdx(t9.focus(), st.hoverCell);
            if (li >= 0 && li < t9.letterCount()) t9.setLetter(li);
        }
        for (const disp::ClickEvent& ck : clicks) {
            int hit = t9Hit(ck.x, ck.y);
            if (hit < 0) continue;  // 九宫格以外忽略
            if (!t9.popup()) {
                if (ck.button == 1) t9.setFocus(hit);              // 左键选格，不弹窗
                else if (ck.button == 2) doAction(cfg::kMagic);    // 右键 = 魔法键
                continue;
            }
            int li = -1;  // 弹窗中：左键点字符键 / 右键触发当前选中
            if (ck.button == 1) li = ui::popupLetterIdx(t9.focus(), hit);
            else if (ck.button == 2) li = t9.letterIdx();
            else continue;
            if (li < 0 || li >= t9.letterCount()) continue;  // 不是字符键（或符号层截断）
            t9.setLetter(li);
            st.pressedCell = ui::popupCellFor(t9.focus(), li);
            st.pressedMs = nowMs;
        }

        // ⑥ 画
        int cr = 0, cc = 0;
        bool cv = true;
        tp->cursor(cr, cc, cv);
        if (long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
            now - lastBlink > 500) {
            lastBlink = now;
            cursorOn = !cursorOn;
            tp->markDirty();
        }

        st.layer = (bd->locks & 2) ? "SYMBOL" : "ALPHA";  // 锁 Shift = 符号层
        t9.setKeys(st.layer == "SYMBOL" ? uic.t9Symbol : uic.t9Alpha);
        st.focusIdx = t9.focus();
        st.popup = t9.popup();
        st.popupFocus = t9.popup() ? t9.focus() : -1;
        st.selCell = t9.popup() ? ui::popupCellFor(t9.focus(), t9.letterIdx()) : -1;
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
        ui::drawT9Panel(r, fonts, uic, g, st);
        ui::drawTermBackground(r, fonts, g);
        drawGrid(r, fonts, g, *tp, cr, cc, cv && cursorOn);
        ui::drawComposeLine(r, fonts, g, st);
        ui::drawMousePanel(r, fonts, uic, g, st);
        ui::drawBottomBar(r, fonts, g, st);
        r.endFrame();

        if (n < 0) running = false;
    }

    delete tp;
    if (ukbd.dropped())
        std::fprintf(stderr, "uinput: 丢弃 %zu 个非 ASCII 字符（中文需要 fcitx5/wtype，见 DESIGN §4.4）\n",
                     ukbd.dropped());
    if (!outPath.empty() && !bd->captureFrame(outPath))
        std::fprintf(stderr, "截图失败：%s\n", outPath.c_str());
    return 0;
}

}  // namespace app
