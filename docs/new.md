> 历史存档（2026-10 的设计稿原文，含当时的 `docs/DESIGN.md` / `docs/ui-mockup.html` 内容）。
> 现行文档：`STATUS.md`（现状）、`docs/ARCH.md`（架构与配置）、`docs/LAYOUT-v2.md`（布局）。

为你重新整理最终的**设计手册**和**设计稿**。这次完全按照“启动装配，运行纯 C++”和“终端解析器解耦”的终极架构来定稿。

---

# 一、 设计手册（当时保存为 `docs/DESIGN.md`，现已并入 `docs/ARCH.md`）

当时指令是「保存为 `docs/DESIGN.md`，作为后续 C++ 与 Lua 开发的唯一准则」；那份文件已并入 `docs/ARCH.md`。

```markdown
# switch-ime-term 项目设计手册 (v4.0 Final)

> **核心架构**：三分区纯字符 UI + 可插拔终端解析器 + Lua 启动装配 + 运行时纯 C++ 状态机。
> **目标**：打造极致的、可运行在 Switchroot / Linux 终端下的手柄 T9 输入法。

---

## 一、 总体系统架构与解耦

屏幕被划分为三大独立视口（Viewport），外加顶部/底部状态栏。所有 UI 元素均基于**纯字符网格**渲染，无任何图形化边框。

```text
┌──────────────────────────────────────────────────────────┐
│ 顶部状态栏 (SYS.TERM / 时间 / 电量)                      │
├──────────────┬────────────────────────┬──────────────────┤
│              │                        │                  │
│ 左侧手柄面板 │ 中间终端区域           │ 右侧手柄面板     │
│ (T9输入法)   │ (TerminalParser 接口)  │ (鼠标模拟/ABXY)  │
│              │                        │                  │
├──────────────┴────────────────────────┴──────────────────┤
│ 底部状态栏 (Layer / Mods / IME状态)                      │
└──────────────────────────────────────────────────────────┘
```

### 1.1 终端解析层（Terminal Backend）解耦
主程序不绑定任何特定的终端库，而是通过统一的 `TerminalParser` 接口与底层通信：
*   `feed(data, len)`：接收 PTY 字节流。
*   `getScreenGrid()`：返回二维字符网格（包含字符、颜色、宽字符标记）。
*   `getCursorState()`：返回光标位置与状态。
*   **支持实现**：`VTermParser` (基于 libvterm，稳定首选)、`GhosttyParser` (基于 libghostty-vt，性能极佳)、`DummyParser` (用于 UI 开发)。

### 1.2 Lua 与 C++ 的边界划分（极速架构）
**绝对不在高频循环中执行 Lua 解释器。**
*   **启动装配阶段**：C++ 启动 Lua 虚拟机，执行 `keymap.lua` 和 `ui.lua`。Lua 将 UI 字符矩阵、按键映射表通过 C API 注册给 C++，C++ 将其转换为原生的 `std::unordered_map` 和 `std::vector`。
*   **运行阶段**：主事件循环**纯 C++ 驱动**。鼠标移动、按键高亮、状态切换完全不查 Lua，零性能损耗。
*   **回调阶段**：只有触发复杂业务逻辑（如 T9 弹窗状态切换）时，才调用 Lua 绑定的闭包。

---

## 二、 视觉规范 (Visual Spec)

*   **字体**：强制等宽字体 (`JetBrains Mono` / `Fira Code`)。
*   **颜色规范**：
    *   背景底色：`#08080c` (极深黑)
    *   面板底色：`#0c0c14` (深灰黑)
    *   主文本/普通状态：`#c8c8d4` (灰白)
    *   左手柄高亮：青 `#22d3ee` / 右手柄高亮：红 `#ff6b5a`
    *   符号层/魔法键：紫 `#a855f7`
    *   聚焦/弹窗选中：蓝 `#4ab8e6` (反色/下划线/底色)
*   **无边框原则**：所有按键、网格、面板，只通过字符本身的颜色、反色（背景色）和排版留白来区分，杜绝任何实线或圆角绘制。

---

## 三、 手柄交互逻辑映射

### 3.1 左手区：T9 极客输入法
*   **左摇杆 (3x3 物理映射)**：
    *   完美对应标准九键布局（1-9）。摇杆居中时，默认选中中心键 `5 (JKL)`（高亮反色）。
    *   推摇杆改变焦点格子。
*   **LB (左肩键) —— 魔法键 (Magic Key)**：
    *   **Grid 状态按下**：当前选中的格子“弹开”（其他格子变暗），进入 Popup 状态。
    *   **Popup 状态按下**：输入当前选中的字母。**不退出弹窗**，允许连击。
    *   **摇杆左右推**：在 Popup 状态下，切换该格子的三个/四个字母。
*   **LT (左扳机) —— 空格键**：直接输出空格。
*   **DPAD (十字键) —— 修饰符**：
    *   上 = `Tab` (单击触发，不可锁定)。
    *   左 = `Ctrl` / 下 = `Alt` / 右 = `Shift`。
    *   **双击锁定**：双击 `Ctrl/Alt/Shift` 任意键，锁定该修饰符，底部状态栏亮起提示。再次双击解锁。

### 3.2 右手区：鼠标与全局控制
*   **右摇杆**：纯鼠标移动模拟。
*   **RB (右肩键)**：鼠标左键点击。
*   **RT (右扳机)**：鼠标右键点击。
*   **ABXY**：
    *   `A` = 回车 (Enter)
    *   `B` = 退格 (Backspace)
    *   `X` = 删除 (Del)
    *   `Y` = 取消 (Esc)

---

## 四、 Lua 配置文件参考

### `config/keymap.lua` (按键逻辑)
```lua
-- 修饰键锁定
keymap.bind_double("dpad", "left",  function() cpp_api.toggle_mod("ctrl")  end)
keymap.bind_double("dpad", "down",  function() cpp_api.toggle_mod("alt")   end)
keymap.bind_double("dpad", "right", function() cpp_api.toggle_mod("shift") end)

-- T9 魔法键 (LB)
keymap.bind("shoulder", "L", function()
    if ime.state == "grid" then
        ime.state = "popup"
        ime.popup_idx = 1
    elseif ime.state == "popup" then
        local char = current_layout[ime.focus_idx].letters[ime.popup_idx]
        cpp_api.send_to_pty(char) -- 只发送，状态保持
    end
end)

-- 鼠标模拟
keymap.bind_axis("rstick", "move", function(dx, dy) cpp_api.mouse_move(dx * 2.5, dy * 2.5) end)
keymap.bind("shoulder", "R", function() cpp_api.mouse_click("left") end)
keymap.bind("trigger", "ZR", function() cpp_api.mouse_click("right") end)
```

---

## 五、 开发里程碑 (Roadmap)

1.  **M0 - 骨架与渲染**：完成三分区坐标划分，实现假数据填充的纯字符 UI 渲染，接入 FreeType。
2.  **M1 - 终端接入**：封装 `TerminalParser`，跑通 PTY，确保中文宽字符显示无误。
3.  **M2 - 驱动与鼠标**：接入 evdev，打通右摇杆鼠标（包含 RB/RT 左右键），打通左摇杆基本方向捕获。
4.  **M3 - Lua 装配与 T9**：集成 Lua 虚拟机，将 Lua 映射转为 C++ 结构体，实现 Grid/Popup 状态机及 LB 连击。
5.  **M4 - Switch 移植**：交叉编译部署，调整死区、颜色对比度及 Framebuffer 刷新性能。
```

---

# 二、 设计稿 (`docs/ui-mockup.html`)

这份 HTML 完美还原了手册中的规范，没有任何圆角和边框，只有字符矩阵、颜色和反色高亮。你可以直接保存到本地预览交互。

```html
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<title>switch-ime-term · 终极设计稿</title>
<style>
:root{
  --bg:#08080c; --bg-panel:#0c0c14; --bg-term:#06060a;
  --fg:#c8c8d4; --fg-dim:#5a5a72; --fg-faint:#2e2e40;
  --green:#4ade80; --blue:#4ab8e6; --red:#ff6b5a;
  --yellow:#fbbf24; --purple:#a855f7; --cyan:#22d3ee;
  --line:#1a1a26;
}
*{margin:0;padding:0;box-sizing:border-box}
html,body{
  width:100%;height:100%;background:#000;
  display:flex;align-items:center;justify-content:center;
  font-family:'JetBrains Mono','Fira Code',monospace;
  color:var(--fg);overflow:hidden;
}
.stage{width:1280px;height:720px;transform-origin:center}
.screen{
  width:1280px;height:720px;background:var(--bg);
  display:flex;flex-direction:column;overflow:hidden;position:relative;
}

/* ================= 状态栏 ================= */
.statusbar{
  height:32px;flex:0 0 32px;
  display:flex;align-items:center;justify-content:space-between;
  padding:0 16px;font-size:11px;letter-spacing:.5px;
  border-bottom:1px solid var(--line);color:var(--fg-dim);
}
.statusbar .sys{color:var(--cyan);letter-spacing:1.5px}
.statusbar .time{color:var(--fg);font-variant-numeric:tabular-nums}
.statusbar .bat{color:var(--green)}
.statusbar .sep{color:var(--fg-faint);margin:0 8px}

/* ================= 主区域 ================= */
.main{flex:1;display:flex;min-height:0}

/* ---------- 手柄面板 ---------- */
.panel{
  width:220px;flex:0 0 220px;
  background:var(--bg-panel);
  display:flex;flex-direction:column;align-items:center;
  padding:12px 10px 8px;gap:8px;overflow:hidden;
}
.panel.left{border-right:1px solid var(--line)}
.panel.right{border-left:1px solid var(--line)}

.ptitle{
  font-size:10px;letter-spacing:1.5px;color:var(--fg-dim);
  align-self:flex-start;text-transform:uppercase;margin-top:6px;
}
.panel.left .ptitle{color:var(--cyan)}
.panel.right .ptitle{color:var(--red)}

/* 摇杆容器：3x3 标准九键 */
.stick-wrap{
  position:relative;width:180px;height:180px;flex:0 0 180px;
  margin:4px 0;cursor:pointer;
}
.stick-layer{
  position:absolute;inset:0;
  display:none;
  grid-template-columns:repeat(3, 1fr);
  grid-template-rows:repeat(3, 1fr);
  place-items:center;
  gap: 2px;
}
.screen.mode-alpha .panel.left .stick-layer.alpha {display:grid}
.screen.mode-symbol .panel.left .stick-layer.symbol {display:grid}
.screen.mode-alpha .panel.right .stick-layer.alpha {display:grid}

/* 字符按键 */
.key{
  display:flex;flex-direction:column;align-items:center;justify-content:center;
  width:48px;height:48px;font-weight:700;
  cursor:pointer;user-select:none;transition:all .15s;
}
.key .num{font-size:10px;color:var(--fg-dim);margin-bottom:1px}
.key .txt{font-size:14px;letter-spacing:1px}

/* 位置分布（3x3 全网格） */
.r1c1{grid-area:1/1} .r1c2{grid-area:1/2} .r1c3{grid-area:1/3}
.r2c1{grid-area:2/1} .r2c2{grid-area:2/2} .r2c3{grid-area:2/3}
.r3c1{grid-area:3/1} .r3c2{grid-area:3/2} .r3c3{grid-area:3/3}

/* 颜色状态 */
.panel.left .layer1{color:var(--cyan)}
.panel.right .layer1{color:#ff9a8a}
.panel.left .symbol .layer1{color:var(--purple)}

/* 焦点与弹窗 */
.screen.is-popup .key.layer1:not(.active-popup){
  color:var(--fg-faint) !important;
}
.key.active-popup{
  background:rgba(74,184,230,.15);
  box-shadow:inset 0 0 0 1px var(--blue);
}
.key.active-popup .txt{color:#fff}
.key.active-popup .num{color:var(--blue)}
/* 默认中心高亮 */
.key.center-key {
  background:rgba(74,184,230,.1);
  box-shadow:inset 0 0 0 1px rgba(74,184,230,.5);
}
.key.center-key .txt {color:#fff;}

/* 鼠标准星 */
.mouse-cross{
  grid-area:2/2;
  color:var(--red);
  font-size:24px;
  display:flex;align-items:center;justify-content:center;
  width:100%;height:100%;
}

/* 点击反馈 */
.key.pressed{background:var(--fg) !important;color:var(--bg) !important;}
.shoulder.pressed{background:var(--fg) !important;color:var(--bg) !important}
.abxy .b.pressed,.dpad .d.pressed{background:var(--fg) !important;color:var(--bg) !important}

/* 肩键 */
.shoulder-row{width:100%;display:flex;gap:6px;margin-bottom:2px}
.panel.left .shoulder-row{justify-content:flex-start}
.panel.right .shoulder-row{justify-content:flex-end}
.shoulder{
  height:20px;padding:0 8px;display:flex;align-items:center;
  font-size:10px;font-weight:700;letter-spacing:.5px;
  cursor:pointer;user-select:none;transition:all .12s;
  background:#14141e;color:var(--fg-dim);
}
.shoulder.LB{color:var(--purple);background:#1a1226}
.shoulder.LT{color:var(--cyan);background:#0e1e26}
.shoulder.RB{color:var(--green);background:#0e1e12}
.shoulder.RT{color:var(--red);background:#1e0e0c}
.screen.is-popup .shoulder.LB{background:var(--purple);color:#000}

/* 十字键 */
.dpad{
  display:grid;grid-template-columns:repeat(3,28px);grid-template-rows:repeat(3,28px);
  gap:0;margin-top:2px;text-align:center;font-size:9px;color:var(--fg-dim);
}
.dpad .d{display:flex;align-items:center;justify-content:center;cursor:pointer;user-select:none}
.dpad .d.blank{pointer-events:none}
.dpad .d.center{color:var(--fg-faint);pointer-events:none}
.dpad .d.tab{color:var(--green)}
.dpad .d.shift{color:var(--yellow)}
.dpad .d.ctrl{color:var(--purple)}
.dpad .d.alt{color:var(--blue)}
/* 修饰键锁定视觉 */
.screen.mod-ctrl .dpad .d.ctrl,
.screen.mod-shift .dpad .d.shift,
.screen.mod-alt .dpad .d.alt {
  background:var(--fg);
  color:#000;
}

/* ABXY */
.abxy{
  display:grid;grid-template-columns:repeat(3,30px);grid-template-rows:repeat(3,30px);
  gap:0;margin-top:2px;text-align:center;font-size:13px;font-weight:700;
}
.abxy .b{display:flex;align-items:center;justify-content:center;cursor:pointer;user-select:none}
.abxy .b.blank{pointer-events:none}
.abxy .b.a{color:var(--green)}
.abxy .b.b{color:var(--red)}
.abxy .b.x{color:var(--blue)}
.abxy .b.y{color:var(--yellow)}

/* 功能键列表 */
.klist{width:100%;margin-top:auto;display:flex;flex-direction:column;gap:4px}
.krow{display:flex;align-items:center;gap:6px;font-size:9px;line-height:1.4}
.kk{flex:0 0 auto;min-width:28px;height:18px;display:flex;align-items:center;justify-content:center;font-weight:700;background:#14141e}
.kk.purple{color:var(--purple);background:#1a1226}
.kk.cyan{color:var(--cyan);background:#0e1e26}
.kk.green{color:var(--green);background:#0e1e12}
.kk.red{color:var(--red);background:#1e0e0c}
.kk.blue{color:var(--blue);background:#0e1626}
.kk.yellow{color:var(--yellow);background:#1e1a0e}
.kk.dim{color:var(--fg-dim)}
.kd{color:var(--fg-dim);font-size:9px}
.kd b{color:var(--fg);font-weight:600}

/* ================= 终端 ================= */
.terminal{
  flex:1;display:flex;flex-direction:column;
  background:var(--bg-term);min-width:0;position:relative;
  padding:10px 14px 8px;
}
.term-lines{flex:1;font-size:12.5px;line-height:1.65;white-space:pre-wrap;overflow:hidden}
.prompt{color:var(--green)}
.prompt-path{color:var(--blue)}
.cmd{color:#fff}
.out{color:#7a7a90}
.warn{color:var(--yellow)}

.input-line{
  white-space:pre-wrap;display:flex;align-items:baseline;flex-wrap:wrap;
  font-size:12.5px;line-height:1.65;
}
.compose-str{color:var(--blue);text-decoration:underline;text-underline-offset:3px;letter-spacing:1px}
.cursor{display:inline-block;width:7px;height:14px;background:var(--blue);vertical-align:-2px;margin:0 1px;animation:blink 1s steps(1) infinite}
@keyframes blink{50%{opacity:0}}

/* 候选行 */
.cand-line{
  display:flex;align-items:center;gap:6px;flex-wrap:wrap;
  font-size:12.5px;line-height:1.65;margin-top:2px;padding:2px 0;border-top:1px solid var(--line);
}
.cand-item{display:flex;align-items:baseline;gap:4px;padding:1px 6px;cursor:pointer;user-select:none;transition:all .1s}
.cand-item .idx{font-size:9px;color:var(--fg-faint)}
.cand-item .txt{color:#a8a8bc}
.cand-item .ann{font-size:10px;color:var(--fg-dim);font-style:italic}
.cand-item.active{background:rgba(74,184,230,.15);box-shadow:inset 0 0 0 1px var(--blue)}
.cand-item.active .idx{color:var(--blue)}
.cand-item.active .txt{color:#fff}
.cand-item.active .ann{color:#6ab8d8}
.cand-sep{color:var(--fg-faint);margin:0 2px}

.cand-meta{display:flex;align-items:center;gap:10px;font-size:9px;color:var(--fg-faint);padding:2px 0 0;font-family:monospace}
.cand-meta .k{color:var(--fg-dim)}
.cand-meta .k.hl-green{color:var(--green)}
.cand-meta .k.hl-blue{color:var(--blue)}
.cand-meta .pg{margin-left:auto}

/* ================= 底部状态栏 ================= */
.footer{
  height:26px;flex:0 0 26px;display:flex;align-items:center;gap:14px;
  padding:0 16px;font-size:10px;letter-spacing:.5px;
  border-top:1px solid var(--line);color:var(--fg-faint);
}
.footer .item{display:flex;align-items:center;gap:4px}
.footer .val{color:var(--fg-dim)}
.footer .val.alpha{color:var(--cyan)}
.footer .val.symbol{color:var(--purple)}
.footer .right{margin-left:auto;display:flex;gap:14px}

/* ================= 控件 ================= */
.controls{position:fixed;right:16px;bottom:16px;display:flex;flex-direction:column;align-items:flex-end;gap:6px;z-index:100}
.mode-buttons{display:flex;gap:4px;flex-wrap:wrap;justify-content:flex-end;max-width:520px}
.mode-btn{padding:5px 10px;background:#0e0e16;border:1px solid var(--line);color:var(--fg-dim);font-size:10px;font-family:inherit;cursor:pointer;transition:all .15s}
.mode-btn.active{background:rgba(74,184,230,.12);border-color:var(--blue);color:var(--blue)}
.mode-btn.symbol.active{background:rgba(168,85,247,.12);border-color:var(--purple);color:var(--purple)}
</style>
</head>
<body>

<div class="stage" id="stage">
  <div class="screen mode-alpha" id="screen">

    <div class="statusbar">
      <span class="sys">SYS.TERM</span>
      <span><span class="time" id="clock">20:20</span><span class="sep">·</span><span class="bat">BAT 100%</span></span>
    </div>

    <div class="main">
      <!-- 左手柄 -->
      <div class="panel left">
        <div class="shoulder-row">
          <div class="shoulder LB" id="btnLB">LB (MAGIC)</div>
          <div class="shoulder LT" id="btnLT">LT (SPACE)</div>
        </div>
        <div class="ptitle">// T9_KEYPAD</div>

        <div class="stick-wrap" data-stick="left">
          <!-- alpha 层 -->
          <div class="stick-layer alpha">
            <div class="key layer1 r1c1" data-idx="0"><span class="num">1</span><span class="txt">.,?</span></div>
            <div class="key layer1 r1c2" data-idx="1"><span class="num">2</span><span class="txt">ABC</span></div>
            <div class="key layer1 r1c3" data-idx="2"><span class="num">3</span><span class="txt">DEF</span></div>
            <div class="key layer1 r2c1" data-idx="3"><span class="num">4</span><span class="txt">GHI</span></div>
            <div class="key layer1 center-key active-popup r2c2" id="centerKey" data-idx="4"><span class="num">5</span><span class="txt">JKL</span></div>
            <div class="key layer1 r2c3" data-idx="5"><span class="num">6</span><span class="txt">MNO</span></div>
            <div class="key layer1 r3c1" data-idx="6"><span class="num">7</span><span class="txt">PQRS</span></div>
            <div class="key layer1 r3c2" data-idx="7"><span class="num">8</span><span class="txt">TUV</span></div>
            <div class="key layer1 r3c3" data-idx="8"><span class="num">9</span><span class="txt">WXYZ</span></div>
          </div>
          <!-- symbol 层 -->
          <div class="stick-layer symbol">
            <div class="key layer1 r1c1"><span class="num">+</span><span class="txt">-*</span></div>
            <div class="key layer1 r1c2"><span class="num">.</span><span class="txt">,?</span></div>
            <div class="key layer1 r1c3"><span class="num">:</span><span class="txt">;!</span></div>
            <div class="key layer1 r2c1"><span class="num">(</span><span class="txt">)[</span></div>
            <div class="key layer1 center-key r2c2"><span class="num">]</span><span class="txt">}{</span></div>
            <div class="key layer1 r2c3"><span class="num">/</span><span class="txt">=%</span></div>
            <div class="key layer1 r3c1"><span class="num">&lt;</span><span class="txt">&gt;|</span></div>
            <div class="key layer1 r3c2"><span class="num">\</span><span class="txt">_&amp;</span></div>
            <div class="key layer1 r3c3"><span class="num">~</span><span class="txt">^`</span></div>
          </div>
        </div>

        <div class="ptitle">// DPAD (MODS)</div>
        <div class="dpad">
          <div class="d blank"></div><div class="d tab" id="btnTab">Tab</div><div class="d blank"></div>
          <div class="d ctrl" id="btnCtrl">Ctrl</div><div class="d center">·</div><div class="d shift" id="btnShift">Shift</div>
          <div class="d blank"></div><div class="d alt" id="btnAlt">Alt</div><div class="d blank"></div>
        </div>

        <div class="klist">
          <div class="krow"><span class="kk purple">LB</span><span class="kd"><b>魔法键</b> 弹出/连击输入</span></div>
          <div class="krow"><span class="kk cyan">LT</span><span class="kd"><b>空格</b></span></div>
          <div class="krow"><span class="kk dim">DPAD</span><span class="kd">双击 <b>锁定 Ctrl/Shift/Alt</b></span></div>
        </div>
      </div>

      <!-- 终端 -->
      <div class="terminal">
        <div class="term-lines"><span class="prompt">switch@arch</span>:<span class="prompt-path">~</span>$ <span class="cmd">uname -a</span>
<span class="out">Linux switch 4.9.140-l4t aarch64 GNU/Linux</span>
<span class="prompt">switch@arch</span>:<span class="prompt-path">~</span>$ <span class="cmd">echo "你好，Switch"</span>
<span class="out">你好，Switch</span>
<span class="prompt">switch@arch</span>:<span class="prompt-path">~</span>$ <span class="cmd">sudo pacman -S --needed fcitx5</span>
<span class="out">:: 正在解析依赖关系...</span>
<span class="warn">:: 注意：Switch 内核未加载动态模块</span>
<span class="out">完成。</span></div>

        <div class="input-line"><span class="prompt">switch@arch</span>:<span class="prompt-path">~</span>$ <span id="composeArea"></span></div>

        <div class="cand-line" id="candLine"></div>
        <div class="cand-meta" id="candMeta">
          <span class="k hl-green">LB</span><span>输入</span>
          <span class="k">A</span><span>回车</span>
          <span class="k">B</span><span>退格</span>
          <span class="pg" id="pageText">1/3</span>
        </div>
      </div>

      <!-- 右手柄 -->
      <div class="panel right">
        <div class="shoulder-row">
          <div class="shoulder RB" id="btnRB">RB (L-CLICK)</div>
          <div class="shoulder RT" id="btnRT">RT (R-CLICK)</div>
        </div>
        <div class="ptitle">// MOUSE_CONTROL</div>

        <div class="stick-wrap" data-stick="right">
          <div class="stick-layer alpha">
            <div class="mouse-cross">╬</div>
          </div>
        </div>

        <div class="ptitle">// ABXY</div>
        <div class="abxy">
          <div class="b blank"></div><div class="b y" id="btnY">Y</div><div class="b blank"></div>
          <div class="b x" id="btnX">X</div><div class="b blank"></div><div class="b a" id="btnA">A</div>
          <div class="b blank"></div><div class="b b" id="btnB">B</div><div class="b blank"></div>
        </div>

        <div class="klist">
          <div class="krow"><span class="kk green">A</span><span class="kd"><b>回车</b></span> <span class="kk red" style="margin-left:6px">B</span><span class="kd"><b>退格</b></span></div>
          <div class="krow"><span class="kk blue">X</span><span class="kd"><b>Del</b></span> <span class="kk yellow" style="margin-left:8px">Y</span><span class="kd"><b>Esc</b></span></div>
          <div class="krow" style="margin-top:6px"><span class="kk green">RB</span><span class="kd"><b>鼠标左键</b></span></div>
          <div class="krow"><span class="kk red">RT</span><span class="kd"><b>鼠标右键</b></span></div>
        </div>
      </div>
    </div>

    <div class="footer">
      <div class="item"><span class="lbl">LAYER</span><span class="val alpha" id="fLayer">ALPHA</span></div>
      <div class="item"><span class="lbl">LANG</span><span class="val zh">ZH</span></div>
      <div class="item"><span class="lbl">IME</span><span class="val">T9</span></div>
      <div class="right">
        <span class="lbl" id="fMods" style="color:var(--fg-faint)">MODS —</span>
      </div>
    </div>

  </div>
</div>

<div class="controls">
  <div class="mode-buttons">
    <button class="mode-btn active" data-mode="alpha">字母层</button>
    <button class="mode-btn symbol" data-mode="symbol">符号层 (Shift)</button>
  </div>
</div>

<script>
const stage=document.getElementById('stage');
function fit(){const s=Math.min(innerWidth/1280,innerHeight/720);stage.style.transform=`scale(${s})`;}
addEventListener('resize',fit);fit();

setInterval(()=>{
  const d=new Date();
  document.getElementById('clock').textContent=String(d.getHours()).padStart(2,'0')+':'+String(d.getMinutes()).padStart(2,'0');
},1000);

const screen=document.getElementById('screen');
const modeBtns=document.querySelectorAll('.mode-btn');
const fLayer=document.getElementById('fLayer');
const fMods=document.getElementById('fMods');

let currentMode='alpha';
let modCtrl=false, modShift=false, modAlt=false;
let isPopup=false, currentFocusIdx=4, popupLetterIdx=0;

const candPages=[
  [{t:'你好',a:'hello'},{t:'你',a:'you'},{t:'尼',a:'ni'},{t:'呢',a:'ne'},{t:'泥',a:'mud'},{t:'拟',a:'draft'},{t:'逆',a:'reverse'}],
  [{t:'妮',a:'girl'},{t:'倪',a:'ni'},{t:'霓',a:'rainbow'},{t:'昵',a:'intimate'},{t:'溺',a:'drown'},{t:'腻',a:'greasy'},{t:'匿',a:'hide'}],
  [{t:'您',a:'formal'},{t:'柠',a:'lemon'},{t:'凝',a:'condense'},{t:'宁',a:'peaceful'},{t:'拧',a:'twist'},{t:'泞',a:'muddy'},{t:'佞',a:'flatter'}]
];
const candLine=document.getElementById('candLine');
const composeArea=document.getElementById('composeArea');
let composeText='', cursorPos=0;
let candPage=0;

function renderCompose(){
  const b=composeText.slice(0,cursorPos);
  const a=composeText.slice(cursorPos);
  composeArea.innerHTML=`<span class="compose-str">${b}</span><span class="cursor"></span><span class="compose-str">${a}</span>`;
}
function renderCandidates(){
  const words=candPages[candPage];
  let html='';
  words.forEach((w,i)=>{
    const active=i===0?' active':'';
    html+=`<span class="cand-item${active}"><span class="idx">${i+1}</span><span class="txt">${w.t}</span><span class="ann">${w.a}</span></span>`;
    if(i===4)html+=`<span class="cand-sep">│</span>`;
  });
  candLine.innerHTML=html;
}
function applyState(){
  screen.classList.toggle('mode-alpha', currentMode==='alpha');
  screen.classList.toggle('mode-symbol', currentMode==='symbol');
  screen.classList.toggle('is-popup', isPopup);
  
  screen.classList.toggle('mod-ctrl', modCtrl);
  screen.classList.toggle('mod-shift', modShift);
  screen.classList.toggle('mod-alt', modAlt);

  fLayer.textContent = currentMode==='alpha'?'ALPHA':'SYMBOL';
  fLayer.className='val '+(currentMode==='alpha'?'alpha':'symbol');
  
  let mods = [];
  if (modCtrl) mods.push('CTRL');
  if (modShift) mods.push('SHIFT');
  if (modAlt) mods.push('ALT');
  if (mods.length > 0) {
    fMods.textContent = `MODS [${mods.join('+')}]`;
    fMods.style.color = 'var(--yellow)';
  } else {
    fMods.textContent = 'MODS —';
    fMods.style.color = 'var(--fg-faint)';
  }
  
  modeBtns.forEach(b=>{
    b.classList.toggle('active', b.dataset.mode===currentMode);
  });
}

// 双击锁定
function bindDoubleClick(id, toggleFn) {
  let timer=null;
  document.getElementById(id).addEventListener('click',e=>{
    pressFeedback(e.currentTarget);
    if(timer){
      clearTimeout(timer); timer=null;
      toggleFn();
      applyState();
    }else{
      timer=setTimeout(()=>{timer=null;},280);
    }
  });
}

bindDoubleClick('btnCtrl', () => modCtrl = !modCtrl);
bindDoubleClick('btnShift',() => modShift = !modShift);
bindDoubleClick('btnAlt',  () => modAlt = !modAlt);
document.getElementById('btnTab').addEventListener('click',e=>{pressFeedback(e.currentTarget);});

// LB 魔法键
document.getElementById('btnLB').addEventListener('click',e=>{
  pressFeedback(e.currentTarget);
  if(!isPopup){
    isPopup=true;
    popupLetterIdx=0;
    document.querySelectorAll('.key.active-popup').forEach(x=>x.classList.remove('active-popup'));
    const activeKey = document.querySelector(`.stick-layer.${currentMode} .key[data-idx="${currentFocusIdx}"]`);
    if(activeKey) activeKey.classList.add('active-popup');
  }else{
    e.currentTarget.style.color='#fff';
    setTimeout(()=>e.currentTarget.style.color='',150);
  }
  applyState();
});

// 左摇杆
document.querySelectorAll('.panel.left .key').forEach(el=>{
  el.addEventListener('click',e=>{
    e.stopPropagation();pressFeedback(el);
    if(isPopup){
      const numStr = el.querySelector('.num').textContent;
      if(numStr >= '2' && numStr <= '9') {
        popupLetterIdx = (popupLetterIdx + 1) % 3;
        el.querySelector('.txt').textContent = `[${popupLetterIdx+1}]`;
        setTimeout(()=>{
          const original = el.getAttribute('data-orig-txt') || el.querySelector('.txt').textContent;
          el.querySelector('.txt').textContent = original;
        }, 300);
      }
    }else{
      currentFocusIdx = parseInt(el.dataset.idx);
      document.querySelectorAll('.key.active-popup').forEach(x=>x.classList.remove('active-popup'));
      el.classList.add('active-popup');
    }
  });
});

// 其他按键
document.getElementById('btnLT').addEventListener('click',e=>{pressFeedback(e.currentTarget);composeText+=" ";renderCompose();});
document.getElementById('btnA').addEventListener('click',e=>{pressFeedback(e.currentTarget);});
document.getElementById('btnB').addEventListener('click',e=>{pressFeedback(e.currentTarget);if(composeText.length>0){composeText=composeText.slice(0,-1);renderCompose();}});
document.getElementById('btnX').addEventListener('click',e=>{pressFeedback(e.currentTarget);});
document.getElementById('btnY').addEventListener('click',e=>{pressFeedback(e.currentTarget);});
document.getElementById('btnRB').addEventListener('click',e=>{pressFeedback(e.currentTarget);});
document.getElementById('btnRT').addEventListener('click',e=>{pressFeedback(e.currentTarget);});

function pressFeedback(el,dur=150){if(!el)return;el.classList.add('pressed');setTimeout(()=>el.classList.remove('pressed'),dur);}

modeBtns.forEach(btn=>{
  btn.addEventListener('click',()=>{
    currentMode=btn.dataset.mode;
    applyState();
  });
});

renderCompose();renderCandidates();applyState();
</script>
</body>
</html>
```