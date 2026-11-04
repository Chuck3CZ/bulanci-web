// Checks runtime/web/tz.c against the JS engine's local time for the zone of
// this process (TZ=... in the environment): web/tools/tztest.sh runs it per zone.
//   TZ=<zone> node web/tools/tztest.mjs <tztest.wasm>
import { execFileSync } from 'node:child_process';
import { tzEnv } from '../src/host/tz.ts';

const wasm = process.argv[2];
const env = tzEnv(new Date());
const spec = env.NFSU2_TZ.split(';');
const y = new Date().getUTCFullYear();
const start = Date.UTC(y - 1, 0, 1) / 1000, end = Date.UTC(y + 2, 0, 1) / 1000 - 1;
const ts = [];
for (let t = start; t <= end; t += 6 * 3600 + 1234) ts.push(t);       // every ~6 h, drifting through the day
for (const tr of spec.slice(3)) { const t = +tr.split(':')[0]; ts.push(t - 3601, t - 1, t, t + 1, t + 3599); }
const out = execFileSync(process.execPath, ['--no-warnings', new URL('./wasi-run.mjs', import.meta.url).pathname, wasm, ...ts.map(String)],
  { env: { ...process.env, ...env, WASI_PAGES: '64', WASI_MAX_PAGES: '65536' }, maxBuffer: 64 << 20 }).toString();
let bad = 0, ambiguous = 0, n = 0;
const pad = (v, k = 2) => String(v).padStart(k, '0');
for (const line of out.trim().split('\n')) {
  const [t, off, isdst, date, time, wday, yday, back] = line.split(' ');
  const d = new Date(+t * 1000); n++;
  const want = `${-d.getTimezoneOffset() * 60} ${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())} ${d.getDay()}`;
  const got = `${off} ${date} ${time} ${wday}`;
  const jan1 = new Date(d.getFullYear(), 0, 1), ydayWant = Math.round((new Date(d.getFullYear(), d.getMonth(), d.getDate()) - jan1) / 864e5);
  let ok = got === want && +yday === ydayWant;
  // mktime: the same instant, or the other occurrence of a repeated (fall-back) local time
  if (+back !== +t) { const b = new Date(+back * 1000); if (b.getHours() === d.getHours() && b.getMinutes() === d.getMinutes() && b.getDate() === d.getDate()) ambiguous++; else ok = false; }
  const std = Math.min(...spec.slice(2).map(x => +x.split(':').pop()));
  if ((+isdst === 1) !== (+off !== std)) ok = false;
  if (!ok && bad++ < 8) console.log(`  MISMATCH t=${t}: got "${got} yday ${yday} back ${back} isdst ${isdst}", want "${want} yday ${ydayWant}"`);
}
console.log(`${process.env.TZ ?? '(system)'}: ${n} instants, ${bad} mismatches, ${ambiguous} repeated-hour round trips; names "${spec[0]}" / "${spec[1]}", ${spec.length - 3} transitions`);
process.exitCode = bad ? 1 : 0;
