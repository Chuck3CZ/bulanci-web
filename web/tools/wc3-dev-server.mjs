// A small development server for the WC3 port, until the Rust server
// (web/server-rs) is adapted: the host and test pages, the module and the
// game folder, on 127.0.0.1 only.
//   node web/tools/wc3-dev-server.mjs --data "<Warcraft III folder>" [--port 8086]
// Serves
//   /test/*, /host/*, /gpu/*, /audio/*   web/dist-dev, web/dist (node web/build.mjs)
//   /wc3.wasm                            out/web/wc3.wasm (./build.sh)
//   /data-manifest                       every file of the game folder (the I/O worker's
//                                        manifest without "blocks": it reads byte ranges)
//   /data/<path>                         the game folder's files, with Range
//   /ovladani                            the control page (web/control.html): game, live log, switches
//   POST /log                            the test page's log -> out/browser.log
//   POST /shot                           the test page's screenshot -> out/shot.png
//   WS   /lan                            a minimal virtual LAN switch (web/server-rs crates/lan's protocol, no limits)
// with the headers that make the page cross-origin isolated (SharedArrayBuffer).
// Port 8086: 8080 is the QNAP's admin page (docs/PLAN.md).
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const DATA = arg('--data');
const PORT = +arg('--port', 8086);
const HOST = arg('--host', '127.0.0.1');   // 0.0.0.0: reachable from the LAN (Chrome there needs a secure origin)
// the game module and where the test pages' logs and screenshots go (Docker: a volume each)
const WASM = arg('--wasm', path.join(ROOT, 'out', 'web', 'wc3.wasm'));
// which game this server is (the hub's id: wc3, rally, bulanci): the accounts ask for it, the module is served as /<id>.wasm
const GAME_ID = arg('--game', process.env.GAME_ID || 'wc3');
const MODULE_URL = '/' + GAME_ID + '.wasm';
// the start page's save folder prefix (GAME.savePrefix: p- for WC3, cmr- for the rally): with accounts a player's own is <prefix><name>
const SAVE_PREFIX = arg('--save-prefix', process.env.SAVE_PREFIX || 'p-');
const OUT = arg('--out', path.join(ROOT, 'out'));
if (!DATA || !fs.statSync(DATA, { throwIfNoEntry: false })?.isDirectory()) {
  console.error('usage: node web/tools/wc3-dev-server.mjs --data "<Warcraft III folder>" [--port 8086]'); process.exit(2);
}
const STATIC = [path.join(ROOT, 'web', 'dist-dev'), path.join(ROOT, 'web', 'dist')];
const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.map': 'application/json', '.json': 'application/json',
  '.wasm': 'application/wasm', '.css': 'text/css', '.txt': 'text/plain; charset=utf-8' };
const ISOLATION = { 'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp',
  'Cross-Origin-Resource-Policy': 'same-origin', 'Cache-Control': 'no-store' };
// files: revalidated each time (ETag from size and time), so the 81 MB module comes once until it changes
const CACHED = { ...ISOLATION, 'Cache-Control': 'no-cache' };

// the manifest: files and folders, '/'-separated paths relative to the game folder
function manifest() {
  const files = [];
  const walk = (dir, rel) => {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
      const p = rel ? `${rel}/${e.name}` : e.name, full = path.join(dir, e.name);
      if (e.isDirectory()) { files.push({ p, d: 1 }); walk(full, p); }
      else if (e.isFile()) { const st = fs.statSync(full); files.push({ p, s: st.size, m: Math.floor(st.mtimeMs / 1000) }); }
    }
  };
  walk(DATA, '');
  return { files };
}
let MANIFEST = JSON.stringify(manifest());
console.log(`[dev-server] game folder ${DATA}: ${JSON.parse(MANIFEST).files.length} entries`);

// a path inside root, or null (no "..", no absolute paths)
function inside(root, rel) {
  const full = path.resolve(root, '.' + path.posix.normalize('/' + rel));
  return full === root || full.startsWith(root + path.sep) ? full : null;
}
function sendFile(req, res, full, type, extra) {
  let st; try { st = fs.statSync(full); } catch { res.writeHead(404, ISOLATION).end('not found'); return; }
  if (!st.isFile()) { res.writeHead(404, ISOLATION).end('not found'); return; }
  const etag = `"${st.size.toString(36)}-${Math.floor(st.mtimeMs).toString(36)}"`;
  const h = { ...CACHED, ...extra, ETag: etag, 'Last-Modified': st.mtime.toUTCString(), 'Content-Type': type ?? TYPES[path.extname(full).toLowerCase()] ?? 'application/octet-stream', 'Accept-Ranges': 'bytes' };
  if (req.headers['if-none-match'] === etag && !req.headers.range) { res.writeHead(304, h).end(); return; }
  const m = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range ?? '');
  if (m) {
    const start = +m[1], end = m[2] ? Math.min(+m[2], st.size - 1) : st.size - 1;
    if (start >= st.size || end < start) { res.writeHead(416, { ...h, 'Content-Range': `bytes */${st.size}` }).end(); return; }
    res.writeHead(206, { ...h, 'Content-Range': `bytes ${start}-${end}/${st.size}`, 'Content-Length': end - start + 1 });
    if (req.method === 'HEAD') { res.end(); return; }
    fs.createReadStream(full, { start, end }).pipe(res);
  } else {
    res.writeHead(200, { ...h, 'Content-Length': st.size });
    if (req.method === 'HEAD') { res.end(); return; }
    fs.createReadStream(full).pipe(res);
  }
}
// the module's SHA-256 (X-Module-Sha256 on /wc3.wasm): the start page checks its saved copy and a download against it,
// and the pages' own module.json (written by tools/pack-qnap.sh) against it, so a module from another build stops the start
// (X-Module-Rev: the git revision it was built from, out of build.sh's wc3.json beside it, when that is there)
let modSha = { key: '', sha: '', rev: '' };
function moduleInfo() {
  try {
    const st = fs.statSync(WASM), key = st.size + ':' + st.mtimeMs;
    if (modSha.key !== key) {
      let rev = ''; try { const j = JSON.parse(fs.readFileSync(WASM.replace(/\.wasm$/, '.json'), 'utf8')); if (j.size === st.size) rev = String(j.rev ?? ''); } catch { /* none */ }
      modSha = { key, sha: crypto.createHash('sha256').update(fs.readFileSync(WASM)).digest('hex'), rev };
    }
    return { 'X-Module-Sha256': modSha.sha, ...(modSha.rev ? { 'X-Module-Rev': modSha.rev } : {}) };
  } catch { return {}; }
}

// ---- the virtual LAN switch (minimal; the real one is web/server-rs crates/lan) ----
// Every page gets 10.77.0.N; binary frames: [0] kind, [4..7] source vip (rewritten to the
// sender's), [8..11] destination vip (0xFFFFFFFF: everyone else); kind 32 is a bundle of frames.
import { WebSocketServer } from 'ws';
import crypto from 'node:crypto';
const lan = new WebSocketServer({ noServer: true });
// A page whose hello says "replay":1 keeps its place through a lost link: what the switch sends it is counted and
// kept until the page says it has it ("rx" in its pings), and the page does the same the other way (our "rx" in each
// pong), so after a resume (its hello with the address, the token and its "rx") what did not arrive goes again and
// the game's TCP connections never notice.  Meanwhile the place is held (on everyone's peer list, frames to it kept)
// for HOLD_MS; a page that closed the link itself (1000: idle, the game ended; 1001: the page went) frees it at once.
const HOLD_MS = 150_000, HOLD_BYTES = 32 << 20;
const peers = new Map();            // vip -> { vip, ws, name, token, replay, rxIn, out, outBase, outBytes, hold, heldAt }
let nextHost = 2;
const ipStr = v => [v >>> 24, (v >>> 16) & 255, (v >>> 8) & 255, v & 255].join('.');
const ipNum = s => typeof s === 'string' && /^\d+\.\d+\.\d+\.\d+$/.test(s) ? s.split('.').reduce((a, b) => (a * 256 + (+b)) >>> 0, 0) : 0;
const live = p => p.ws && p.ws.readyState === 1;
const peerList = () => [...peers].map(([vip, p]) => ({ vip, name: p.name }));
const tellPeers = () => { const m = JSON.stringify({ t: 'peers', peers: peerList() });
  for (const p of peers.values()) if (live(p)) p.ws.send(m); };
function newVip() {
  for (let i = 0; i < 253; i++) {
    const v = ((10 << 24) | (77 << 16) | (2 + (nextHost++ - 2) % 253)) >>> 0;
    if (!peers.has(v)) return v;
  }
  return 0;
}
function forget(p, why) {
  if (peers.get(p.vip) !== p) return;
  clearTimeout(p.hold); peers.delete(p.vip);
  console.log(`[lan] ${ipStr(p.vip)} left (${why})`); tellPeers();
}
/** the page has our messages up to number n */
function trimOut(p, n) {
  const k = n - p.outBase;
  if (!(k > 0) || k > p.out.length) return;
  for (let i = 0; i < k; i++) p.outBytes -= p.out[i].length;
  p.out.splice(0, k); p.outBase = n;
}
function sendTo(p, frame) {
  if (p.replay) {
    p.out.push(frame); p.outBytes += frame.length;
    if (p.outBytes > HOLD_BYTES) {
      if (!p.ws) { forget(p, 'too much waited for it'); return; }
      p.outBase += p.out.length; p.out = []; p.outBytes = 0;     // it does not say what it got: no resume for it
    }
  }
  if (live(p)) p.ws.send(frame);
}
function route(from, frame) {
  if (frame.length < 12) return;
  frame.writeUInt32BE(from, 4);
  const dst = frame.readUInt32BE(8);
  if (dst === 0xFFFFFFFF) { for (const [v, p] of peers) if (v !== from) sendTo(p, frame); }
  else { const p = peers.get(dst); if (p) sendTo(p, frame); }
}
lan.on('connection', ws => {
  let me = null;
  const mine = () => me && peers.get(me.vip) === me && me.ws === ws ? me : null;
  ws.on('message', (data, isBinary) => {
    if (!isBinary) {
      let m; try { m = JSON.parse(String(data)); } catch { return; }
      if (m.t === 'hello' && !me) {
        const name = String(ws.hubNick ?? m.name ?? 'player').slice(0, 32), old = ipNum(m.vip), held = old ? peers.get(old) : null;
        if (held && held.replay && m.replay === 1 && typeof m.token === 'string' && m.token === held.token &&
            Number.isInteger(m.rx) && m.rx >= held.outBase && m.rx <= held.outBase + held.out.length) {
          // the same session again: the place was held (or its old link died without a close we heard)
          if (held.ws && held.ws !== ws) { const o = held.ws; held.ws = null; o.terminate(); }
          clearTimeout(held.hold); held.hold = null;
          const away = held.heldAt ? ((Date.now() - held.heldAt) / 1000).toFixed(1) : '0';
          held.heldAt = 0; held.ws = ws; held.name = name; me = held;
          trimOut(held, m.rx);
          ws.send(JSON.stringify({ t: 'welcome', vip: old, vipStr: ipStr(old), mask: '255.255.255.0', token: held.token, replay: 1, resumed: 1, rx: held.rxIn }));
          for (const f of held.out) ws.send(f);
          ws.send(JSON.stringify({ t: 'peers', peers: peerList() }));
          console.log(`[lan] ${ipStr(old)} "${name}" back after ${away} s (${held.out.length} frame(s) sent again)`);
          return;
        }
        // a new session (a held place is never given to another page: it waits for its token)
        const vip = old && !peers.has(old) ? old : newVip();
        if (!vip) { ws.close(1013, 'LAN full'); return; }
        me = { vip, ws, name, token: crypto.randomBytes(16).toString('hex'), replay: m.replay === 1, rxIn: 0, out: [], outBase: 0, outBytes: 0, hold: null, heldAt: 0 };
        peers.set(vip, me);
        ws.send(JSON.stringify({ t: 'welcome', vip, vipStr: ipStr(vip), mask: '255.255.255.0', token: me.token, ...(me.replay ? { replay: 1 } : {}) }));
        console.log(`[lan] ${ipStr(vip)} "${name}" joined (${peers.size} on the LAN)`);
        tellPeers();
      } else if (m.t === 'ping') {
        const p = mine();
        if (p && p.replay && Number.isInteger(m.rx)) trimOut(p, m.rx);
        ws.send(JSON.stringify({ t: 'pong', id: m.id, ...(p && p.replay ? { rx: p.rxIn } : {}) }));
      }
      return;
    }
    const p = mine();
    if (!p) return;
    p.rxIn++;
    const b = Buffer.isBuffer(data) ? data : Buffer.from(data);
    if (b[0] === 32) {                                   // a bundle: [0] n, [2..3] length, n x destination, frame
      let o = 4;
      while (o + 4 <= b.length) {
        const n = b[o], len = b.readUInt16BE(o + 2); o += 4;
        const dsts = []; for (let i = 0; i < n; i++) { dsts.push(b.readUInt32BE(o)); o += 4; }
        if (o + len > b.length) break;
        const f = Buffer.from(b.subarray(o, o + len)); o += len;
        if (!n) route(p.vip, f);
        else for (const d of dsts) { const g = Buffer.from(f); g.writeUInt32BE(d, 8); route(p.vip, g); }
      }
    } else if (b[0] !== 33) route(p.vip, Buffer.from(b));
  });
  ws.on('close', code => {
    const p = mine();
    if (!p) return;
    if (p.replay && code !== 1000 && code !== 1001) {
      p.ws = null; p.heldAt = Date.now();
      p.hold = setTimeout(() => forget(p, `not back within ${HOLD_MS / 1000} s`), HOLD_MS);
      console.log(`[lan] ${ipStr(p.vip)} "${p.name}" lost its link (${code}): its place is held ${HOLD_MS / 1000} s`);
      return;
    }
    forget(p, code === 1000 ? 'closed' : String(code));
  });
});

// ---- accounts (the games hub hry.gabrhelovi.cz, rozcestnik/api/server.mjs) ----
// --hub <url> (or HUB): everything but the start page needs a player the hub allows this game (GAME_ID), and the
// player's name is the account's.  The hub is asked with the player's cookie (/api/me), the answer kept HUB_CACHE.
// Without --hub (a dev machine) the server is open as before.  HUB_KEY (environment): the hub's key for activity.
const HUB = (arg('--hub', process.env.HUB || '') || '').replace(/\/$/, '');
const HUB_LOGIN = arg('--hub-login', process.env.HUB_LOGIN || 'https://hry.gabrhelovi.cz/');
const HUB_KEY = process.env.HUB_KEY || '', HUB_CACHE = 30_000;
const hubSeen = new Map();            // the cookie -> { t, me }
let hubDown = 0;                      // when the hub last did not answer (the start page says so instead of "sign in")
async function whoIs(req) {
  if (!HUB) return null;
  const m = /(?:^|;\s*)gs=([^;]+)/.exec(req.headers.cookie || ''); if (!m) return null;
  const c = hubSeen.get(m[1]); if (c && Date.now() - c.t < HUB_CACHE) return c.me;
  let me = null;
  try { const r = await fetch(HUB + '/api/me', { headers: { cookie: 'gs=' + m[1] }, signal: AbortSignal.timeout(3000) }); if (r.ok) me = await r.json(); hubDown = 0; }
  catch (e) { console.log(`[hub] ${HUB}: ${e.message}`); hubDown = Date.now(); if (c) return c.me; }   // the hub away: the last answer holds
  if (hubSeen.size > 2000) hubSeen.clear();
  hubSeen.set(m[1], { t: Date.now(), me }); return me;
}
const allowed = me => !!(me && me.games && me.games[GAME_ID]);
const clientIp = req => String(req.headers['cf-connecting-ip'] || req.socket.remoteAddress || '');
function hubRecord(r) {
  if (!HUB || !HUB_KEY) return;
  fetch(HUB + '/api/activity', { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Hub-Key': HUB_KEY }, body: JSON.stringify({ game: GAME_ID, ...r }) })
    .catch(e => console.log('[hub] activity: ' + e.message));
}
// a player's time in the game: from the first presence report to the last (or the page's goodbye), with the address
// it came from and the round trips its reports measured; printed (the container's log) and sent to the hub
const plays = new Map();              // lower-case name -> { nick, start, seen, ip, country, ua, pings }
function playSeen(nick, req, ping) {
  const k = nick.toLowerCase(); let s = plays.get(k);
  if (!s) {
    s = { nick, start: Date.now(), seen: 0, ip: clientIp(req), country: String(req.headers['cf-ipcountry'] || ''), ua: String(req.headers['user-agent'] || '').slice(0, 160), pings: [] };
    plays.set(k, s);
    console.log(`[play] ${nick} started from ${s.ip}${s.country ? ' (' + s.country + ')' : ''}`);
    hubRecord({ nick, ev: 'start', ip: s.ip, country: s.country, ua: s.ua, start: s.start });
  }
  s.seen = Date.now();
  if (Number.isFinite(ping) && ping > 0 && ping < 60e3) { s.pings.push(Math.round(ping)); if (s.pings.length > 500) s.pings.shift(); }
}
function playEnd(k, why) {
  const s = plays.get(k); if (!s) return;
  plays.delete(k);
  const pingMax = s.pings.length ? Math.max(...s.pings) : 0, pingAvg = s.pings.length ? Math.round(s.pings.reduce((a, b) => a + b, 0) / s.pings.length) : 0;
  const end = why === 'left' ? Date.now() : s.seen;
  console.log(`[play] ${s.nick} ended (${why}) after ${Math.round((end - s.start) / 60000)} min, ping max ${pingMax} / avg ${pingAvg} ms`);
  hubRecord({ nick: s.nick, ev: 'end', ip: s.ip, country: s.country, start: s.start, end, pingMax, pingAvg });
}
setInterval(() => { const now = Date.now(); for (const [k, s] of plays) if (now - s.seen > PRESENCE_TTL) playEnd(k, 'no report'); }, 30_000).unref();

const PRESENCE_TTL = 150_000;         // the pages report every 20 s (a hidden tab maybe once a minute); a closed one says so
const presence = new Map();           // lower-case name -> { name, since, seen }
const server = http.createServer(async (req, res) => {
  // a request line that is not a path ("//", a bad %-escape) must not take the server down
  let url, p;
  try { url = new URL(req.url, 'http://x'); p = decodeURIComponent(url.pathname); } catch { res.writeHead(400, ISOLATION).end('bad request'); return; }
  // the account (--hub): only the start page and /whoami without one the hub allows this game
  const me = await whoIs(req);
  if (p === '/whoami') {
    res.writeHead(200, { ...ISOLATION, 'Content-Type': 'application/json', 'Cache-Control': 'no-store' })
      .end(JSON.stringify({ hub: !!HUB, login: HUB_LOGIN, me: me ? { nick: me.nick, ok: allowed(me) } : null, ...(hubDown && Date.now() - hubDown < 60e3 ? { hubDown: 1 } : {}) }));
    return;
  }
  if (HUB && p !== '/' && !allowed(me)) {
    if (p.startsWith('/hra/') || p.startsWith('/test/') || /text\/html/.test(req.headers.accept || '')) { res.writeHead(302, { ...ISOLATION, Location: '/' }).end(); return; }
    res.writeHead(me ? 403 : 401, { ...ISOLATION, 'Content-Type': 'application/json' }).end(JSON.stringify({ err: me ? 'not allowed' : 'login', login: HUB_LOGIN }));
    return;
  }
  // the test page's log (hostcheck.ts ?logpost): out/browser.log, started over by a new run
  // the test page's screenshot (hostcheck.ts ?gpu&shot=N): out/shot.png, replaced each time
  if (req.method === 'POST' && p === '/shot') {
    const parts = []; let n = 0;
    req.on('data', d => { parts.push(d); n += d.length; if (n > 32e6) req.destroy(); });
    req.on('end', () => {
      try { fs.writeFileSync(path.join(OUT, 'shot.png'), Buffer.concat(parts)); } catch (e) { console.log('[dev-server] shot: ' + e.message); }
      res.writeHead(204, ISOLATION).end();
    });
    return;
  }
  if (req.method === 'POST' && p === '/log') {
    let body = ''; req.setEncoding('utf8');
    req.on('data', d => { body += d; if (body.length > 8e6) req.destroy(); });
    req.on('end', () => {
      const f = path.join(OUT, 'browser.log');
      try { if (body.startsWith('@@start')) fs.writeFileSync(f, body); else fs.appendFileSync(f, body); } catch (e) { console.log('[dev-server] log: ' + e.message); }
      res.writeHead(204, ISOLATION).end();
    });
    return;
  }
  // who is playing: a game page says so every 20 s (POST /presence {"name"}, {"name","gone":1} as it closes);
  // GET /presence lists the names seen within PRESENCE_TTL, "lan" when that name also has a place on the LAN switch
  if (p === '/presence') {
    if (req.method === 'POST') {
      let body = ''; req.setEncoding('utf8');
      req.on('data', d => { body += d; if (body.length > 1024) req.destroy(); });
      req.on('end', () => {
        try {
          const m = JSON.parse(body), name = me ? me.nick : String(m.name ?? '');     // with accounts: the account's name
          if (/^[A-Za-z0-9_-]{2,15}$/.test(name)) {
            const k = name.toLowerCase(), was = presence.get(k);
            if (m.gone) { presence.delete(k); playEnd(k, 'left'); }
            else if (was || presence.size < 500) { presence.set(k, { name, since: was?.since ?? Date.now(), seen: Date.now() }); playSeen(name, req, +m.ping); }
          }
        } catch { /* not ours */ }
        res.writeHead(204, ISOLATION).end();
      });
      return;
    }
    const now = Date.now(), onLan = new Set([...peers.values()].map(q => q.name.toLowerCase()));
    for (const [k, v] of presence) if (now - v.seen > PRESENCE_TTL) presence.delete(k);
    const players = [...presence.values()].sort((a, b) => a.since - b.since)
      .map(v => ({ name: v.name, since: v.since, lan: onLan.has(v.name.toLowerCase()) }));
    res.writeHead(200, { ...ISOLATION, 'Content-Type': 'application/json' }).end(JSON.stringify({ players }));
    return;
  }
  // player progress, one zip per player (the start page's "Game progress"): /saves/<key>, key = the save folder's name.
  // Without accounts whoever knows the name can read and replace it; with them (--hub) only its player.
  const mSave = /^\/saves\/([a-z0-9_-]{1,60})$/.exec(p);
  if (mSave && HUB && mSave[1] !== SAVE_PREFIX + me.nick.toLowerCase()) { res.writeHead(403, ISOLATION).end(); return; }
  if (mSave) {
    const f = path.join(OUT, 'saves', mSave[1] + '.zip'), H = { ...ISOLATION, 'Cache-Control': 'no-store' };
    if (req.method === 'PUT') {
      const parts = []; let n = 0, bad = false;
      req.on('data', d => { n += d.length; if (n > 16e6) { bad = true; req.destroy(); } else parts.push(d); });
      req.on('end', () => {
        if (bad) return;
        try { fs.mkdirSync(path.dirname(f), { recursive: true }); fs.writeFileSync(f + '.tmp', Buffer.concat(parts)); fs.renameSync(f + '.tmp', f);
          // X-Saves-Mtime: the newest file in it (ms), so the copy is as old as the progress and not newer than the browser's
          const mt = +req.headers['x-saves-mtime']; if (mt > 0 && mt <= Date.now() + 60e3) fs.utimesSync(f, new Date(), new Date(mt));
          console.log(`[saves] ${mSave[1]}: ${n} bytes`); res.writeHead(204, H).end(); }
        catch (e) { console.log('[saves] ' + e.message); res.writeHead(500, H).end(); }
      });
      req.on('close', () => { if (bad && !res.headersSent) { try { res.writeHead(413, H).end(); } catch { /* gone */ } } });
      return;
    }
    if (req.method === 'GET' || req.method === 'HEAD') {
      let st; try { st = fs.statSync(f); } catch { res.writeHead(404, H).end(); return; }
      res.writeHead(200, { ...H, 'Content-Type': 'application/zip', 'Content-Length': st.size, 'Last-Modified': st.mtime.toUTCString() });
      if (req.method === 'HEAD') res.end(); else fs.createReadStream(f).pipe(res);
      return;
    }
  }
  if (req.method !== 'GET' && req.method !== 'HEAD') { res.writeHead(405, ISOLATION).end(); return; }
  if (p === '/data-manifest') { res.writeHead(200, { ...ISOLATION, 'Content-Type': 'application/json' }).end(MANIFEST); return; }
  if (p.startsWith('/data/')) { const f = inside(DATA, p.slice(6)); return f ? sendFile(req, res, f) : res.writeHead(403, ISOLATION).end(); }
  if (p === MODULE_URL || p === '/wc3.wasm') return sendFile(req, res, WASM, undefined, moduleInfo());   // /wc3.wasm: the name all the ports had
  // the control page: the game in a frame, its log in a terminal, the debug switches (web/control.html)
  if (p === '/ovladani' || p === '/control') return sendFile(req, res, path.join(ROOT, 'web', 'control.html'));
  // the start page: a name, then the game (web/landing.html); without it the test page
  if (p === '/') {
    const land = path.join(ROOT, 'web', 'landing.html');
    if (fs.existsSync(land)) return sendFile(req, res, land);
    res.writeHead(302, { ...ISOLATION, Location: '/test/hostcheck.html' }).end(); return;
  }
  // the game's short address (the start page's Play): the game page, which takes its parameters from the tab
  const rel = /^\/hra\/[A-Za-z0-9_-]{2,15}\/?$/.test(p) ? 'test/hostcheck.html' : p.slice(1);
  for (const root of STATIC) {
    const f = inside(root, rel);
    if (f && fs.existsSync(f) && fs.statSync(f).isFile()) return sendFile(req, res, f);
  }
  res.writeHead(404, ISOLATION).end('not found');
});
server.on('upgrade', async (req, sock, head) => {
  let lanPath = ''; try { lanPath = new URL(req.url, 'http://x').pathname; } catch { /* bad */ }
  if (lanPath !== '/lan') return sock.destroy();
  const me = await whoIs(req);
  if (HUB && !allowed(me)) { sock.end('HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n'); return; }
  lan.handleUpgrade(req, sock, head, ws => { if (me) ws.hubNick = me.nick; lan.emit('connection', ws, req); });
});
server.listen(PORT, HOST, () => console.log(`[dev-server] http://${HOST}:${PORT}/${HUB ? ` (accounts: ${HUB}${HUB_KEY ? ', activity on' : ''})` : ''}`));
