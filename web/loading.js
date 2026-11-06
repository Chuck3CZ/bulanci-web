// Loading overlay for the game's page (/test/hostcheck.html).  The page logs milestones into its <pre> while the game
// starts (compile, threads, WebGPU, the game's own graphics device); this shows them with a ring, the time and the
// log's last line, until the game draws its first picture.  A classic script that runs before the page's module.
// Only for the player's start (?gpu); the theme (?theme=cmr) only picks the colours.  Switches (the start page's
// "Advanced" section): ?showlog the log under the game, ?logpost the log to the server, ?noload none of this, ?nocache no module cache.
//
// It also wraps fetch() for the game module (?wasm=): the module goes into the Cache Storage (the HTTP cache refuses a
// file this big), keyed by its ETag, so the page shows the real download and a later start reads it from the browser;
// the start page's loading step fills the same cache.
(() => {
  const q = new URLSearchParams(location.search);
  if (!q.has('gpu')) return;
  // the log under the game (the <details> the page makes for it) is for whoever debugs: hidden unless ?showlog
  if (!q.has('showlog')) { const hide = document.createElement('style'); hide.textContent = 'details{display:none!important}'; document.documentElement.append(hide); }
  if (q.has('noload')) return;     // ?noload: the page as it was before this script (to tell whether a freeze is the game's own)
  // ?logpost: the overlay also reports to the server's log (/log, appended to the page's own lines): the browser, the
  // module's download and compile, and a heartbeat that shows whether the page's main thread is alive or blocked
  const posting = q.has('logpost');
  const rq = [], diagLines = [];
  let diagEl = null, diagShown = 0;
  const rep = s => { if (posting) { const l = '[overlay] +' + Math.round(performance.now() / 100) / 10 + 's ' + s; rq.push(l); diagLines.push(l.slice(10)); } };
  // the page's own first post starts the log file over ("@@start"), so ours begin after it
  if (posting) setTimeout(() => setInterval(() => { if (rq.length) { const b = rq.splice(0).join('\n') + '\n'; try { fetch('/log', { method: 'POST', body: b, keepalive: true }).catch(() => {}); } catch {} } }, 1000), 1500);
  let cs = true;
  try { cs = (localStorage.getItem('landing-lang') || navigator.language || 'cs').slice(0, 2) !== 'en'; } catch {}
  const th = q.get('theme') === 'cmr' ? { a: '#ff6a1a', bg: '#0e0f12' } : { a: '#d9a441', bg: '#0b0e16' };
  const T = cs
    ? { h: 'Hra se spouští', s: 'Nech tuto kartu otevřenou v popředí: skrytá nebo neaktivní karta hru pozastaví. Nejdéle trvá první spuštění.', hide: 'Skrýt', fail: 'Spuštění se nezdařilo', failsub: 'Podrobnosti jsou v logu pod hrou. Zkus obnovit stránku (F5).',
        click: 'Klikni myší do okna hry. Bez toho se hra bere jako neaktivní (Windows inactive).', frames: 'snímků', draws: 'kreslí',
        slow: 'Kompilace velkého modulu v prohlížeči může trvat desítky sekund. Stránka nezamrzla, čas běží.', cached: 'z paměti prohlížeče', mb: 'MB',
        steps: ['Stáhnout modul hry', 'Zkompilovat modul', 'Spustit vlákna hry', 'Připravit grafiku (WebGPU)', 'Hra vytváří grafické zařízení', 'Čekám na první obraz'] }
    : { h: 'Starting the game', s: 'Keep this tab open in the foreground: a hidden or inactive tab pauses the game. The first start takes the longest.', hide: 'Hide', fail: 'The start failed', failsub: 'Details are in the log under the game. Try reloading (F5).',
        click: 'Click the game window with the mouse. Without it the game counts as inactive (Windows inactive).', frames: 'frames', draws: 'drawing',
        slow: 'Compiling a big module in the browser can take tens of seconds. The page has not frozen, the time keeps running.', cached: 'from the browser', mb: 'MB',
        steps: ['Download the game module', 'Compile the module', 'Start the game threads', 'Set up graphics (WebGPU)', 'The game creates its graphics device', 'Waiting for the first picture'] };
  // log lines that finish steps 1..4 (step 0, the download, is finished by the fetch below; step 5 by the picture)
  const RE = [null, /^compiled in/, /^\[thread\] start tid=|^\[pool\] \d+ game workers started/, /^\[gpu\] renderer attached/, /CreateDevice/];
  // The game is "on" when it has drawn and shown a picture.  Counters only, nothing is read back: the first draw call
  // (the hooks then remove themselves) and the frames the renderer takes from the canvas after it (getCurrentTexture).
  const gpuN = { drawn: false, framesAtDraw: 0, frames: 0 };
  const undo = [];
  try {
    const P = window.GPURenderPassEncoder && GPURenderPassEncoder.prototype;
    if (P) for (const k of ['draw', 'drawIndexed', 'drawIndirect', 'drawIndexedIndirect']) {
      const o = P[k]; if (!o) continue;
      const restore = () => { if (P[k] !== o) P[k] = o; };
      P[k] = function (a, b, c, d, e) { if (!gpuN.drawn) { gpuN.drawn = true; gpuN.framesAtDraw = gpuN.frames; for (const u of undo) if (u.draw) u.fn(); } return o.call(this, a, b, c, d, e); };
      undo.push({ draw: true, fn: restore });
    }
    const C = window.GPUCanvasContext && GPUCanvasContext.prototype;
    if (C && C.getCurrentTexture) { const g = C.getCurrentTexture; C.getCurrentTexture = function () { gpuN.frames++; return g.call(this); }; undo.push({ fn: () => { C.getCurrentTexture = g; } }); }
  } catch {}
  const unhook = () => { for (const u of undo) { try { u.fn(); } catch {} } undo.length = 0; };
  const css = document.createElement('style');
  css.textContent = `.ldg{position:fixed;inset:0;z-index:2147483000;display:grid;place-items:center;padding:16px;background:${th.bg};color:#e8e6df;font:16px/1.5 system-ui,sans-serif;transition:opacity .4s;pointer-events:none}
.ldg.out{opacity:0;pointer-events:none}.ldg *{box-sizing:border-box}.ldg .b{width:100%;max-width:460px;text-align:center;pointer-events:auto}.ldg .f{margin:0 0 14px;padding:8px 12px;border:1px solid ${th.a};border-radius:8px;color:#e8e6df;font-size:14px}.ldg-bn{position:fixed;left:50%;bottom:24px;transform:translateX(-50%);z-index:2147483000;max-width:90vw;padding:10px 16px;border-radius:10px;background:rgba(20,22,28,.92);border:1px solid ${th.a};color:#e8e6df;font:14px/1.4 system-ui,sans-serif;pointer-events:none;text-align:center}
.ldg .r{position:relative;width:140px;height:140px;margin:0 auto 18px}.ldg svg{width:100%;height:100%}.ldg .r.i svg{animation:ldgs 1.1s linear infinite}
.ldg .t{fill:none;stroke:#2a3040;stroke-width:7}.ldg .a{fill:none;stroke:${th.a};stroke-width:7;stroke-linecap:round;stroke-dasharray:276.46;stroke-dashoffset:190;filter:drop-shadow(0 0 6px ${th.a});transition:stroke-dashoffset .25s}
.ldg .r:not(.i) svg{transform:rotate(-90deg)}
.ldg .n{position:absolute;inset:0;display:grid;place-items:center;font-size:26px;font-weight:800;font-variant-numeric:tabular-nums}
.ldg h3{margin:0 0 4px;font-size:22px}.ldg .s{margin:0 0 16px;color:#9a9890;font-size:14px;min-height:2.8em}
.ldg ul{list-style:none;margin:0 0 14px;padding:0;text-align:left;display:grid;gap:6px;font-size:14.5px}.ldg li{display:grid;grid-template-columns:20px 1fr auto;gap:8px;color:#9a9890}
.ldg li span:last-child{font-variant-numeric:tabular-nums}.ldg li:before{content:"○";text-align:center}.ldg li.c{color:#e8e6df}.ldg li.c:before{content:"◔";color:${th.a}}.ldg li.d{color:#e8e6df}.ldg li.d:before{content:"✓";color:#6fcf8e;font-weight:800}
.ldg .l{margin:0 0 16px;height:1.4em;overflow:hidden;white-space:nowrap;text-overflow:ellipsis;color:#6b6a64;font:12px ui-monospace,Menlo,monospace}
.ldg button{padding:9px 20px;color:#e8e6df;background:transparent;border:1px solid #2a3040;border-radius:8px;font:inherit;cursor:pointer}.ldg button:hover{border-color:${th.a}}
@keyframes ldgs{to{transform:rotate(360deg)}}@media(prefers-reduced-motion:reduce){.ldg .r.i svg{animation:none}}`;
  const root = document.createElement('div');
  root.className = 'ldg'; root.setAttribute('role', 'status');
  root.innerHTML = `<div class="b"><div class="r i"><svg viewBox="0 0 100 100" aria-hidden="true"><circle class="t" cx="50" cy="50" r="44"/><circle class="a" cx="50" cy="50" r="44"/></svg><div class="n">0 s</div></div>
<h3></h3><p class="s"></p><ul></ul><p class="l" aria-hidden="true"></p><button type="button"></button></div>`;
  const $ = s => root.querySelector(s);
  $('h3').textContent = T.h; $('.s').textContent = T.s; $('button').textContent = T.hide;
  const hint = document.createElement('p'); hint.className = 'f'; hint.textContent = T.click; hint.hidden = true; $('.b').insertBefore(hint, $('.l'));
  const ul = $('ul'), ring = $('.r'), arc = $('.a'), num = $('.n');
  const lis = T.steps.map(x => { const li = document.createElement('li'); li.append(document.createElement('span'), document.createElement('span')); li.firstChild.textContent = x; ul.append(li); return li; });
  const detail = (i, s) => { lis[i].lastChild.textContent = s; };
  if (posting) {
    diagEl = document.createElement('pre');
    diagEl.style.cssText = 'margin:14px 0 0;padding:8px 10px;max-height:9.5em;overflow:auto;text-align:left;white-space:pre-wrap;word-break:break-all;font:11px/1.35 ui-monospace,Menlo,monospace;color:#9a9890;background:rgba(0,0,0,.35);border:1px solid #2a3040;border-radius:8px';
    $('.b').append(diagEl);
  }
  document.documentElement.append(css, root);

  const done = new Array(lis.length).fill(false);
  const t0 = performance.now();
  let mo = null, timers = [], finished = false, dlActive = false, compileFrom = 0;
  const paint = () => { const cur = done.indexOf(false); lis.forEach((li, i) => { li.className = done[i] ? 'd' : i === cur ? 'c' : ''; }); };
  const finish = () => { if (finished) return; finished = true; if (mo) mo.disconnect(); afterFinish(); timers.forEach(clearInterval); timers.forEach(clearTimeout); root.classList.add('out'); setTimeout(() => root.remove(), 600); };
  const failed = () => { timers.forEach(clearInterval); ring.classList.remove('i'); arc.style.strokeDashoffset = '276.46'; num.textContent = '✕'; $('h3').textContent = T.fail; $('.s').textContent = T.failsub; };
  function afterFinish() {
    unhook();
    try { window.focus(); const c = document.querySelector('canvas'); if (c) { if (c.tabIndex < 0) c.tabIndex = 0; c.focus({ preventScroll: true }); } } catch {}
    if (!document.hasFocus()) {
      const bn = document.createElement('div'); bn.className = 'ldg-bn'; bn.textContent = T.click; document.documentElement.append(bn);
      const chk = setInterval(() => { if (document.hasFocus()) { clearInterval(chk); bn.remove(); } }, 400);
      setTimeout(() => { clearInterval(chk); bn.remove(); }, 60000);
    }
  }
  const setDone = i => { if (done[i]) return; done[i] = true; paint(); if (i === 4) timers.push(setTimeout(finish, 300000)); if (i === 0) { dlActive = false; ring.classList.add('i'); compileFrom = performance.now(); } if (i >= 3) arm(); };
  paint();
  // the clock: the download shows its percentage in the ring, everything else the seconds since the page opened
  timers.push(setInterval(() => {
    if (finished) return;
    const s = Math.round((performance.now() - t0) / 1000);
    if (!dlActive) num.textContent = s + ' s';
    if (done[0] && !done[1]) {
      const c = Math.round((performance.now() - compileFrom) / 1000);
      detail(1, c + ' s');
      if (c >= 8) $('.s').textContent = T.slow;
    }
  }, 250));

  // ---- the game module through the Cache Storage ----
  const wasmPath = (() => { try { return new URL(q.get('wasm') ?? '', location.href).pathname; } catch { return ''; } })();
  const realFetch = window.fetch.bind(window);
  const mbs = n => (n / 1048576).toFixed(1);
  const progress = (got, total) => {
    dlActive = true; ring.classList.remove('i');
    if (total) { arc.style.strokeDashoffset = String(276.46 * (1 - Math.min(1, got / total))); num.textContent = Math.round(got / total * 100) + ' %'; detail(0, mbs(got) + ' / ' + mbs(total) + ' ' + T.mb); }
    else detail(0, mbs(got) + ' ' + T.mb);
  };
  async function wasmFetch(input, init) {
    let etag = '', total = 0;
    try { const h = await realFetch(input, { method: 'HEAD', signal: init?.signal }); etag = h.headers.get('etag') || h.headers.get('last-modified') || ''; total = +h.headers.get('content-length') || 0; } catch {}
    let cache = null; try { cache = await caches.open('wasm-v1'); } catch {}
    const key = etag ? wasmPath + '?v=' + encodeURIComponent(etag) : '';
    if (cache && key) { try { const hit = await cache.match(key); if (hit) { rep('wasm: cache hit ' + key); detail(0, T.cached); setDone(0); return hit; } } catch (e) { rep('wasm: cache.match failed ' + e); } }
    rep('wasm: downloading, etag=' + etag + ' total=' + total + ' cache=' + !!cache);
    const r = await realFetch(input, init);
    if (!r.ok || !r.body) { setDone(0); return r; }
    const len = total || +r.headers.get('content-length') || 0;
    let body = r.body, put = null;
    if (cache && key) { const [a, b] = body.tee(); body = a; put = cache.put(key, new Response(b, { status: 200, headers: r.headers })).catch(() => {}); }
    let got = 0;
    const counted = body.pipeThrough(new TransformStream({
      transform(chunk, ctl) { got += chunk.length; progress(got, len); ctl.enqueue(chunk); },
      flush() { rep('wasm: download done ' + got + ' bytes'); detail(0, mbs(got) + ' ' + T.mb); setDone(0); },
    }));
    if (put) put.then(async () => { try { for (const k of await cache.keys()) if (new URL(k.url).pathname === wasmPath && !k.url.endsWith(key)) await cache.delete(k); } catch {} });
    return new Response(counted, { status: 200, headers: r.headers });
  }
  if (!q.has('nocache')) window.fetch = function (input, init) {       // ?nocache: no Cache Storage / fetch wrapper
    let p = ''; try { p = new URL(typeof input === 'string' ? input : input.url, location.href).pathname; } catch {}
    return p && p === wasmPath && (!init || !init.method || init.method === 'GET') ? wasmFetch(input, init) : realFetch(input, init);
  };

  // ---- hiding it: no reading of the game's canvas (a readback of a WebGPU canvas costs the GPU and the page's memory
  // and stalled the page); a few seconds after the game made its graphics device it has begun to draw, and the Hide
  // button is always there ----
  let fallbackSet = false;
  function arm() {
    if (fallbackSet) return; fallbackSet = true;
    // never keep the overlay over a running game
    timers.push(setTimeout(finish, 600000));
  }
  let lastTxt = '', shownTxt = '';
  const gpuSeen = { drawn: false, frames: 0 };
  const pendingRe = () => RE.some((re, i) => re && !done[i]);
  timers.push(setInterval(() => {
    if (finished) return;
    if (lastTxt !== shownTxt) { shownTxt = lastTxt; $('.l').textContent = lastTxt.slice(0, 120); }
    hint.hidden = document.hasFocus() || !done[2];
    if (done[4] && !done[5]) {
      detail(5, gpuN.frames + ' ' + T.frames + (gpuN.drawn ? ', ' + T.draws : ''));
      if (gpuN.drawn !== gpuSeen.drawn || (gpuN.frames > 0 && gpuSeen.frames === 0)) { gpuSeen.drawn = gpuN.drawn; gpuSeen.frames = gpuN.frames; rep('gpu: drawn=' + gpuN.drawn + ' frames=' + gpuN.frames); }
      if (gpuN.drawn && gpuN.frames - gpuN.framesAtDraw >= 30 && !document.hidden) { rep('first picture: frames=' + gpuN.frames); done[5] = true; paint(); finish(); return; }
    }
    if (diagEl && diagLines.length !== diagShown) { diagShown = diagLines.length; diagEl.textContent = diagLines.slice(-8).join('\n'); diagEl.scrollTop = diagEl.scrollHeight; }
  }, 300));
  const line = s => {
    const m = /^\[\d+!?\] (.*)$/.exec(s); const txt = (m ? m[1] : s).trim();
    if (txt) lastTxt = txt;
    if (!pendingRe()) { if (!/^\[page\] no renderer|^EXIT /.test(txt)) return; }
    RE.forEach((re, i) => { if (re && !done[i] && re.test(txt)) { rep('milestone ' + i + ': ' + txt.slice(0, 80)); if (i === 1) { setDone(0); detail(1, Math.round((performance.now() - compileFrom) / 1000) + ' s'); } setDone(i); } });
    if (/^\[page\] no renderer|^EXIT /.test(txt)) failed();
  };
  // an error the page throws (a module that does not compile, a worker that does not start, a fetch that fails) never
  // reaches the log; without this the overlay would count seconds over a page that has already given up
  const oops = (what, e) => {
    const msg = String((e && (e.message || e.reason?.message || e.reason)) || what).slice(0, 300);
    if (/wasm|webassembly|compile|link|worker|sharedarraybuffer|memory|fetch|network|instantiat/i.test(msg + ' ' + (e && e.error && e.error.name))) { failed(); $('.s').textContent = T.failsub; }
    rep(what + ': ' + msg);
    $('.l').textContent = msg; $('.l').style.whiteSpace = 'normal'; $('.l').style.height = 'auto'; $('.l').style.color = '#ff7a6b';
  };
  addEventListener('error', e => oops('error', e));
  addEventListener('unhandledrejection', e => oops('rejection', e));
  // the page's log is a <pre> that gets one text node per line
  mo = new MutationObserver(ms => { for (const m of ms) for (const n of m.addedNodes) if (n.nodeType === 3) for (const l of n.data.split('\n')) if (l) line(l); });
  mo.observe(document.documentElement, { childList: true, subtree: true });
  $('button').addEventListener('click', finish);
  if (posting) {
    rep(`start ${navigator.userAgent} | isolated=${crossOriginIsolated} gpu=${!!navigator.gpu} mem=${navigator.deviceMemory ?? '?'}GB cores=${navigator.hardwareConcurrency}`);
    let last = performance.now();
    timers.push(setInterval(() => {
      const now = performance.now(), lag = Math.round(now - last - 3000); last = now;
      const heap = performance.memory ? Math.round(performance.memory.usedJSHeapSize / 1048576) + 'MB' : '?';
      rep(`beat lag=${lag}ms heap=${heap} steps=${done.map(d => (d ? 1 : 0)).join('')} hidden=${document.hidden}`);
    }, 3000));
  }
  addEventListener('keydown', e => { if (e.key === 'Escape' && !finished) finish(); });
})();
