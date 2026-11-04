// The page's time zone for the game.  WASI has no zone database, so the C
// runtime's local time would be UTC; instead the page reads the zone from the
// browser and passes it in the environment (runtime/web/tz.c parses it):
//
//   NFSU2_TZ      "<standard name>;<daylight name>;<offset>[;<t>:<offset>]..."
//                 offsets in seconds east of UTC, t = Unix seconds of each
//                 transition (from t on, the offset after it), for the
//                 previous, this and the next year
//   NFSU2_TZ_NAME the IANA name, for the log
//
// The game reads it through GetTimeZoneInformation (the CRT's tzset) and
// FileTimeToLocalFileTime (save-file times).  Plain erasable TypeScript, so
// node can run it too (web/tools/tztest.sh).

/** seconds east of UTC at Unix time t (seconds) */
function offsetAt(t: number): number {
  return -new Date(t * 1000).getTimezoneOffset() * 60;
}

function zoneName(t: number, zone: string | undefined): string {
  try {
    const parts = new Intl.DateTimeFormat('en-US', { timeZone: zone, timeZoneName: 'long' }).formatToParts(new Date(t * 1000));
    return (parts.find(p => p.type === 'timeZoneName')?.value ?? '').replace(/;/g, ',').slice(0, 31);
  } catch { return ''; }
}

export function tzEnv(now: Date = new Date()): Record<string, string> {
  const y = now.getUTCFullYear();
  const start = Date.UTC(y - 1, 0, 1) / 1000, end = Date.UTC(y + 2, 0, 1) / 1000;
  const trans: [number, number][] = [];
  let prev = offsetAt(start);
  const off0 = prev;
  for (let t = start + 3600; t <= end; t += 3600) {
    const o = offsetAt(t);
    if (o === prev) continue;
    let lo = t - 3600, hi = t;                    // to the second: offsetAt(lo) = prev, offsetAt(hi) = o
    while (hi - lo > 1) { const m = Math.floor((lo + hi) / 2); if (offsetAt(m) === prev) lo = m; else hi = m; }
    trans.push([hi, o]);
    prev = o;
  }
  const offs = [off0, ...trans.map(x => x[1])];
  const std = Math.min(...offs), dst = Math.max(...offs);
  const zone = (() => { try { return Intl.DateTimeFormat().resolvedOptions().timeZone; } catch { return undefined; } })();
  // names: from an instant in standard and one in daylight time
  const nowS = Math.floor(now.getTime() / 1000);
  const stdAt = [start, ...trans.map(x => x[0])].find((_t, i) => offs[i] === std) ?? nowS;
  const dstAt = [start, ...trans.map(x => x[0])].find((_t, i) => offs[i] === dst) ?? nowS;
  const stdName = zoneName(stdAt, zone) || 'Standard Time', dstName = dst !== std ? (zoneName(dstAt, zone) || 'Daylight Time') : stdName;
  const spec = [stdName, dstName, String(off0), ...trans.map(([t, o]) => `${t}:${o}`)].join(';');
  return { NFSU2_TZ: spec, ...(zone ? { NFSU2_TZ_NAME: zone } : {}) };
}
