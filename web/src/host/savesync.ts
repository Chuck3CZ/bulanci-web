// The player's progress to the server from the game page: when the game has written a file into its save folder (the
// I/O worker's 'saved', process.ts onSaved), the whole folder goes up a few seconds later as the zip the start page
// keeps on the server (PUT /saves/<folder>, web/tools/wc3-dev-server.mjs; the start page's syPush makes the same one).
// Nothing is checked on a timer: only a save starts it.  While something is waiting or on its way, closing the tab
// asks the browser's own "leave the page?" (a page cannot word that dialog or add buttons to it).
type Entry = { name: string; data: Uint8Array; mtime: number };

const crcT = (() => { const t = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
const crc32 = (b: Uint8Array) => { let c = 0xFFFFFFFF; for (let i = 0; i < b.length; i++) c = crcT[(c ^ b[i]) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
/** "stored" entries only, UTF-8 names (the start page's zipMake) */
function zipMake(entries: Entry[]): Blob {
  const enc = new TextEncoder(), parts: Uint8Array[] = [], cen: Uint8Array[] = []; let off = 0;
  for (const e of entries) {
    const nm = enc.encode(e.name), crc = crc32(e.data), len = e.data.length;
    const h = new DataView(new ArrayBuffer(30));
    h.setUint32(0, 0x04034b50, true); h.setUint16(4, 20, true); h.setUint16(6, 0x800, true); h.setUint16(12, 0x21, true);
    h.setUint32(14, crc, true); h.setUint32(18, len, true); h.setUint32(22, len, true); h.setUint16(26, nm.length, true);
    parts.push(new Uint8Array(h.buffer), nm, e.data);
    const c = new DataView(new ArrayBuffer(46));
    c.setUint32(0, 0x02014b50, true); c.setUint16(4, 20, true); c.setUint16(6, 20, true); c.setUint16(8, 0x800, true); c.setUint16(14, 0x21, true);
    c.setUint32(16, crc, true); c.setUint32(20, len, true); c.setUint32(24, len, true); c.setUint16(28, nm.length, true); c.setUint32(42, off, true);
    cen.push(new Uint8Array(c.buffer), nm);
    off += 30 + nm.length + len;
  }
  const cs = cen.reduce((a, b) => a + b.length, 0), end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true); end.setUint16(8, entries.length, true); end.setUint16(10, entries.length, true);
  end.setUint32(12, cs, true); end.setUint32(16, off, true);
  return new Blob([...parts, ...cen, new Uint8Array(end.buffer)] as BlobPart[], { type: 'application/zip' });
}

/** the folder's files; null when one cannot be read now (the game has it open): then nothing goes up this time */
async function folder(dirName: string): Promise<Entry[] | null> {
  const out: Entry[] = [];
  let d: FileSystemDirectoryHandle;
  try { d = await (await navigator.storage.getDirectory()).getDirectoryHandle(dirName); } catch { return out; }
  let ok = true;
  const walk = async (h: FileSystemDirectoryHandle, pre: string) => {
    for await (const [nm, c] of (h as any).entries() as AsyncIterable<[string, FileSystemHandle]>) {
      if (c.kind === 'directory') await walk(c as FileSystemDirectoryHandle, pre + nm + '/');
      else try { const f = await (c as FileSystemFileHandle).getFile(); out.push({ name: pre + nm, data: new Uint8Array(await f.arrayBuffer()), mtime: f.lastModified }); } catch { ok = false; }
    }
  };
  await walk(d, '');
  return ok ? out : null;
}

export type SaveSync = { saved(path: string): void; state(): 'idle' | 'waiting' | 'sending' };

export function startSaveSync(dirName: string, note: (s: string) => void, show: (s: string) => void): SaveSync {
  const DELAY = 4000, RETRY = 15000, MAX = 16e6;
  let timer = 0, sending = false, dirty = false, off = false;
  const fmt = () => new Date().toLocaleTimeString('cs-CZ', { timeStyle: 'short' });
  async function send() {
    timer = 0;
    if (sending || off) return;
    sending = true; dirty = false;
    try {
      const files = await folder(dirName);
      if (!files) { dirty = true; timer = setTimeout(send, RETRY) as unknown as number; return; }
      if (!files.length) return;
      const blob = zipMake(files);
      if (blob.size > MAX) { off = true; show('Postup je příliš velký pro server (limit 16 MB)'); return; }
      const r = await fetch('/saves/' + dirName, { method: 'PUT', body: blob, headers: { 'X-Saves-Mtime': String(files.reduce((m, f) => Math.max(m, f.mtime), 0)) } });
      if (r.status === 404 || r.status === 405) { off = true; return; }          // a server that keeps no progress
      if (!r.ok) throw new Error('HTTP ' + r.status);
      note(`[saves] progress sent to the server: ${files.length} files, ${Math.round(blob.size / 1024)} KB`);
      show('Postup uložen na server ' + fmt());
    } catch (e) {
      dirty = true; note('[saves] sending the progress failed: ' + e); show('Postup se nepodařilo poslat, zkusím znovu');
      timer = setTimeout(send, RETRY) as unknown as number;
    } finally {
      sending = false;
      if (dirty && !timer && !off) timer = setTimeout(send, DELAY) as unknown as number;
    }
  }
  addEventListener('beforeunload', e => { if (!off && (dirty || sending || timer)) { e.preventDefault(); e.returnValue = ''; if (!sending) { clearTimeout(timer); void send(); } } });
  return {
    saved(path: string) {
      if (off || path.startsWith('TempReplay')) return;                            // the replay of the game being played: not progress
      dirty = true;
      if (!sending) { clearTimeout(timer); timer = setTimeout(send, DELAY) as unknown as number; }
      show('Postup se ukládá na server…');
    },
    state: () => sending ? 'sending' : dirty || timer ? 'waiting' : 'idle',
  };
}
