/* kbd.h -- keyboard and mouse input between the host and the guest.
 *
 * Producers: the AppKit main thread (runtime/keyboard_mac.m: real and
 * synthesised NSEvents; metal.m: mouse), the autotest script (guest thread)
 * and the old AUTOSEQ/AUTOKEY test hooks.  Consumers: the DirectInput
 * keyboard device (runtime/dinput_kbd.c, guest thread) and the guest's
 * window message queue (win32_ext.c), which kbd_pump() fills from
 * PeekMessageA/GetMessageA so that queue stays single-threaded. */
#pragma once
#include <stdint.h>

/* ---- key state (DirectInput scan codes, DIK_*) ---------------------------
 * A key is down while any source holds it.  Every change of that combined
 * state is also recorded as an event for DirectInput's buffered data. */
enum { KSRC_HOST = 0, KSRC_SCRIPT = 1, KSRC_LEGACY = 2 };
void kbd_set_key(int src, unsigned dik, int down);       /* any thread */
void kbd_release_source(int src);                        /* every key of src up */
void kbd_state(uint8_t *dst, unsigned n);                /* 0x80 = down */
int  kbd_key_down(unsigned dik);

typedef struct { uint8_t dik, down; uint32_t ms, seq; } KbdEv;
uint32_t kbd_ev_head(void);                  /* sequence number of the next event */
/* 1 = event seq returned, 0 = not recorded yet, -1 = already overwritten */
int      kbd_ev_get(uint32_t seq, KbdEv *e);

/* ---- window messages from the host -------------------------------------- */
void kbd_post(uint32_t msg, uint32_t wp, uint32_t lp);   /* any thread */
void kbd_post_mousemove(uint32_t wp, uint32_t lp);       /* coalesced, as Windows does */
void kbd_pump(void);                                     /* guest thread */

/* NFSU2_KEY_LOG=1: log host key events and the messages the game gets */
int  kbd_log(void);
const char *kbd_msg_name(uint32_t msg);

/* ---- runtime/keyboard_mac.m --------------------------------------------- */
/* The key's name on the current keyboard layout for the Controls page, or
 * 0 (then runtime/dikeys.c's US name is used).  Any thread. */
const char *kbdmac_layout_name(unsigned dik);
/* autotest: synthesised NSEvents, posted into AppKit's event queue on the
 * main thread so they take the same path as real keys.  Guest thread. */
enum { KSYN_DOWN, KSYN_UP, KSYN_REPEAT, KSYN_CHAR_DOWN, KSYN_CHAR_UP,
       KSYN_FOCUS_OUT, KSYN_FOCUS_IN, KSYN_MOUSE_MOVE, KSYN_MOUSE_DOWN, KSYN_MOUSE_UP,
       KSYN_SCROLL };
/* key: name (A, RETURN, LSHIFT, LCMD, kc:0x0c ...) with optional modifier
 * prefixes "cmd+", "shift+", "ctrl+", "opt+"; returns 0 if unknown */
int  kbdmac_synth_key(int type, const char *key);
int  kbdmac_synth_char(int type, uint32_t unicode);      /* 0 if no key types it */
void kbdmac_synth_simple(int type, double a, double b);  /* focus, mouse */
