// Run a single-threaded WASI command module (wasm32-wasip1-threads build with
// an imported shared memory, as web/build.sh links the game) under node:wasi.
// Used for the pure-C test programs (the ISA suite, web/tools/isatest.sh) and the
// single-threaded WC3 start-up test (tools/run-node.sh).
//   node web/tools/wasi-run.mjs <module.wasm> [args...]
// Exit code = the program's exit code.
import fs from 'node:fs';
import { WASI } from 'node:wasi';

const [file, ...args] = process.argv.slice(2);
if (!file) { console.error('usage: node web/tools/wasi-run.mjs <module.wasm> [args...]'); process.exit(2); }
const mod = await WebAssembly.compile(fs.readFileSync(file));
const memImport = WebAssembly.Module.imports(mod).find(i => i.module === 'env' && i.name === 'memory');
const pages = +(process.env.WASI_PAGES ?? 16384);            // 1 GiB: the guest span plus the runtime above it
const memory = memImport ? new WebAssembly.Memory({ initial: pages, maximum: +(process.env.WASI_MAX_PAGES ?? 65536), shared: true }) : undefined;
// WASI_PREOPEN="/data=<host dir>,/save=<host dir>": folders the program sees (the
// WC3 start-up test mounts the game folder as /data, as the page does)
const preopens = Object.fromEntries((process.env.WASI_PREOPEN ?? '').split(',').filter(Boolean).map(m => {
  const k = m.indexOf('='); return [m.slice(0, k), m.slice(k + 1)];
}));
const wasi = new WASI({ version: 'preview1', args: [file, ...args], env: process.env, preopens, returnOnExit: true });
const imports = { wasi_snapshot_preview1: wasi.wasiImport, env: memory ? { memory } : {} };
// The page's host functions (runtime/web/host_abi.h, module "nfsu2"), for a
// program without the page: no subsystem is registered (headless), the clock
// is performance.now(), nothing blocks in a host.
imports.nfsu2 = {
  register: () => 0,
  now_ms: () => performance.timeOrigin + performance.now(),
  blocked_ms: () => 0,
  stack_frames: () => 0,
};
// a test program never starts threads; fail loudly if it tries
imports.wasi = { 'thread-spawn': () => { console.error('thread-spawn is not available under wasi-run'); return -1; } };
const inst = await WebAssembly.instantiate(mod, imports);
process.exitCode = wasi.start(inst);
