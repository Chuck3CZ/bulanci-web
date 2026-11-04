// Node's http server with the rules the Node server set on it (LIMITS: its sizes, timeouts and keep-alive;
// onClientError, refuseSocket, onReq's pipelining count), and a test service: the reference the Rust engine is
// compared with (tests/cross_node.rs).  Every route answers the same bytes as the Rust tests' service
// (tests/common/mod.rs).
//   node ref_server.mjs '<json options>'     prints "listening <port>" once it is up (on 127.0.0.1, port 0)
import http from 'node:http';

const o = JSON.parse(process.argv[2] || '{}');
const LIMITS = {
  maxHeaderSize: o.maxHeaderSize ?? 16 << 10,
  headersTimeout: o.headersTimeout ?? 15_000,
  requestTimeout: o.requestTimeout ?? 30_000,
  keepAliveTimeout: o.keepAliveTimeout ?? 65_000,
  socketIdle: o.socketIdle ?? 120_000,
  maxConnections: o.maxConnections ?? 1024,
  pipelined: o.pipelined ?? 4,
  check: o.check ?? 5_000,
};
const STATUS = { 400: 'Bad Request', 408: 'Request Timeout', 431: 'Request Header Fields Too Large' };
const rawResponse = code => `HTTP/1.1 ${code} ${STATUS[code]}\r\ncontent-length: 0\r\nconnection: close\r\n\r\n`;
function refuseSocket(sock, code) {
  try { if (sock.writable) sock.end(rawResponse(code)); } catch { /* gone */ }
  setTimeout(() => sock.destroy(), 1000).unref();
}
const echo = q => JSON.stringify({ m: q.method, u: q.url, v: q.httpVersion, h: Object.entries(q.headers) });
function plain(q, s, code, text, extra = {}) {
  const body = text + '\n';
  s.writeHead(code, { 'content-type': 'text/plain; charset=utf-8', 'content-length': Buffer.byteLength(body), ...extra });
  s.end(q.method === 'HEAD' ? undefined : body);
}
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function handler(q, s) {
  const p = q.url.split('?')[0];
  if (p === '/chunked') { s.writeHead(200, { 'content-type': 'text/plain' }); s.write('hello'); await sleep(20); s.write(' world'); s.end(); return; }
  if (p === '/empty') { s.writeHead(200, { 'x-a': '1' }); s.end(); return; }
  if (p === '/204') { s.writeHead(204, { 'x-a': '1' }); s.end(); return; }
  if (p === '/304') { s.writeHead(304, { etag: '"e"' }); s.end(); return; }
  if (p === '/204te') { s.writeHead(204, { 'transfer-encoding': 'chunked' }); s.end(); return; }
  if (p === '/te') { s.writeHead(200, { 'transfer-encoding': 'chunked' }); s.write('ab'); s.end(); return; }
  if (p === '/close') { s.writeHead(200, { 'content-length': 2, connection: 'close' }); s.end('ok'); return; }
  if (p === '/keep') { s.writeHead(200, { 'content-length': 2, connection: 'keep-alive' }); s.end('ok'); return; }
  if (p === '/ka') { s.setHeader('Keep-Alive', 'timeout=7'); s.writeHead(200, { 'content-length': 2, 'x-b': '2', 'keep-alive': 'timeout=8' }); s.end('ok'); return; }
  if (p === '/ka2') { s.setHeader('Keep-Alive', 'timeout=7'); s.writeHead(200, { 'content-length': 2 }); s.end('ok'); return; }
  if (p === '/date') { s.writeHead(200, { date: 'yesterday', 'content-length': 2 }); s.end('ok'); return; }
  if (p === '/big') { const b = Buffer.alloc(200_000, 0x61); s.writeHead(200, { 'content-length': b.length }); s.end(b); return; }
  if (p === '/bigchunked') { s.writeHead(200, {}); s.write(Buffer.alloc(70_000, 0x62)); s.end(Buffer.alloc(10, 0x63)); return; }
  if (p === '/slow') { await sleep(150); plain(q, s, 200, 'slow'); return; }
  if (p.startsWith('/status/')) { s.writeHead(+p.slice(8), { 'content-length': 0 }); s.end(); return; }
  if (p === '/body') {
    const parts = [];
    for await (const c of q) parts.push(c);
    const b = Buffer.concat(parts);
    const body = JSON.stringify({ n: b.length, hex: b.toString('hex') });
    s.writeHead(200, { 'content-type': 'application/json', 'content-length': Buffer.byteLength(body) });
    s.end(body);
    return;
  }
  const body = echo(q);
  s.writeHead(200, { 'content-type': 'application/json', 'content-length': Buffer.byteLength(body) });
  s.end(q.method === 'HEAD' ? undefined : body);
}

const kQueued = Symbol('queued');
const onReq = (q, s) => {
  const conn = q.socket;
  conn[kQueued] = (conn[kQueued] ?? 0) + 1;
  if (conn[kQueued] > LIMITS.pipelined) return conn.destroy();
  let done = false;
  const finished = () => { if (!done) { done = true; conn[kQueued]--; } };
  s.once('finish', finished); s.once('close', finished);
  handler(q, s).catch(() => { try { if (!s.headersSent) plain(q, s, 500, 'Server error'); else s.destroy(); } catch { /* gone */ } });
};
function onClientError(e, sock) {
  const code = e?.code === 'HPE_HEADER_OVERFLOW' ? 431 : e?.code === 'ERR_HTTP_REQUEST_TIMEOUT' ? 408
    : String(e?.code ?? '').startsWith('HPE_') ? 400 : 0;
  if (o.logErrors) console.log(`clientError ${e?.code} writable=${sock.writable}`);
  if (!code || !sock.writable) return sock.destroy();
  refuseSocket(sock, code);
}
function onUpgrade(q, sock, head) {
  sock.on('error', () => {});
  sock.end(`HTTP/1.1 101 Switching Protocols\r\nx-up: ${echo(q)}\r\nx-pre: ${head.toString('hex')}\r\n\r\n`);
  setTimeout(() => sock.destroy(), 1000).unref();
}
const srv = http.createServer({ maxHeaderSize: LIMITS.maxHeaderSize, headersTimeout: LIMITS.headersTimeout, requestTimeout: LIMITS.requestTimeout,
  connectionsCheckingInterval: LIMITS.check, requireHostHeader: false }, onReq);
srv.keepAliveTimeout = LIMITS.keepAliveTimeout;
srv.maxConnections = LIMITS.maxConnections;
srv.setTimeout(LIMITS.socketIdle);
srv.on('clientError', onClientError);
srv.on('upgrade', onUpgrade);
srv.on('checkExpectation', (q, s) => plain(q, s, 417, 'Expectation failed', { connection: 'close' }));
srv.listen(0, '127.0.0.1', () => console.log(`listening ${srv.address().port}`));
process.on('SIGTERM', () => process.exit(0));
