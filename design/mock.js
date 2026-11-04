/* mockup 公共行为：40 键（每手 5×4）+ 3 列滑窗 + 吸附/组字状态。四版共用一套，对比的只是视觉。 */
(() => {
  // 数据照 config/ui.lua：左手 20 键 + 右手 20 键，行=真实 108 键盘。'' = 该层空键（不画也不吸）
  // 每格 [主字符, 上档/副字符]
  const DATA = {
    alpha: {
      left: [
        [['1', '!'], ['2', '@'], ['3', '#'], ['4', '$'], ['5', '%']],
        [['q', ''], ['w', ''], ['e', ''], ['r', ''], ['t', '']],
        [['a', ''], ['s', ''], ['d', ''], ['f', ''], ['g', '']],
        [['z', ''], ['x', ''], ['c', ''], ['v', ''], ['b', '']],
      ],
      right: [
        [['6', '^'], ['7', '&'], ['8', '*'], ['9', '('], ['0', ')']],
        [['y', ''], ['u', ''], ['i', ''], ['o', ''], ['p', '']],
        [['h', ''], ['j', ''], ['k', ''], ['l', ''], [';', ':']],
        [['n', ''], ['m', ''], [',', '<'], ['.', '>'], ['/', '?']],
      ],
    },
    num: {
      left: [
        [['', ''], ['', ''], ['', ''], ['', ''], ['', '']],
        [['', ''], ['', ''], ['', ''], ['', ''], ['', '']],
        [['', ''], ['、', ''], ['【', ''], ['】', ''], ['', '']],
        [['', ''], ['「', ''], ['」', ''], ['', ''], ['', '']],
      ],
      right: [
        [['', ''], ['', ''], ['', ''], ['', ''], ['', '']],
        [['', ''], ['', ''], ['', ''], ['', ''], ['', '']],
        [['', ''], ['-', '_'], ['=', '+'], ['[', '{'], ['', '']],
        [['', ''], [']', '}'], ['\\', '|'], ["'", '"'], ['', '']],
      ],
    },
  };

  const PAGES = [
    [['你好', 'nǐ hǎo'], ['你', 'nǐ'], ['尼', 'ní'], ['呢', 'ne'], ['泥', 'ní'], ['拟', 'nǐ'], ['逆', 'nì']],
    [['妮', 'nī'], ['倪', 'ní'], ['霓', 'ní'], ['昵', 'nì'], ['溺', 'nì'], ['腻', 'nì'], ['匿', 'nì']],
    [['您', 'nín'], ['柠', 'níng'], ['凝', 'níng'], ['宁', 'níng'], ['拧', 'nǐng'], ['泞', 'nìng'], ['佞', 'nìng']],
  ];

  if (location.hash.includes('embed')) document.body.classList.add('embed');
  document.body.classList.add('live');

  const S = {
    layer: 'alpha',
    snapL: { r: 2, c: 2 },
    snapR: { r: 2, c: 2 },
    snapping: true,
    active: 'left',
    lang: 'zh',
    shift: false,
    ctrl: false,
    alt: false,
    noCand: false,
    page: 0,
    sel: 0,
    compose: 'nihao',
    caret: 5,
    typed: '',
    tab: 0,
  };

  const screen = document.querySelector('.screen');
  const $ = s => screen.querySelector(s);
  const $$ = s => [...screen.querySelectorAll(s)];
  const esc = s => s.replace(/[&<>]/g, m => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[m]));

  // 每手 3 列 × 4 行窗口；列跟着吸附键滑动（吸附键恒在窗口中间那列，除非顶到边界）
  const WIN = { cols: 3, rows: 4 };
  const PITCH_X = 76, PITCH_Y = 82, MASK_IN = 50, MASK_OUT = 230;

  const pads = $$('.pad');
  pads.forEach(pad => {
    for (let r = 0; r < WIN.rows; r++) {
      for (let v = 0; v < WIN.cols; v++) {
        const b = document.createElement('button');
        b.type = 'button';
        b.className = 'key';
        b.style.gridRow = String(r + 1);
        b.style.gridColumn = String(v + 1);
        b.dataset.r = r;
        b.dataset.v = v;
        b.innerHTML = '<span class="m"></span><span class="s"></span>';
        pad.appendChild(b);
      }
    }
  });

  const cell = (hand, r, c) => (DATA[S.layer][hand][r] || [])[c] || ['', ''];
  const isEmpty = (hand, r, c) => cell(hand, r, c)[0] === '';

  // 切层后吸附键变空：挪到窗口里最近的非空键（空键不吸）
  function settleSnap(hand) {
    const s = hand === 'left' ? S.snapL : S.snapR;
    if (!isEmpty(hand, s.r, s.c)) return;
    let best = null, bd = 1e9;
    for (let r = 0; r < 4; r++) {
      for (let c = 0; c < 5; c++) {
        if (isEmpty(hand, r, c)) continue;
        const d = Math.hypot((c - s.c) * PITCH_X, (r - s.r) * PITCH_Y);
        if (d < bd) { bd = d; best = { r, c }; }
      }
    }
    if (best) { if (hand === 'left') S.snapL = best; else S.snapR = best; }
  }

  function renderKeys() {
    pads.forEach(pad => {
      const hand = pad.dataset.hand;
      const s = hand === 'left' ? S.snapL : S.snapR;
      const start = Math.min(Math.max(s.c - 1, 0), 5 - WIN.cols);
      const focused = !S.snapping;
      const baseR = s.r, baseC = focused ? start + 1 : s.c;
      pad.querySelectorAll('.key').forEach(b => {
        const r = +b.dataset.r, v = +b.dataset.v;
        const c = start + v;
        const [main, sub] = cell(hand, r, c);
        b.querySelector('.m').textContent = main;
        b.querySelector('.s').textContent = sub;
        b.hidden = main === '';
        const snapped = S.snapping && r === s.r && c === s.c && main !== '';
        const hovered = focused && r === baseR && c === start + 1 && main !== '';
        b.classList.toggle('snap', snapped);
        b.classList.toggle('hover', hovered && !snapped);
        b.dataset.key = main;
        b.dataset.hand = hand;
        b.dataset.rr = r;
        b.dataset.cc = c;
        const d = Math.hypot((c - baseC) * PITCH_X, (r - baseR) * PITCH_Y);
        const fade = 1 - Math.min(Math.max((d - MASK_IN) / (MASK_OUT - MASK_IN), 0), 1);
        const shrink = snapped || hovered ? (snapped ? 1.09 : 1.0)
          : 1 - 0.3 * Math.min(Math.max((d - 45) / 135, 0), 1);
        b.style.setProperty('--fade', fade.toFixed(3));
        b.style.setProperty('--shrink', shrink.toFixed(3));
      });
    });
  }

  function render() {
    screen.dataset.side = S.active;
    screen.classList.toggle('layer-num', S.layer === 'num');
    screen.classList.toggle('zh', S.lang === 'zh');
    screen.classList.toggle('nograb', !S.snapping);
    screen.classList.toggle('nocand', S.noCand || !S.compose.length);
    screen.classList.toggle('shifted', S.shift);
    screen.classList.toggle('ctrled', S.ctrl);
    screen.classList.toggle('alted', S.alt);
    renderKeys();

    const c = $('[data-compose]');
    if (c) {
      c.innerHTML = `<span class="cmp">${esc(S.compose.slice(0, S.caret))}</span>`
        + `<i class="caret"></i><span class="cmp rest">${esc(S.compose.slice(S.caret))}</span>`;
    }
    const typed = $('[data-typed]');
    if (typed) typed.textContent = S.typed;

    const box = $('[data-cands]');
    if (box) {
      box.innerHTML = PAGES[S.page].map(([w, p], i) =>
        `<span class="cand${i === S.sel ? ' on' : ''}"><span class="ci">${i + 1}</span>`
        + `<span class="ct">${w}</span><span class="cp">${p}</span></span>`).join('');
    }
    const set = (sel, v) => { const e = $(sel); if (e) e.textContent = v; };
    set('[data-page]', `${S.page + 1}/${PAGES.length}`);
    set('[data-f-layer]', S.layer === 'alpha' ? 'ALPHA' : 'NUM');
    set('[data-f-lang]', S.lang === 'zh' ? 'ZH' : 'EN');
    const mods = [S.ctrl && 'CTRL', S.shift && 'SHIFT', S.alt && 'ALT'].filter(Boolean).join('+');
    set('[data-f-mods]', mods || '—');
    const fl = $('[data-f-lang]');
    if (fl) fl.classList.toggle('on', S.lang === 'zh');

    $$('[data-act="shift"]').forEach(b => b.classList.toggle('on', S.shift));
    $$('[data-act="ctrl"]').forEach(b => b.classList.toggle('on', S.ctrl));
    $$('[data-act="alt"]').forEach(b => b.classList.toggle('on', S.alt));
    $$('.panel[data-side]').forEach(p => p.classList.toggle('active', p.dataset.side === S.active));

    $$('[data-hud]').forEach(b => {
      const a = b.dataset.hud;
      if (a === 'layer') b.textContent = '层 ' + (S.layer === 'alpha' ? 'ALPHA' : 'NUM');
      else b.classList.toggle('on',
        a === 'nograb' ? !S.snapping : a === 'caps' ? S.shift : a === 'nocand' ? S.noCand
          : a === 'lang' ? S.lang === 'en' : a === 'side' ? S.active === 'right' : false);
    });
  }

  const flash = el => { el.classList.remove('hit'); void el.offsetWidth; el.classList.add('hit'); };

  function type(ch) {
    if (!ch) return;
    S.compose = S.compose.slice(0, S.caret) + ch + S.compose.slice(S.caret);
    S.caret += ch.length;
    S.noCand = false;
  }

  function act(a) {
    switch (a) {
      case 'layer':
        S.layer = S.layer === 'alpha' ? 'num' : 'alpha';
        settleSnap('left'); settleSnap('right');
        break;
      case 'commit':  // LB：上屏吸附键（Shift 取上档）
        {
          const hand = S.active;
          const s = hand === 'left' ? S.snapL : S.snapR;
          if (!S.snapping) break;
          const [m, sub] = cell(hand, s.r, s.c);
          if (m) type(S.shift && sub ? sub : m);
          S.shift = false;
        }
        break;
      case 'space':
        if (S.compose.length) {
          const w = PAGES[S.page][S.sel][0];
          S.typed += w; S.compose = ''; S.caret = 0; S.noCand = true; S.sel = 0; S.page = 0;
        } else S.typed += ' ';
        break;
      case 'tab': if (S.compose.length) S.page = (S.page + 1) % PAGES.length; break;
      case 'cursor-l': if (S.compose.length) S.sel = (S.sel + 6) % 7; break;
      case 'cursor-r': if (S.compose.length) S.sel = (S.sel + 1) % 7; break;
      case 'enter': S.typed += '\n'; S.compose = ''; S.caret = 0; S.noCand = true; break;
      case 'backspace': if (S.compose.length) { S.compose = S.compose.slice(0, -1); S.caret = Math.min(S.caret, S.compose.length); } else S.typed = S.typed.slice(0, -1); break;
      case 'delete': break;
      case 'esc': S.compose = ''; S.caret = 0; S.noCand = true; break;
      case 'shift': S.shift = !S.shift; break;
      case 'ctrl': S.ctrl = !S.ctrl; break;
      case 'alt': S.alt = !S.alt; break;
    }
    render();
  }

  let shiftTimer = null;
  screen.addEventListener('click', e => {
    const key = e.target.closest('.pad .key');
    if (key && key.dataset.key) {
      flash(key);
      const hand = key.dataset.hand;
      const s = { r: +key.dataset.rr, c: +key.dataset.cc };
      if (hand === 'left') S.snapL = s; else S.snapR = s;
      S.snapping = true;
      S.active = hand;
      render();
      return;
    }
    const b = e.target.closest('[data-act]');
    if (b) {
      flash(b);
      if (b.dataset.act === 'shift') {
        if (shiftTimer) { clearTimeout(shiftTimer); shiftTimer = null; act('shift'); }
        else shiftTimer = setTimeout(() => { shiftTimer = null; }, 300);
      } else act(b.dataset.act);
      return;
    }
    const cand = e.target.closest('.cand');
    if (cand) {
      S.sel = [...cand.parentNode.children].indexOf(cand);
      render();
      return;
    }
    const tab = e.target.closest('.tab');
    if (tab && !tab.classList.contains('plus')) {
      $$('.tab').forEach(t => t.classList.toggle('on', t === tab));
      return;
    }
  });

  document.querySelectorAll('[data-hud]').forEach(b => {
    b.addEventListener('click', () => {
      const a = b.dataset.hud;
      if (a === 'layer') act('layer');
      else if (a === 'nograb') { S.snapping = !S.snapping; render(); }
      else if (a === 'caps') { S.shift = !S.shift; render(); }
      else if (a === 'nocand') { S.noCand = !S.noCand; render(); }
      else if (a === 'lang') { S.lang = S.lang === 'zh' ? 'en' : 'zh'; render(); }
      else if (a === 'side') { S.active = S.active === 'left' ? 'right' : 'left'; render(); }
    });
  });

  // #nofit：截图用，舞台保持 1:1（不然 headless 的可用高度会把整页缩小）
  const stage = document.querySelector('.stage');
  if (!location.hash.includes('nofit')) {
    const fit = () => { stage.style.transform = `scale(${Math.min(innerWidth / 1280, innerHeight / 720)})`; };
    addEventListener('resize', fit);
    fit();
  }

  const clock = () => {
    const d = new Date();
    const t = String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');
    document.querySelectorAll('[data-clock]').forEach(e => e.textContent = t);
  };
  clock();
  setInterval(clock, 5000);

  render();
})();
