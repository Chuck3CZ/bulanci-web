// The autotest's "real input" commands (nsdown, nstap, nstype, nsmouse,
// nsfocus ...; runtime/autotest.c) arrive as requests in the mailbox's
// synthetic ring.  On the Mac they became NSEvents posted into AppKit's
// queue; here they become DOM events dispatched on the game element, so they
// go through the same handlers as a person's keys and mouse (index.ts),
// including the browser-specific rules (Meta, Caps Lock on macOS, focus).
// Synthetic events are untrusted: they never trigger browser defaults.
import { CODE_OF_DIK, keyFor, strokeFor } from './keymap';
import {
  ISYN_ALT, ISYN_CTRL, ISYN_META, ISYN_SHIFT, KSYN_CHAR_DOWN, KSYN_CHAR_UP, KSYN_DOWN, KSYN_FOCUS_IN, KSYN_FOCUS_OUT,
  KSYN_MOUSE_DOWN, KSYN_MOUSE_MOVE, KSYN_MOUSE_UP, KSYN_REPEAT, KSYN_SCROLL, KSYN_UP,
} from './proto';

export interface SynReq { type: number; dik: number; mods: number; ch: number; a: number; b: number }
export interface SynthHooks {
  element: HTMLElement;
  mac: boolean;
  layout(): Map<string, string> | null;
  focus(active: boolean): void;          // the page's own blur/focus handling, as for a real switch
  log(s: string): void;
}

const MOD_CODES: [number, string][] = [[ISYN_META, 'MetaLeft'], [ISYN_SHIFT, 'ShiftLeft'], [ISYN_CTRL, 'ControlLeft'], [ISYN_ALT, 'AltLeft']];

export class SynthDriver {
  private held = new Set<string>();      // modifier codes held by synthetic events
  private caps = false;
  private mx = 0.5; private my = 0.5; private buttons = 0;
  constructor(private o: SynthHooks) {}

  private mods() {
    const has = (a: string, b: string) => this.held.has(a) || this.held.has(b);
    return { shiftKey: has('ShiftLeft', 'ShiftRight'), ctrlKey: has('ControlLeft', 'ControlRight'),
      altKey: has('AltLeft', 'AltRight'), metaKey: has('MetaLeft', 'MetaRight') };
  }
  private key(type: 'keydown' | 'keyup', code: string, repeat = false) {
    const m = this.mods();
    const ev = new KeyboardEvent(type, {
      code, key: keyFor(code, m.shiftKey, this.caps, this.o.mac, this.o.layout()), repeat, ...m,
      modifierCapsLock: this.caps, bubbles: true, cancelable: true, composed: true,
    });
    this.o.element.dispatchEvent(ev);
  }
  /** a modifier or Caps Lock going down or up, with the modifier state the browser would report */
  private modifier(code: string, down: boolean) {
    if (code === 'CapsLock') {
      // macOS: one event per toggle, keydown when it turns on, keyup when it
      // turns off; elsewhere a normal press and release
      if (this.o.mac) { if (!down) return; this.caps = !this.caps; this.key(this.caps ? 'keydown' : 'keyup', code); }
      else { if (down) this.caps = !this.caps; this.key(down ? 'keydown' : 'keyup', code); }
      return;
    }
    if (down) { this.held.add(code); this.key('keydown', code); }
    else { this.held.delete(code); this.key('keyup', code); }
  }
  private isMod(code: string) { return code === 'CapsLock' || /^(Shift|Control|Alt|Meta)(Left|Right)$/.test(code); }
  private press(code: string, type: number) {
    if (this.isMod(code)) { if (type !== KSYN_REPEAT) this.modifier(code, type !== KSYN_UP); return; }
    if (type === KSYN_UP) this.key('keyup', code); else this.key('keydown', code, type === KSYN_REPEAT);
  }
  private pointer(type: string, extra: PointerEventInit = {}) {
    const rc = this.o.element.getBoundingClientRect();
    const init: PointerEventInit = { clientX: rc.left + this.mx * rc.width, clientY: rc.top + this.my * rc.height,
      pointerId: 1, pointerType: 'mouse', isPrimary: true, buttons: this.buttons, bubbles: true, cancelable: true,
      composed: true, ...this.mods(), ...extra };
    this.o.element.dispatchEvent(type === 'wheel' ? new WheelEvent(type, init as WheelEventInit) : new PointerEvent(type, init));
  }

  handle(q: SynReq) {
    switch (q.type) {
      case KSYN_DOWN: case KSYN_UP: case KSYN_REPEAT: {
        const code = CODE_OF_DIK[q.dik];
        if (!code) { this.o.log(`[key] synthetic key: no DOM code for DIK 0x${q.dik.toString(16)}`); return; }
        const mods = MOD_CODES.filter(([bit]) => q.mods & bit).map(([, c]) => c);
        // "cmd+shift+Q": modifiers down in order, the key, and on release the reverse
        if (q.type === KSYN_DOWN) for (const c of mods) this.modifier(c, true);
        this.press(code, q.type);
        if (q.type === KSYN_UP) for (const c of mods.reverse()) this.modifier(c, false);
        return;
      }
      case KSYN_CHAR_DOWN: case KSYN_CHAR_UP: {
        const ch = String.fromCodePoint(q.ch);
        const s = strokeFor(ch, this.o.layout());
        if (!s) {
          if (q.type === KSYN_CHAR_DOWN)
            this.o.log(`[key] nstype: no key types U+${q.ch.toString(16).toUpperCase().padStart(4, '0')} on this layout`);
          return;
        }
        if (q.type === KSYN_CHAR_DOWN) { if (s.shift) this.modifier('ShiftLeft', true); this.key('keydown', s.code); }
        else { this.key('keyup', s.code); if (s.shift) this.modifier('ShiftLeft', false); }
        return;
      }
      case KSYN_FOCUS_OUT: case KSYN_FOCUS_IN: this.o.focus(q.type === KSYN_FOCUS_IN); return;
      case KSYN_MOUSE_MOVE: this.mx = q.a; this.my = q.b; this.pointer('pointermove'); return;
      case KSYN_MOUSE_DOWN: case KSYN_MOUSE_UP: {
        // the script's 0 left, 1 right, 2 middle -> DOM button numbers and the buttons mask
        const b = q.a | 0, dom = b === 1 ? 2 : b === 2 ? 1 : 0, bit = b === 1 ? 2 : b === 2 ? 4 : 1;
        if (q.type === KSYN_MOUSE_DOWN) this.buttons |= bit; else this.buttons &= ~bit;
        this.pointer(q.type === KSYN_MOUSE_DOWN ? 'pointerdown' : 'pointerup', { button: dom });
        return;
      }
      case KSYN_SCROLL: this.pointer('wheel', { deltaY: -q.a, deltaMode: 1 } as WheelEventInit); return;
    }
  }
}
