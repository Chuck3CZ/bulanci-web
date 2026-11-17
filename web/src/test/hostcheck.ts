// Host self-test page: runs a test module (?wasm=...) through the real host
// (pool, I/O worker, mounts) and reports its output; title becomes DONE.
import { startProcess } from '../host/process';
import { attachNet } from '../net/index';
import { attachRenderer } from '../gpu/index';
import { attachInput } from '../input/index';
import { attachTouchKeys, touchWanted } from '../app/touchkeys';
import { attachAudio } from '../audio/index';
import { startSaveSync, type SaveSync } from '../host/savesync';
// the short address /hra/<name>: its parameters set by the page's first script (build.mjs BOOT); without them it is on its way back
if ((window as any).__redir) await new Promise(() => {});
const q: URLSearchParams = (window as any).__q ?? new URLSearchParams(location.search);
const gpu = q.has('gpu');
// the game's window size (WC3_WIDTH/HEIGHT, runtime/web/null_platform.c), its picture's resolution: ?res=WxH,
// by default the screen's (CSS pixels, so a fullscreen picture is not scaled; device pixels cost the GPU too much)
const resQ = /^(\d{3,4})x(\d{3,4})$/.exec(q.get('res') ?? '');
const resW = (resQ ? +resQ[1] : Math.max(1280, screen.width)) & ~1, resH = (resQ ? +resQ[2] : Math.max(720, screen.height)) & ~1;
const canvas = document.createElement('canvas');
const fs = document.createElement('button');
// the canvas in a box of its own: the touch controls lie over it, and in touch mode the box goes full screen
const wrap = document.createElement('div');
const touch = gpu && touchWanted(q);
// ?net=<name>: the player; the page tells the server it is here (POST /presence) and lists who else is, beside the game
const me = q.get('net') ?? '';
const hubPanel = !!(window as any).__hubPanel;       // the hub's panel (build.mjs HUBPANEL) lists the players instead
const who = document.createElement('aside');
// the progress on its way to the server (savesync.ts), beside the fullscreen button
const syncNote = document.createElement('span'); syncNote.style.cssText = 'margin-left:14px;font:13px system-ui,sans-serif;color:#8a8';
if (gpu) { canvas.width = resW; canvas.height = resH; canvas.style.cssText = 'display:block;width:1280px;max-width:100%;background:#000';
           wrap.style.cssText = 'position:relative;width:max-content;max-width:100%;flex:0 1 auto;min-width:0';
           wrap.append(canvas);
           const row = document.createElement('div'); row.style.cssText = 'display:flex;gap:12px;align-items:flex-start;flex-wrap:wrap';
           who.style.cssText = 'flex:0 0 170px;font:13px system-ui,sans-serif;color:#ccc;background:#15171c;border:1px solid #2a2e36;border-radius:6px;padding:8px 10px';
           row.append(wrap); if (me && !hubPanel) row.append(who); document.body.append(row);
           // full screen in touch mode: the box (buttons included), the picture fitted, landscape on a phone
           const st = document.createElement('style');
           st.textContent = '.wc3wrap:fullscreen{width:100vw!important;max-width:none!important;background:#000;display:flex;align-items:center;justify-content:center}' +
             '.wc3wrap:fullscreen canvas{width:auto!important;height:100vh!important;max-width:100vw!important;object-fit:contain}';
           document.head.append(st); wrap.className = 'wc3wrap';
           fs.textContent = 'Celá obrazovka'; fs.style.cssText = 'margin:6px 0;font-size:14px';
           fs.onclick = () => { canvas.requestFullscreen({ navigationUI: 'hide' }).then(() => canvas.focus(), e => console.log('fullscreen: ' + e)); };
           const hint = document.createElement('span'); hint.style.cssText = 'margin-left:10px;font:13px system-ui,sans-serif;color:#666';
           hint.textContent = `${resW}×${resH} (?res=ŠxV). Celou obrazovku opustíš podržením Esc.`;
           document.body.append(fs, hint, syncNote); }
if (me) {
  // each report carries the round trip the previous one took (the server keeps the worst and the average per game)
  let rtt = 0;
  const here = (gone = false) => { const body = JSON.stringify(gone ? { name: me, gone: 1 } : { name: me, ...(rtt ? { ping: rtt } : {}) });
    if (gone) { navigator.sendBeacon('/presence', body); return; }
    const t0 = performance.now();
    fetch('/presence', { method: 'POST', body, keepalive: true }).then(() => { rtt = Math.round(performance.now() - t0); }, () => {}); };
  const draw = (ps: { name: string; lan: boolean }[]) => {
    who.textContent = '';
    const h = document.createElement('div'); h.style.cssText = 'font-weight:600;color:#f0c674;margin-bottom:6px';
    h.textContent = `Hráči online (${ps.length})`; who.append(h);
    for (const pl of ps) {
      const d = document.createElement('div'); d.style.cssText = 'margin:2px 0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis';
      const dot = document.createElement('span'); dot.style.cssText = `display:inline-block;width:7px;height:7px;border-radius:50%;margin-right:6px;background:${pl.lan ? '#4caf50' : '#777'}`;
      dot.title = pl.lan ? 'připojen k LAN síti' : 've hře';
      d.append(dot, pl.name + (pl.name.toLowerCase() === me.toLowerCase() ? ' (ty)' : ''));
      if (pl.name.toLowerCase() === me.toLowerCase()) d.style.color = '#fff';
      who.append(d);
    }
    const n = document.createElement('div'); n.style.cssText = 'margin-top:6px;font-size:11px;color:#777'; n.textContent = 'zelená = připojen k LAN'; who.append(n);
  };
  const poll = async () => { try { const r = await fetch('/presence', { cache: 'no-store' }); if (r.ok) draw((await r.json()).players); } catch { /* the server's list is optional */ } };
  here(); setTimeout(poll, 500);
  setInterval(here, 20_000); setInterval(poll, 10_000);
  addEventListener('pagehide', () => here(true));
}
const pre = document.createElement('pre');
if (gpu) {
  // the log folded away under the game, opened by a click
  const box = document.createElement('details'), sum = document.createElement('summary');
  sum.textContent = 'Log'; sum.style.cssText = 'cursor:pointer;font:13px system-ui,sans-serif;margin:4px 0';
  pre.style.cssText = 'max-height:40vh;overflow:auto;font-size:11px;margin:0';
  box.append(sum, pre); document.body.append(box);
} else document.body.append(pre);
// ?logpost: the log also goes to the server (POST /log, web/tools/wc3-dev-server.mjs keeps it in
// out/browser.log), so a run can be read without copying it from the page
const posting = q.has('logpost');
let outq: string[] = ['@@start ' + new Date().toISOString() + ' ' + location.pathname + '?' + q.toString()];
const flush = () => { if (!posting || !outq.length) return; const body = outq.join('\n') + '\n'; outq = [];
  // keepalive bodies are limited to 64 KB: a larger batch (a texture dump) goes without it
  fetch('/log', { method: 'POST', body, keepalive: body.length < 60000 }).catch(() => {}); };
if (posting) setInterval(flush, 500);
const log = (s: string) => { pre.append(s + '\n'); console.log(s); if (posting) outq.push(s); };
const t0 = performance.now();
const mod = await WebAssembly.compileStreaming(fetch(q.get('wasm') ?? '/test/hostcheck.wasm'));
log(`compiled in ${(performance.now() - t0).toFixed(0)} ms`);
if (q.has('fresh')) {
  const root = await navigator.storage.getDirectory();
  for await (const [name] of (root as any).entries()) if (name === (q.get('save') ?? 'save-test')) await root.removeEntry(name, { recursive: true });
}
// ?reg=/tests/web/seed/x.txt: registry seed, copied in as /save/registry.txt
async function preload(url: string, name: string) {
  const txt = await (await fetch(url, { cache: 'no-store' })).text();
  const dir = await (await navigator.storage.getDirectory()).getDirectoryHandle(q.get('save') ?? 'save-test', { create: true });
  const w = await (await dir.getFileHandle(name, { create: true })).createWritable();
  await w.write(txt); await w.close();
}
const env0: Record<string, string> = {};
if (q.get('reg')) { await preload(q.get('reg')!, 'registry.txt'); env0.NFSU2_REGFILE = '/save/registry.txt'; }
// ?script=/tests/scripts/x.txt: an autotest input script, copied into the save dir
const env: Record<string, string> = env0;
if (gpu) { env.NFSU2_RENDERER = 'webgpu'; env.WC3_WIDTH = String(resW); env.WC3_HEIGHT = String(resH); }
const script = q.get('script');
if (script) {
  const txt = await (await fetch(script, { cache: 'no-store' })).text();
  const dir = await (await navigator.storage.getDirectory()).getDirectoryHandle(q.get('save') ?? 'save-test', { create: true });
  const w = await (await dir.getFileHandle('autotest-script.txt', { create: true })).createWritable();
  await w.write(txt); await w.close();
  env.NFSU2_INPUT_SCRIPT = '/save/autotest-script.txt';
}
// a player's folder (the start page's p-<name>) goes to the server each time the game saves into it
const saveDir = q.get('save') ?? '';
let sync: SaveSync | null = null;
if (me && /^[a-z0-9]{1,8}-[a-z0-9_-]{2,15}$/.test(saveDir)) sync = startSaveSync(saveDir, log, s => { syncNote.textContent = s; });
const p = await startProcess({
  module: mod, pool: +(q.get('pool') ?? 24), initialPages: +(q.get('pages') ?? 1024),
  cfg: { args: ['nfsu2', ...(q.getAll('arg'))], env: { HOME: '/save', NFSU2_DATA: '/data', ...env, ...Object.fromEntries(q.getAll('env').map(e => [e.slice(0, e.indexOf('=')), e.slice(e.indexOf('=') + 1)])) }, dataBase: '/data/', manifestUrl: '/data-manifest', saveDir: q.get('save') ?? 'save-test' },
  onLog: (s, fd, tid) => log(`[${tid}${fd === 2 ? '!' : ''}] ${s}`),
  onNote: log,
  onSaved: path => sync?.saved(path),
});
if (gpu) {
  try {
    const r = await attachRenderer(p, canvas, { log });
    log('[page] WebGPU renderer attached');
    // Ctrl+Shift+D: the next frame in full into the log (every draw, its shaders, textures and states); the
    // game does not see the key (before attachInput: its listener is on window too, in the capture phase)
    const traceKey = (e: KeyboardEvent) => e.ctrlKey && e.shiftKey && e.code === 'KeyD';
    window.addEventListener('keydown', e => {
      if (!traceKey(e)) return;
      e.preventDefault(); e.stopImmediatePropagation();
      if (e.repeat) return;
      log('[page] Ctrl+Shift+D: tracing the next frame'); r.trace(1);
    }, { capture: true });
    window.addEventListener('keyup', e => { if (traceKey(e)) { e.preventDefault(); e.stopImmediatePropagation(); } }, { capture: true });
    // mouse and keyboard over the picture (imageRect is in canvas pixels; the input wants CSS px)
    // the OS cursor hidden over the game (the game draws its own; both together looked like a lagging mouse), and
    // in fullscreen the keyboard (Escape goes to the game; holding it leaves) and the mouse locked to the game
    const inp = attachInput(p, canvas, { log, pointerLock: true, viewRect: () => {
      const ir = r.imageRect(), k = canvas.clientWidth / (canvas.width || 1);
      return { x: ir.x * k, y: ir.y * k, w: ir.w * k, h: ir.h * k }; } });
    (globalThis as any).wc3Input = inp;
    fs.onclick = () => { void inp.enterFullscreen().then(() => canvas.focus()); };
    // ?touch (or a phone / tablet): on-screen buttons that press the game's keys; full screen takes them along
    if (touch) {
      attachTouchKeys(wrap, inp);
      fs.onclick = () => { wrap.requestFullscreen({ navigationUI: 'hide' }).then(() => {
        const o: any = (screen as any).orientation; if (o && o.lock) o.lock('landscape').catch(() => {});
        // a keyboard (computer): Escape goes to the game, holding it leaves full screen (as without the touch buttons)
        try { (navigator as any).keyboard?.lock?.().catch((e: unknown) => log(`[input] keyboard lock refused: ${e}`)); } catch { /* none */ }
        canvas.focus(); },
        e => log('fullscreen: ' + e)); };
      log('[page] touch controls on (?notouch hides them)');
    }
    // sound: the context starts on the first click or key (the browser's rule)
    attachAudio(p, { log, gestureTarget: window }).then(a => { (globalThis as any).wc3Audio = a; log('[page] audio attached'); },
                                                       e => log('[page] no audio: ' + e));
    canvas.tabIndex = 0; canvas.focus();
    const every = +(q.get('shot') ?? 0);
    if (every > 0) setInterval(async () => {
      try { const png = await r.dump('shot'); await fetch('/shot', { method: 'POST', body: png }); }
      catch (e) { log(`[page] screenshot: ${e}`); }
    }, every * 1000);
  } catch (e) { log(`[page] no renderer: ${e}`); }
}
// ?net=<name>: the virtual LAN (web/src/net) under that player name; the notices the app shows over the game, logged
const net = q.has('net') ? attachNet(p, { name: q.get('net') || 'test', log, onNotice: t => log(`[notice] ${t}`), onLobby: j => log(`[lobby] ${j}`) }) : null;
(globalThis as any).nfsu2Net = net;
const code = await p.exited;
if (net) log(`NET ${JSON.stringify(net.stats())}`);
log(`EXIT ${code}`); flush(); document.title = 'DONE';
// the other game threads would stay where the exit left them: a trapped thread keeps the GIL, the rest wait for it
// (the picture froze, the watchdog reported every 20 s), so they stop with the process
p.terminate();
// the player's page: the frozen last picture gives way to an end screen (what happened, play again, back to the start
// page), and the tab blinks "Hra ukončena" until the player looks at it; test pages keep the plain "DONE" title
if (gpu) {
  const saveLog = () => {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([pre.textContent ?? ''], { type: 'text/plain' }));
    a.download = `log-${new Date().toISOString().replace(/[:.]/g, '-')}.txt`; a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
  };
  if (document.fullscreenElement) void document.exitFullscreen().catch(() => {});
  const crashed = code !== 0;
  const svgIcon = (size: number) => crashed
    ? `<svg xmlns='http://www.w3.org/2000/svg' width='${size}' height='${size}' viewBox='0 0 24 24' fill='none' stroke='#ff8a7a' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M12 3 2 20h20z'/><path d='M12 9v5'/><path d='M12 17.2v.3'/></svg>`
    : `<svg xmlns='http://www.w3.org/2000/svg' width='${size}' height='${size}' viewBox='0 0 24 24' fill='none' stroke='#f0c674' stroke-width='2.2' stroke-linecap='round'><path d='M12 3v8'/><path d='M6.3 6.8a8 8 0 1 0 11.4 0'/></svg>`;
  const end = document.createElement('div');
  const w = canvas.clientWidth || 1280, h = canvas.clientHeight || 720;
  end.style.cssText = `width:${w}px;height:${h}px;max-width:100%;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:14px;` +
    `background:radial-gradient(ellipse at center,#1d2230 0%,#08090d 75%);color:#ece9e1;font:16px/1.45 system-ui,sans-serif;text-align:center;border-radius:4px;box-sizing:border-box;padding:24px`;
  const icon = document.createElement('div'); icon.style.cssText = 'line-height:0'; icon.innerHTML = svgIcon(88);
  const head = document.createElement('div'); head.style.cssText = 'font-size:28px;font-weight:700;color:' + (crashed ? '#ff8a7a' : '#f0c674');
  head.textContent = crashed ? 'Hra spadla' : 'Vypnul jsi hru';
  const text = document.createElement('div'); text.style.cssText = 'max-width:640px;color:#b9b5ab';
  text.textContent = !crashed ? 'Hra skončila. Můžeš ji spustit znovu, nebo se vrátit na úvodní stránku.'
    : code === 134 ? 'Chyba ve hře (kód 134, „TRAP“ v logu), není to výpadek internetu. Stránka po pádu odpojila tento prohlížeč od LAN, '
      + 'proto ostatním hráčům hra skončila. Ulož si log a založte hru znovu.'
    : `Hra skončila s kódem ${code}. Ulož si log a spusť ji znovu.`;
  const btns = document.createElement('div'); btns.style.cssText = 'display:flex;gap:10px;flex-wrap:wrap;justify-content:center;margin-top:6px';
  const btn = (label: string, primary: boolean, on: () => void) => {
    const b = document.createElement('button'); b.textContent = label; b.onclick = on;
    b.style.cssText = `font:600 15px system-ui,sans-serif;padding:9px 18px;border-radius:8px;cursor:pointer;border:1px solid #d9a441;` +
      (primary ? 'background:#d9a441;color:#111' : 'background:transparent;color:#d9a441');
    btns.append(b);
  };
  btn('▶ Spustit znovu', true, () => location.reload());
  btn('← Úvodní stránka', false, () => { location.href = '/'; });
  if (crashed) btn('Uložit log', false, saveLog);
  end.append(icon, head, text, btns);
  canvas.replaceWith(end);
  if (me) {
    // the tab: "Hra ukončena" with the drawn icon, blinking while the page is out of sight
    const was = document.querySelector<HTMLLinkElement>('link[rel="icon"]'), wasHref = was?.href ?? '';
    const mark = 'data:image/svg+xml,' + encodeURIComponent(svgIcon(32));
    const label = crashed ? 'Hra spadla' : 'Hra ukončena', name = document.title === 'DONE' ? '' : document.title;
    let on = false;
    const show = (x: boolean) => { document.title = x || !name ? label : name; if (was) was.href = x ? mark : wasHref; };
    show(true);
    setInterval(() => { if (document.hidden) { on = !on; show(on); } else if (!on) { on = true; show(true); } }, 900);
  }
}
