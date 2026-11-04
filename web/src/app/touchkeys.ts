// Touch controls for a phone or tablet: buttons over the game that press the game's keyboard keys
// (CMR 2005 drives and navigates its menus by keyboard; it sees no DirectInput pad yet).  Each button holds its
// key while a finger is on it (several at once: steer and throttle).  The layout: steering bottom left, throttle /
// brake / handbrake bottom right, Enter / Escape / gears along the top.

export interface KeySink { key(code: string, down: boolean): void }

interface Btn { label: string; code: string; css: string; big?: boolean }

const BUTTONS: Btn[] = [
  { label: '◀', code: 'ArrowLeft',  css: 'left:2%;bottom:6%', big: true },
  { label: '▶', code: 'ArrowRight', css: 'left:17%;bottom:6%', big: true },
  { label: 'Plyn',   code: 'ArrowUp',   css: 'right:2%;bottom:30%', big: true },
  { label: 'Brzda',  code: 'ArrowDown', css: 'right:2%;bottom:6%', big: true },
  { label: 'Ruční',  code: 'Space',     css: 'right:17%;bottom:6%', big: true },
  { label: 'Enter',  code: 'Enter',     css: 'left:2%;top:3%' },
  { label: 'Esc',    code: 'Escape',    css: 'left:13%;top:3%' },
  { label: '▲ kvalt', code: 'KeyA',     css: 'right:13%;top:3%' },
  { label: '▼ kvalt', code: 'KeyZ',     css: 'right:2%;top:3%' },
];

/** is this a touch-first device (a phone or a tablet), or did the page ask (?touch) */
export function touchWanted(q: URLSearchParams): boolean {
  if (q.has('notouch')) return false;
  return q.has('touch') || (typeof matchMedia === 'function' && matchMedia('(pointer: coarse)').matches);
}

/** the buttons, in an overlay over `host` (which must be position:relative or fixed) */
export function attachTouchKeys(host: HTMLElement, sink: KeySink): { show(on: boolean): void } {
  const layer = document.createElement('div');
  layer.style.cssText = 'position:absolute;inset:0;pointer-events:none;user-select:none;-webkit-user-select:none;touch-action:none;z-index:5';
  for (const b of BUTTONS) {
    const el = document.createElement('div');
    const size = b.big ? 'width:13vmin;height:13vmin;font-size:4vmin' : 'width:10vmin;height:7vmin;font-size:2.8vmin';
    el.style.cssText = `position:absolute;${b.css};${size};display:flex;align-items:center;justify-content:center;` +
      'border-radius:2vmin;background:rgba(20,24,32,.45);border:2px solid rgba(255,255,255,.55);color:#fff;' +
      'font-family:system-ui,sans-serif;font-weight:600;pointer-events:auto;touch-action:none';
    el.textContent = b.label;
    const fingers = new Set<number>();
    const down = (e: PointerEvent) => {
      e.preventDefault(); e.stopPropagation();
      try { el.setPointerCapture(e.pointerId); } catch { /* an old browser */ }
      if (!fingers.size) { sink.key(b.code, true); el.style.background = 'rgba(217,164,65,.7)'; }
      fingers.add(e.pointerId);
    };
    const up = (e: PointerEvent) => {
      e.preventDefault(); e.stopPropagation();
      if (!fingers.delete(e.pointerId)) return;
      if (!fingers.size) { sink.key(b.code, false); el.style.background = 'rgba(20,24,32,.45)'; }
    };
    el.addEventListener('pointerdown', down);
    el.addEventListener('pointerup', up);
    el.addEventListener('pointercancel', up);
    el.addEventListener('lostpointercapture', up);
    el.addEventListener('contextmenu', e => e.preventDefault());
    layer.append(el);
  }
  host.append(layer);
  return { show(on) { layer.style.display = on ? '' : 'none'; } };
}
