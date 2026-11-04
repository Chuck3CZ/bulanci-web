/* keyboard.c -- key state, DirectInput key events and host window messages.
 *
 * Key state: one bit per source and key (kbd.h); a key is down for
 * DirectInput while any source holds it.  Each change of the combined state
 * goes into a ring of events with the game's clock (GetTickCount) and a
 * sequence number, from which every DirectInput keyboard device reads its
 * buffered data at its own pace (runtime/dinput_kbd.c).  So a key pressed
 * and released between two of the game's polls is still reported, as
 * DirectInput's buffer does, instead of being lost to a state comparison.
 *
 * Window messages made by the host (WM_KEYDOWN/WM_CHAR/..., mouse) are
 * queued here under a lock and moved into the guest's own message queue by
 * kbd_pump() on the guest thread, from PeekMessageA/GetMessageA.  Mouse
 * moves are coalesced into the newest one, as Windows does.
 * Everything here may be called from any thread. */
#include "kbd.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t host_ms(void);                    /* win32.c: GetTickCount's clock */
void win32_post_message(uint32_t msg, uint32_t wp, uint32_t lp);   /* win32_ext.c */
int  win32_msg_room(void);                                       /* win32_ext.c */

int kbd_log(void){
    static int l = -1;
    if (l < 0){ const char *e = getenv("NFSU2_KEY_LOG"); l = (e && *e && *e != '0') ? 1 : 0; }
    return l;
}
const char *kbd_msg_name(uint32_t m){
    switch (m){
    case 0x0100: return "WM_KEYDOWN";     case 0x0101: return "WM_KEYUP";
    case 0x0102: return "WM_CHAR";        case 0x0103: return "WM_DEADCHAR";
    case 0x0104: return "WM_SYSKEYDOWN";  case 0x0105: return "WM_SYSKEYUP";
    case 0x0106: return "WM_SYSCHAR";     case 0x0200: return "WM_MOUSEMOVE";
    case 0x0201: return "WM_LBUTTONDOWN"; case 0x0202: return "WM_LBUTTONUP";
    case 0x0204: return "WM_RBUTTONDOWN"; case 0x0205: return "WM_RBUTTONUP";
    case 0x0207: return "WM_MBUTTONDOWN"; case 0x0208: return "WM_MBUTTONUP";
    case 0x020A: return "WM_MOUSEWHEEL";
    default:     return "WM_?";
    }
}

/* ---- key state and DirectInput events ----------------------------------- */
static pthread_mutex_t g_kmx = PTHREAD_MUTEX_INITIALIZER;
static uint8_t  g_src[256];              /* bit per KSRC_* */
#define EVN 1024u                        /* power of two */
static KbdEv    g_ev[EVN];
static uint32_t g_seq = 1;               /* next sequence number (0 = never used) */

static void push_ev_locked(unsigned dik, int down){
    KbdEv *e = &g_ev[g_seq & (EVN - 1u)];
    e->dik = (uint8_t)dik; e->down = (uint8_t)(down ? 1 : 0);
    e->ms = host_ms();                   /* dwTimeStamp in GetTickCount time */
    e->seq = g_seq++;
}
void kbd_set_key(int src, unsigned dik, int down){
    if (dik == 0 || dik > 255u || src < 0 || src > 7) return;
    uint8_t bit = (uint8_t)(1u << src);
    pthread_mutex_lock(&g_kmx);
    int was = g_src[dik] != 0;
    if (down) g_src[dik] |= bit; else g_src[dik] &= (uint8_t)~bit;
    int now = g_src[dik] != 0;
    if (now != was) push_ev_locked(dik, now);
    pthread_mutex_unlock(&g_kmx);
}
void kbd_release_source(int src){
    if (src < 0 || src > 7) return;
    uint8_t bit = (uint8_t)(1u << src);
    pthread_mutex_lock(&g_kmx);
    for (unsigned k = 1; k < 256u; k++)
        if (g_src[k] & bit){
            g_src[k] &= (uint8_t)~bit;
            if (!g_src[k]) push_ev_locked(k, 0);
        }
    pthread_mutex_unlock(&g_kmx);
}
void kbd_state(uint8_t *dst, unsigned n){
    if (n > 256u) n = 256u;
    pthread_mutex_lock(&g_kmx);
    for (unsigned k = 0; k < n; k++) dst[k] = g_src[k] ? 0x80 : 0x00;
    pthread_mutex_unlock(&g_kmx);
}
int kbd_key_down(unsigned dik){
    if (dik > 255u) return 0;
    pthread_mutex_lock(&g_kmx);
    int d = g_src[dik] != 0;
    pthread_mutex_unlock(&g_kmx);
    return d;
}
uint32_t kbd_ev_head(void){
    pthread_mutex_lock(&g_kmx);
    uint32_t s = g_seq;
    pthread_mutex_unlock(&g_kmx);
    return s;
}
int kbd_ev_get(uint32_t seq, KbdEv *e){
    pthread_mutex_lock(&g_kmx);
    int r;
    if ((int32_t)(seq - g_seq) >= 0) r = 0;
    else if (g_seq - seq > EVN) r = -1;
    else { *e = g_ev[seq & (EVN - 1u)]; r = 1; }
    pthread_mutex_unlock(&g_kmx);
    return r;
}

/* ---- host -> guest window messages --------------------------------------- */
#define MQN 1024u
static struct { uint32_t m, wp, lp; } g_mq[MQN];
static unsigned g_mq_h = 0, g_mq_t = 0;   /* head (read), tail (write) */
static pthread_mutex_t g_mmx = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_mq_drop = 0;

static void post_locked(uint32_t m, uint32_t wp, uint32_t lp){
    unsigned nx = (g_mq_t + 1u) % MQN;
    if (nx == g_mq_h){ g_mq_drop++; return; }
    g_mq[g_mq_t].m = m; g_mq[g_mq_t].wp = wp; g_mq[g_mq_t].lp = lp;
    g_mq_t = nx;
}
void kbd_post(uint32_t m, uint32_t wp, uint32_t lp){
    pthread_mutex_lock(&g_mmx);
    post_locked(m, wp, lp);
    pthread_mutex_unlock(&g_mmx);
}
void kbd_post_mousemove(uint32_t wp, uint32_t lp){
    pthread_mutex_lock(&g_mmx);
    unsigned last = (g_mq_t + MQN - 1u) % MQN;
    if (g_mq_h != g_mq_t && g_mq[last].m == 0x0200u){ g_mq[last].wp = wp; g_mq[last].lp = lp; }
    else post_locked(0x0200u, wp, lp);
    pthread_mutex_unlock(&g_mmx);
}
void kbd_pump(void){
    int room = win32_msg_room();
    for (;;){
        uint32_t m, wp, lp;
        pthread_mutex_lock(&g_mmx);
        if (g_mq_h == g_mq_t || room <= 8){ pthread_mutex_unlock(&g_mmx); break; }
        m = g_mq[g_mq_h].m; wp = g_mq[g_mq_h].wp; lp = g_mq[g_mq_h].lp;
        g_mq_h = (g_mq_h + 1u) % MQN;
        pthread_mutex_unlock(&g_mmx);
        win32_post_message(m, wp, lp);
        room--;
        if (kbd_log() && m != 0x0200u)
            fprintf(stderr, "[key] guest gets %-13s wParam 0x%08x lParam 0x%08x\n", kbd_msg_name(m), wp, lp);
    }
    if (g_mq_drop){ static uint64_t said = 0;
        if (said != g_mq_drop){ said = g_mq_drop;
            fprintf(stderr, "[key] host message queue full: %llu message(s) dropped\n",
                    (unsigned long long)g_mq_drop); } }
}
