// Host self-test page: runs a test module (?wasm=...) through the real host
// (pool, I/O worker, mounts) and reports its output; title becomes DONE.
import { startProcess } from '../host/process';
import { attachNet } from '../net/index';
import { attachRenderer } from '../gpu/index';
import { attachInput } from '../input/index';
import { attachTouchKeys, touchWanted } from '../app/touchkeys';
import { attachAudio } from '../audio/index';
const q = new URLSearchParams(location.search);
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
if (gpu) { canvas.width = resW; canvas.height = resH; canvas.style.cssText = 'display:block;width:1280px;max-width:100%;background:#000';
           wrap.style.cssText = 'position:relative;width:max-content;max-width:100%';
           wrap.append(canvas); document.body.append(wrap);
           // full screen in touch mode: the box (buttons included), the picture fitted, landscape on a phone
           const st = document.createElement('style');
           st.textContent = '.wc3wrap:fullscreen{width:100vw!important;max-width:none!important;background:#000;display:flex;align-items:center;justify-content:center}' +
             '.wc3wrap:fullscreen canvas{width:auto!important;height:100vh!important;max-width:100vw!important;object-fit:contain}';
           document.head.append(st); wrap.className = 'wc3wrap';
           fs.textContent = 'Celá obrazovka'; fs.style.cssText = 'margin:6px 0;font-size:14px';
           fs.onclick = () => { canvas.requestFullscreen({ navigationUI: 'hide' }).then(() => canvas.focus(), e => console.log('fullscreen: ' + e)); };
           const hint = document.createElement('span'); hint.style.cssText = 'margin-left:10px;font:13px system-ui,sans-serif;color:#666';
           hint.textContent = `${resW}×${resH} (?res=ŠxV). Celou obrazovku opustíš podržením Esc.`;
           document.body.append(fs, hint); }
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
let outq: string[] = ['@@start ' + new Date().toISOString() + ' ' + location.search];
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
const p = await startProcess({
  module: mod, pool: +(q.get('pool') ?? 24), initialPages: +(q.get('pages') ?? 1024),
  cfg: { args: ['nfsu2', ...(q.getAll('arg'))], env: { HOME: '/save', NFSU2_DATA: '/data', ...env, ...Object.fromEntries(q.getAll('env').map(e => [e.slice(0, e.indexOf('=')), e.slice(e.indexOf('=') + 1)])) }, dataBase: '/data/', manifestUrl: '/data-manifest', saveDir: q.get('save') ?? 'save-test' },
  onLog: (s, fd, tid) => log(`[${tid}${fd === 2 ? '!' : ''}] ${s}`),
  onNote: log,
});
if (gpu) {
  try {
    const r = await attachRenderer(p, canvas, { log });
    log('[page] WebGPU renderer attached');
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
        const o: any = (screen as any).orientation; if (o && o.lock) o.lock('landscape').catch(() => {}); canvas.focus(); },
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
if (gpu && code !== 0) {
  const box = document.createElement('div');
  box.style.cssText = 'margin:6px 0;padding:10px 12px;background:#7a1010;color:#fff;font:14px/1.4 system-ui,sans-serif;max-width:1280px';
  box.textContent = code === 134
    ? 'Hra spadla (chyba ve hře, kód 134, „TRAP“ v logu). Není to výpadek internetu: stránka po pádu odpojila tento prohlížeč od LAN, '
      + 'proto ostatním hráčům hra skončila. Ulož si log, obnov stránku (F5) a založte hru znovu.'
    : `Hra skončila s kódem ${code}. Ulož si log a obnov stránku (F5).`;
  const save = document.createElement('button'); save.textContent = 'Uložit log'; save.style.cssText = 'margin-left:10px;font-size:14px';
  save.onclick = () => {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([pre.textContent ?? ''], { type: 'text/plain' }));
    a.download = `wc3-log-${new Date().toISOString().replace(/[:.]/g, '-')}.txt`; a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
  };
  box.append(save);
  canvas.after(box);
}
