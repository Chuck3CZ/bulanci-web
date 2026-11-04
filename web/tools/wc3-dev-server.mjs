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
function sendFile(req, res, full, type) {
  let st; try { st = fs.statSync(full); } catch { res.writeHead(404, ISOLATION).end('not found'); return; }
  if (!st.isFile()) { res.writeHead(404, ISOLATION).end('not found'); return; }
  const etag = `"${st.size.toString(36)}-${Math.floor(st.mtimeMs).toString(36)}"`;
  const h = { ...CACHED, ETag: etag, 'Content-Type': type ?? TYPES[path.extname(full).toLowerCase()] ?? 'application/octet-stream', 'Accept-Ranges': 'bytes' };
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

// ---- the virtual LAN switch (minimal; the real one is web/server-rs crates/lan) ----
// Every page gets 10.77.0.N; binary frames: [0] kind, [4..7] source vip (rewritten to the
// sender's), [8..11] destination vip (0xFFFFFFFF: everyone else); kind 32 is a bundle of frames.
import { WebSocketServer } from 'ws';
import crypto from 'node:crypto';
const lan = new WebSocketServer({ noServer: true });
const peers = new Map();            // vip -> { ws, name }
let nextHost = 2;
const ipStr = v => [v >>> 24, (v >>> 16) & 255, (v >>> 8) & 255, v & 255].join('.');
const peerList = () => [...peers].map(([vip, p]) => ({ vip, name: p.name }));
const tellPeers = () => { const m = JSON.stringify({ t: 'peers', peers: peerList() });
  for (const p of peers.values()) if (p.ws.readyState === 1) p.ws.send(m); };
function route(from, frame) {
  if (frame.length < 12) return;
  frame.writeUInt32BE(from, 4);
  const dst = frame.readUInt32BE(8);
  if (dst === 0xFFFFFFFF) { for (const [v, p] of peers) if (v !== from && p.ws.readyState === 1) p.ws.send(frame); }
  else { const p = peers.get(dst); if (p && p.ws.readyState === 1) p.ws.send(frame); }
}
lan.on('connection', ws => {
  let vip = 0;
  ws.on('message', (data, isBinary) => {
    if (!isBinary) {
      let m; try { m = JSON.parse(String(data)); } catch { return; }
      if (m.t === 'hello' && !vip) {
        const old = typeof m.vip === 'string' ? m.vip.split('.').reduce((a, b) => (a * 256 + (+b)) >>> 0, 0) : 0;
        vip = old && !peers.has(old) ? old : ((10 << 24) | (77 << 16) | (nextHost++ & 255)) >>> 0;
        peers.set(vip, { ws, name: String(m.name ?? 'player').slice(0, 32) });
        ws.send(JSON.stringify({ t: 'welcome', vip, vipStr: ipStr(vip), mask: '255.255.255.0', token: crypto.randomBytes(16).toString('hex') }));
        console.log(`[lan] ${ipStr(vip)} "${peers.get(vip).name}" joined (${peers.size} on the LAN)`);
        tellPeers();
      } else if (m.t === 'ping') ws.send(JSON.stringify({ t: 'pong', id: m.id }));
      return;
    }
    if (!vip) return;
    const b = Buffer.isBuffer(data) ? data : Buffer.from(data);
    if (b[0] === 32) {                                   // a bundle: [0] n, [2..3] length, n x destination, frame
      let o = 4;
      while (o + 4 <= b.length) {
        const n = b[o], len = b.readUInt16BE(o + 2); o += 4;
        const dsts = []; for (let i = 0; i < n; i++) { dsts.push(b.readUInt32BE(o)); o += 4; }
        if (o + len > b.length) break;
        const f = Buffer.from(b.subarray(o, o + len)); o += len;
        if (!n) route(vip, f);
        else for (const d of dsts) { const g = Buffer.from(f); g.writeUInt32BE(d, 8); route(vip, g); }
      }
    } else if (b[0] !== 33) route(vip, Buffer.from(b));
  });
  ws.on('close', () => { if (vip && peers.get(vip)?.ws === ws) { peers.delete(vip); console.log(`[lan] ${ipStr(vip)} left`); tellPeers(); } });
});

const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  const p = decodeURIComponent(url.pathname);
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
  if (req.method !== 'GET' && req.method !== 'HEAD') { res.writeHead(405, ISOLATION).end(); return; }
  if (p === '/data-manifest') { res.writeHead(200, { ...ISOLATION, 'Content-Type': 'application/json' }).end(MANIFEST); return; }
  if (p.startsWith('/data/')) { const f = inside(DATA, p.slice(6)); return f ? sendFile(req, res, f) : res.writeHead(403, ISOLATION).end(); }
  if (p === '/wc3.wasm') return sendFile(req, res, WASM);
  // the start page: a name, then the game (web/landing.html); without it the test page
  if (p === '/') {
    const land = path.join(ROOT, 'web', 'landing.html');
    if (fs.existsSync(land)) return sendFile(req, res, land);
    res.writeHead(302, { ...ISOLATION, Location: '/test/hostcheck.html' }).end(); return;
  }
  for (const root of STATIC) {
    const f = inside(root, p.slice(1));
    if (f && fs.existsSync(f) && fs.statSync(f).isFile()) return sendFile(req, res, f);
  }
  res.writeHead(404, ISOLATION).end('not found');
});
server.on('upgrade', (req, sock, head) => {
  if (new URL(req.url, 'http://x').pathname !== '/lan') return sock.destroy();
  lan.handleUpgrade(req, sock, head, ws => lan.emit('connection', ws, req));
});
server.listen(PORT, HOST, () => console.log(`[dev-server] http://${HOST}:${PORT}/`));
