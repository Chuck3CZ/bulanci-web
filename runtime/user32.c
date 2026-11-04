/* user32.c -- windows and window messages for WC3 (the USER32 HLE).
 *
 * nfsu2-web's win32_ext.c had one window, one window procedure (the last
 * class registered), PostMessage/SendMessage that did nothing and a window
 * that never got WM_CREATE: enough for NFSU2, which reads DirectInput.  WC3
 * runs on its window procedures: Game.dll's GetMessage/DispatchMessage loop
 * (0x0152baf0), its CGxDevice window (wndproc 0x0152cd00), timers and posted
 * messages.  Here:
 *   - classes (RegisterClass[Ex]A) and windows (CreateWindowExA) with their
 *     own procedure (GWL_WNDPROC), styles, extra bytes, user data, props;
 *   - CreateWindowExA sends WM_NCCREATE and WM_CREATE with a CREATESTRUCTA;
 *     ShowWindow sends WM_SHOWWINDOW, the activation (WM_ACTIVATEAPP,
 *     WM_NCACTIVATE, WM_ACTIVATE, WM_SETFOCUS), WM_SIZE and WM_MOVE, and
 *     posts WM_PAINT;
 *   - SendMessageA calls the procedure; PostMessageA / PostThreadMessageA /
 *     PostQuitMessage queue (one queue: the game's windows all live on its
 *     main thread); GetMessageA waits (the guest lock released) for a
 *     message, a due timer or the host's input; WM_QUIT ends GetMessage;
 *   - DefWindowProcA's defaults, CallWindowProcA, BeginPaint/EndPaint;
 *   - SetTimer/KillTimer: a due timer is one WM_TIMER when the queue is empty.
 * Windows sit at the top left of a virtual screen the size of the game's
 * device (no frames: window rect = client rect).
 * The host's keyboard, mouse and focus messages (keyboard.c, lifecycle.c:
 * win32_post_message) go to the focus window. */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
uint32_t gheap_alloc(int heap, uint32_t n, int zero);
void guest_sleep_ns(int64_t ns);
uint64_t host_us(void);
void kbd_pump(void); void lifecycle_pump(void); int lifecycle_may_park(void);
void metal_game_size(unsigned *w, unsigned *h);
void metal_get_cursor(int *x, int *y);
void win32_set_last_error(CPU *c, uint32_t e);

#define WM_CREATE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_MOVE 0x0003u
#define WM_SIZE 0x0005u
#define WM_ACTIVATE 0x0006u
#define WM_SETFOCUS 0x0007u
#define WM_KILLFOCUS 0x0008u
#define WM_PAINT 0x000Fu
#define WM_CLOSE 0x0010u
#define WM_QUIT 0x0012u
#define WM_ERASEBKGND 0x0014u
#define WM_SHOWWINDOW 0x0018u
#define WM_ACTIVATEAPP 0x001Cu
#define WM_SETCURSOR 0x0020u
#define WM_GETMINMAXINFO 0x0024u
#define WM_NCCREATE 0x0081u
#define WM_NCDESTROY 0x0082u
#define WM_NCHITTEST 0x0084u
#define WM_NCACTIVATE 0x0086u
#define WM_SYSCOMMAND 0x0112u
#define WM_TIMER 0x0113u

#define NCLASS 32
#define NWIN 64
#define HWND_BASE 0x00B00100u          /* window handles: HWND_BASE + 4 * slot */
static struct { char name[64]; uint16_t atom; uint32_t proc, style, extra, inst; } g_cls[NCLASS];
static int g_ncls;
typedef struct {
    int used, visible, cls;
    uint32_t proc, style, exstyle, parent, id, inst, userdata;
    int32_t x, y, w, h;
    uint32_t extra;                     /* guest block of the class's cbWndExtra bytes */
    uint32_t extrasz;
    char text[128];
    struct { char name[32]; uint32_t v; } prop[8];
} Win;
static Win g_win[NWIN];
static uint32_t g_focus, g_active, g_main;          /* hwnds */
static uint32_t g_paint_pending[NWIN];

static Win *win_of(uint32_t h){
    uint32_t k = (h - HWND_BASE) / 4u;
    return (h >= HWND_BASE && (h - HWND_BASE) % 4u == 0 && k < NWIN && g_win[k].used) ? &g_win[k] : 0;
}
static uint32_t hwnd_of(const Win *w){ return HWND_BASE + 4u * (uint32_t)(w - g_win); }

static int msglog(void); static uint32_t ctid(CPU *c);
static uint32_t send_msg(CPU *c, uint32_t h, uint32_t m, uint32_t wp, uint32_t lp){
    Win *w = win_of(h);
    if (!w || !w->proc) return 0;
    if (msglog()) fprintf(stderr, "[msg] send   tid %u  hwnd 0x%08x msg 0x%04x wp 0x%08x lp 0x%08x\n", ctid(c), h, m, wp, lp);
    const uint32_t a[4] = { h, m, wp, lp };
    return call_guest(c, w->proc, 4, a);
}

/* ---- the queue ---- */
#define QN 512
static struct { uint32_t h, m, wp, lp, t; } g_q[QN];
static int g_qh, g_qt, g_quit_posted; static uint32_t g_quit_code;
static int q_room(void){ return (g_qh - g_qt - 1 + QN) % QN; }
static void q_post(uint32_t h, uint32_t m, uint32_t wp, uint32_t lp){
    if (!q_room()){ static int said; if (!said++) fprintf(stderr, "[user32] message queue full, message 0x%x dropped\n", m); return; }
    g_q[g_qt].h = h; g_q[g_qt].m = m; g_q[g_qt].wp = wp; g_q[g_qt].lp = lp; g_q[g_qt].t = (uint32_t)(host_us() / 1000u);
    g_qt = (g_qt + 1) % QN;
}
/* keyboard.c / lifecycle.c (win32_ext.c forwards win32_post_message here) */
void user32_post_input(uint32_t m, uint32_t wp, uint32_t lp){ q_post(g_focus ? g_focus : g_main, m, wp, lp); }
int user32_room(void){ return q_room(); }
uint32_t user32_main_window(void){ return g_main; }

/* ---- timers ---- */
#define NTIMER 32
static struct { uint32_t h, id, ms, proc; uint64_t due; } g_tm[NTIMER];
static uint32_t timer_set(uint32_t h, uint32_t id, uint32_t ms, uint32_t proc){
    static uint32_t next = 0x7000u;
    if (ms < 10u) ms = 10u;                                   /* USER_TIMER_MINIMUM */
    if (!h) id = id ? id : next++;
    int k = -1;
    for (int i = 0; i < NTIMER; i++) if (g_tm[i].ms && g_tm[i].h == h && g_tm[i].id == id){ k = i; break; }
    if (k < 0) for (int i = 0; i < NTIMER; i++) if (!g_tm[i].ms){ k = i; break; }
    if (k < 0) return 0;
    g_tm[k].h = h; g_tm[k].id = id; g_tm[k].ms = ms; g_tm[k].proc = proc; g_tm[k].due = host_us() + (uint64_t)ms * 1000u;
    return id ? id : 1u;
}
static int timer_kill(uint32_t h, uint32_t id){
    for (int i = 0; i < NTIMER; i++) if (g_tm[i].ms && g_tm[i].h == h && g_tm[i].id == id){ g_tm[i].ms = 0; return 1; }
    return 0;
}
static int timer_due(uint32_t *h, uint32_t *id, uint32_t *proc, int remove){
    uint64_t now = host_us();
    for (int i = 0; i < NTIMER; i++)
        if (g_tm[i].ms && now >= g_tm[i].due){
            *h = g_tm[i].h; *id = g_tm[i].id; *proc = g_tm[i].proc;
            if (remove) g_tm[i].due = now + (uint64_t)g_tm[i].ms * 1000u;
            return 1;
        }
    return 0;
}
static int64_t timer_next_us(void){
    uint64_t now = host_us(); int64_t best = -1;
    for (int i = 0; i < NTIMER; i++) if (g_tm[i].ms){
        int64_t d = (int64_t)(g_tm[i].due - now); if (d < 0) d = 0;
        if (best < 0 || d < best) best = d; }
    return best;
}

static void write_msg(uint32_t pm, uint32_t h, uint32_t m, uint32_t wp, uint32_t lp, uint32_t t){
    if (!pm) return;
    mem_commit(pm, 28);
    int cx = 0, cy = 0; metal_get_cursor(&cx, &cy);
    MR32(pm) = h; MR32(pm+4) = m; MR32(pm+8) = wp; MR32(pm+12) = lp; MR32(pm+16) = t;
    MR32(pm+20) = (uint32_t)cx; MR32(pm+24) = (uint32_t)cy;
}
/* one message for GetMessage/PeekMessage (filter hwnd/min/max ignored but WM_QUIT); 0 = none */
static int take_msg(uint32_t pm, int remove){
    if (g_qh != g_qt){
        write_msg(pm, g_q[g_qh].h, g_q[g_qh].m, g_q[g_qh].wp, g_q[g_qh].lp, g_q[g_qh].t);
        if (remove) g_qh = (g_qh + 1) % QN;
        return 1;
    }
    if (g_quit_posted){
        write_msg(pm, 0, WM_QUIT, g_quit_code, 0, (uint32_t)(host_us() / 1000u));
        if (remove) g_quit_posted = 0;
        return 1;
    }
    for (int k = 0; k < NWIN; k++) if (g_paint_pending[k] && g_win[k].used){
        write_msg(pm, hwnd_of(&g_win[k]), WM_PAINT, 0, 0, (uint32_t)(host_us() / 1000u));
        if (remove) g_paint_pending[k] = 0;
        return 1;
    }
    uint32_t th = 0, tid = 0, tp = 0;
    if (timer_due(&th, &tid, &tp, remove)){ write_msg(pm, th, WM_TIMER, tid, tp, (uint32_t)(host_us() / 1000u)); return 1; }
    return 0;
}

static uint32_t A(CPU *c, int i){ return MR32(c->esp + 4u*(uint32_t)i); }
/* WC3_MSGLOG=1: messages taken (and by which thread), posted, sent, and the
 * window calls around them; the first WC3_MSGLOG_MAX (default 400) lines */
static int msglog(void){
    static int v = -1, n = 0, max = 400;
    if (v < 0){ const char *e = getenv("WC3_MSGLOG"); v = e && *e == '1';
                const char *m = getenv("WC3_MSGLOG_MAX"); if (m && *m) max = atoi(m); }
    return v && n++ < max;
}
static uint32_t ctid(CPU *c){ return (c && c->fs_base && mem_is_committed(c->fs_base + 0x24u)) ? MR32(c->fs_base + 0x24u) : 0u; }
static void cstr(uint32_t p, char *out, size_t n){
    out[0] = 0;
    if (p > 0xFFFFu && mem_is_committed(p)) snprintf(out, n, "%s", (const char *)(g_mem + p));
    else if (p) snprintf(out, n, "#%u", p);             /* an atom */
}
static int class_find(uint32_t p){
    char nm[64]; cstr(p, nm, sizeof nm);
    for (int i = 0; i < g_ncls; i++)
        if ((p <= 0xFFFFu && g_cls[i].atom == p) || !strcasecmp(g_cls[i].name, nm)) return i;
    return -1;
}

static void activate(CPU *c, uint32_t h){
    if (g_active == h) return;
    uint32_t old = g_active;
    if (!old) send_msg(c, h, WM_ACTIVATEAPP, 1, 0);
    send_msg(c, h, WM_NCACTIVATE, 1, 0);
    send_msg(c, h, WM_ACTIVATE, 1, old);
    g_active = h;
    uint32_t of = g_focus; g_focus = h;
    if (of && of != h) send_msg(c, of, WM_KILLFOCUS, h, 0);
    send_msg(c, h, WM_SETFOCUS, of, 0);
}

int user32_call(CPU *c, const char *n, uint32_t *out){
    uint32_t r = 0;
#define IS(x) (!strcmp(n, x))
    if (IS("RegisterClassA") || IS("RegisterClassExA")){
        /* WNDCLASSEXA: cbSize style proc cbClsExtra cbWndExtra hInstance ... lpszClassName@40;
         * WNDCLASSA: style proc cbClsExtra cbWndExtra hInstance ... lpszClassName@36 */
        uint32_t wc = A(c,0), ex = IS("RegisterClassExA");
        if (!wc || g_ncls >= NCLASS){ *out = 0; return 1; }
        uint32_t o = ex ? 4u : 0u;
        int k = g_ncls++;
        g_cls[k].style = MR32(wc + o); g_cls[k].proc = MR32(wc + o + 4); g_cls[k].extra = MR32(wc + o + 12);
        g_cls[k].inst = MR32(wc + o + 16);
        cstr(MR32(wc + (ex ? 40u : 36u)), g_cls[k].name, sizeof g_cls[k].name);
        g_cls[k].atom = (uint16_t)(0xC000u + k);
        fprintf(stderr, "[user32] RegisterClass \"%s\" wndproc 0x%08x extra %u\n", g_cls[k].name, g_cls[k].proc, g_cls[k].extra);
        r = g_cls[k].atom;
    }
    else if (IS("UnregisterClassA")) r = 1;
    else if (IS("GetClassInfoA") || IS("GetClassInfoExA")){
        int k = class_find(A(c,1)); uint32_t wc = A(c,2);
        if (k < 0){ win32_set_last_error(c, 1411); r = 0; }              /* ERROR_CLASS_DOES_NOT_EXIST */
        else { uint32_t o = IS("GetClassInfoExA") ? 4u : 0u; if (wc){ MR32(wc+o) = g_cls[k].style; MR32(wc+o+4) = g_cls[k].proc; MR32(wc+o+12) = g_cls[k].extra; }
               r = IS("GetClassInfoExA") ? g_cls[k].atom : 1u; }
    }
    else if (IS("CreateWindowExA")){
        /* (exstyle, class, name, style, x, y, w, h, parent, menu, instance, param) */
        int k = class_find(A(c,1)), slot = -1;
        for (int i = 0; i < NWIN; i++) if (!g_win[i].used){ slot = i; break; }
        if (k < 0 || slot < 0){
            char nm[64]; cstr(A(c,1), nm, sizeof nm);
            fprintf(stderr, "[user32] CreateWindowExA: %s \"%s\"\n", k < 0 ? "no class" : "no free window slot", nm);
            win32_set_last_error(c, 1407); *out = 0; return 1;              /* ERROR_CANNOT_FIND_WND_CLASS */
        }
        Win *w = &g_win[slot]; memset(w, 0, sizeof *w);
        w->used = 1; w->cls = k; w->proc = g_cls[k].proc;
        w->exstyle = A(c,0); w->style = A(c,3); w->parent = A(c,8); w->id = A(c,9); w->inst = A(c,10);
        unsigned gw = 1280, gh = 720; metal_game_size(&gw, &gh); if (!gw || !gh){ gw = 1280; gh = 720; }
        int32_t x = (int32_t)A(c,4), y = (int32_t)A(c,5), ww = (int32_t)A(c,6), hh = (int32_t)A(c,7);
        if (x == (int32_t)0x80000000) x = 0;
        if (y == (int32_t)0x80000000) y = 0;
        if (ww == (int32_t)0x80000000 || ww <= 0) ww = (int32_t)gw;
        if (hh == (int32_t)0x80000000 || hh <= 0) hh = (int32_t)gh;
        w->x = x; w->y = y; w->w = ww; w->h = hh;
        cstr(A(c,2), w->text, sizeof w->text);
        if (g_cls[k].extra){ w->extrasz = g_cls[k].extra; w->extra = gheap_alloc(1, w->extrasz + 4u, 1); }
        uint32_t h = hwnd_of(w);
        if (!w->parent && !g_main) g_main = h;
        fprintf(stderr, "[user32] CreateWindowExA \"%s\" class \"%s\" %dx%d at %d,%d style 0x%08x -> 0x%08x\n",
                w->text, g_cls[k].name, ww, hh, x, y, w->style, h);
        uint32_t cs = gheap_alloc(1, 48, 1);
        MR32(cs) = A(c,11); MR32(cs+4) = w->inst; MR32(cs+8) = A(c,9); MR32(cs+12) = w->parent;
        MR32(cs+16) = (uint32_t)hh; MR32(cs+20) = (uint32_t)ww; MR32(cs+24) = (uint32_t)y; MR32(cs+28) = (uint32_t)x;
        MR32(cs+32) = w->style; MR32(cs+36) = A(c,2); MR32(cs+40) = A(c,1); MR32(cs+44) = w->exstyle;
        if (!send_msg(c, h, WM_NCCREATE, 0, cs)){ fprintf(stderr, "[user32] WM_NCCREATE refused the window\n"); w->used = 0; if (g_main == h) g_main = 0; *out = 0; return 1; }
        if (send_msg(c, h, WM_CREATE, 0, cs) == 0xFFFFFFFFu){ fprintf(stderr, "[user32] WM_CREATE refused the window\n"); w->used = 0; if (g_main == h) g_main = 0; *out = 0; return 1; }
        if (w->style & 0x10000000u){                          /* WS_VISIBLE */
            w->visible = 1; send_msg(c, h, WM_SHOWWINDOW, 1, 0);
            if (!w->parent) activate(c, h);
            send_msg(c, h, WM_SIZE, 0, (uint32_t)(ww & 0xFFFF) | ((uint32_t)hh << 16));
            send_msg(c, h, WM_MOVE, 0, (uint32_t)(x & 0xFFFF) | ((uint32_t)y << 16));
            g_paint_pending[slot] = 1;
        }
        /* WC3's video player (BlizPlay) plays through DirectShow, which is not
         * there: its first WM_TIMER called a NULL filter graph.  Closed at
         * once, as Escape would, it ends the movie and the game goes on;
         * WC3_MOVIES=1 leaves it open. */
        if (!strcmp(g_cls[k].name, "BlizPlay") && !getenv("WC3_MOVIES")){
            fprintf(stderr, "[user32] video player window: closed at once (no DirectShow; a skipped movie)\n");
            q_post(h, WM_CLOSE, 0, 0);
        }
        r = h;
    }
    else if (IS("ShowWindow")){
        Win *w = win_of(A(c,0)); uint32_t cmd = A(c,1);
        if (msglog()) fprintf(stderr, "[msg] ShowWindow(0x%08x, %u) tid %u\n", A(c,0), cmd, ctid(c));
        r = w ? (uint32_t)w->visible : 0u;
        if (w){
            int show = cmd != 0;                              /* SW_HIDE = 0 */
            if (show && !w->visible){
                uint32_t h = A(c,0);
                w->visible = 1; send_msg(c, h, WM_SHOWWINDOW, 1, 0);
                if (!w->parent && cmd != 4 && cmd != 8) activate(c, h);   /* SW_SHOWNOACTIVATE, SW_SHOWNA */
                send_msg(c, h, WM_SIZE, 0, (uint32_t)(w->w & 0xFFFF) | ((uint32_t)w->h << 16));
                send_msg(c, h, WM_MOVE, 0, (uint32_t)(w->x & 0xFFFF) | ((uint32_t)w->y << 16));
                g_paint_pending[w - g_win] = 1;
            } else if (!show && w->visible){ w->visible = 0; send_msg(c, A(c,0), WM_SHOWWINDOW, 0, 0); }
        }
    }
    else if (IS("UpdateWindow")){
        Win *w = win_of(A(c,0));
        if (w && g_paint_pending[w - g_win]){ g_paint_pending[w - g_win] = 0; send_msg(c, A(c,0), WM_PAINT, 0, 0); }
        r = 1;
    }
    else if (IS("DestroyWindow")){
        Win *w = win_of(A(c,0));
        if (w){ uint32_t h = A(c,0);
            send_msg(c, h, WM_DESTROY, 0, 0); send_msg(c, h, WM_NCDESTROY, 0, 0);
            w->used = 0; if (g_focus == h) g_focus = 0; if (g_active == h) g_active = 0; if (g_main == h) g_main = 0;
            for (int i = 0; i < NTIMER; i++) if (g_tm[i].h == h) g_tm[i].ms = 0; }
        r = w != 0;
    }
    else if (IS("SendMessageA")) r = send_msg(c, A(c,0), A(c,1), A(c,2), A(c,3));
    else if (IS("PostMessageA")){
        uint32_t h = A(c,0);
        if (msglog()) fprintf(stderr, "[msg] post   tid %u  hwnd 0x%08x msg 0x%04x wp 0x%08x lp 0x%08x\n", ctid(c), h, A(c,1), A(c,2), A(c,3));
        if (h == 0xFFFFu){ for (int i = 0; i < NWIN; i++) if (g_win[i].used && !g_win[i].parent) q_post(hwnd_of(&g_win[i]), A(c,1), A(c,2), A(c,3)); }
        else q_post(h, A(c,1), A(c,2), A(c,3));
        r = 1;
    }
    else if (IS("PostThreadMessageA")){
        if (msglog()) fprintf(stderr, "[msg] postT  tid %u -> thread %u msg 0x%04x wp 0x%08x lp 0x%08x\n", ctid(c), A(c,0), A(c,1), A(c,2), A(c,3));
        q_post(0, A(c,1), A(c,2), A(c,3)); r = 1; }
    else if (IS("PostQuitMessage")){ g_quit_posted = 1; g_quit_code = A(c,0); r = 0; }
    else if (IS("CallWindowProcA")){
        const uint32_t a[4] = { A(c,1), A(c,2), A(c,3), A(c,4) };
        r = A(c,0) ? call_guest(c, A(c,0), 4, a) : 0u;
    }
    else if (IS("DefWindowProcA")){
        uint32_t h = A(c,0), m = A(c,1), wp = A(c,2);
        switch (m){
        case WM_NCCREATE: r = 1; break;
        case WM_CLOSE: { const uint32_t a[1] = { h }; (void)a; Win *w = win_of(h);
            if (w){ send_msg(c, h, WM_DESTROY, 0, 0); send_msg(c, h, WM_NCDESTROY, 0, 0); w->used = 0; if (g_main == h) g_main = 0; }
            r = 0; break; }
        case WM_ERASEBKGND: r = 1; break;
        case WM_NCHITTEST: r = 1; break;                     /* HTCLIENT */
        case WM_NCACTIVATE: r = 1; break;
        case WM_SYSCOMMAND: if ((wp & 0xFFF0u) == 0xF060u) q_post(h, WM_CLOSE, 0, 0); r = 0; break;   /* SC_CLOSE */
        case WM_PAINT: g_paint_pending[win_of(h) ? win_of(h) - g_win : 0] = 0; r = 0; break;
        default: r = 0;
        }
    }
    else if (IS("GetMessageA") || IS("PeekMessageA")){
        int get = IS("GetMessageA");
        uint32_t pm = A(c,0);
        int remove = get ? 1 : (A(c,4) & 1u);
        if (g_main){ kbd_pump(); lifecycle_pump(); }
        if (take_msg(pm, remove)){ r = get ? (MR32(pm+4) != WM_QUIT) : 1u;
            if (remove && msglog()) fprintf(stderr, "[msg] %s tid %u  hwnd 0x%08x msg 0x%04x wp 0x%08x lp 0x%08x (from 0x%08x)\n",
                                            get ? "get " : "peek", ctid(c), MR32(pm), MR32(pm+4), MR32(pm+8), MR32(pm+12), MR32(c->esp - 4)); }
        else if (!get) r = 0;
        else {
            /* GetMessage waits, with the guest lock released, for a message,
             * a due timer or the host's input */
            for (;;){
                int64_t t = timer_next_us();
                int64_t ns = (t < 0 || t > 2000) ? 2000000 : (t > 200 ? t * 1000 : 200000);
                guest_sleep_ns(ns);
                if (g_main){ kbd_pump(); lifecycle_pump(); }
                if (take_msg(pm, 1)) break;
            }
            if (msglog()) fprintf(stderr, "[msg] get  tid %u  hwnd 0x%08x msg 0x%04x wp 0x%08x lp 0x%08x (waited)\n",
                                  ctid(c), MR32(pm), MR32(pm+4), MR32(pm+8), MR32(pm+12));
            r = MR32(pm+4) != WM_QUIT;
        }
    }
    else if (IS("DispatchMessageA")){
        uint32_t pm = A(c,0);
        if (pm && MR32(pm+4) == WM_TIMER && MR32(pm+12)){       /* a callback timer: TimerProc(hwnd, WM_TIMER, id, time) */
            const uint32_t a[4] = { MR32(pm), WM_TIMER, MR32(pm+8), (uint32_t)(host_us() / 1000u) };
            r = call_guest(c, MR32(pm+12), 4, a);
        } else if (pm) r = send_msg(c, MR32(pm), MR32(pm+4), MR32(pm+8), MR32(pm+12));
    }
    else if (IS("TranslateMessage")) r = 0;                       /* keyboard.c already posts WM_CHAR */
    else if (IS("SetTimer")){ r = timer_set(A(c,0), A(c,1), A(c,2), A(c,3));
        if (msglog()) fprintf(stderr, "[msg] SetTimer(0x%08x, %u, %u ms, proc 0x%08x) tid %u\n", A(c,0), A(c,1), A(c,2), A(c,3), ctid(c)); }
    else if (IS("KillTimer")) r = (uint32_t)timer_kill(A(c,0), A(c,1));
    else if (IS("GetWindowLongA") || IS("SetWindowLongA")){
        Win *w = win_of(A(c,0)); int32_t idx = (int32_t)A(c,1); int set = IS("SetWindowLongA"); uint32_t v = set ? A(c,2) : 0, *f = 0;
        if (!w){ win32_set_last_error(c, 1400); *out = 0; return 1; }     /* ERROR_INVALID_WINDOW_HANDLE */
        switch (idx){
        case -4: f = &w->proc; break; case -16: f = &w->style; break; case -20: f = &w->exstyle; break;
        case -21: f = &w->userdata; break; case -12: f = &w->id; break; case -6: f = &w->inst; break; case -8: f = &w->parent; break;
        }
        if (f){ r = *f; if (set) *f = v; }
        else if (idx >= 0 && (uint32_t)idx + 4u <= w->extrasz){ r = MR32(w->extra + (uint32_t)idx); if (set) MR32(w->extra + (uint32_t)idx) = v; }
        else r = 0;
    }
    else if (IS("GetClassLongA")){ Win *w = win_of(A(c,0)); r = (w && (int32_t)A(c,1) == -24) ? g_cls[w->cls].proc : 0u; }
    else if (IS("GetClientRect") || IS("GetWindowRect")){
        Win *w = win_of(A(c,0)); uint32_t p = A(c,1);
        unsigned gw = 1280, gh = 720; metal_game_size(&gw, &gh); if (!gw || !gh){ gw = 1280; gh = 720; }
        int32_t x = 0, y = 0, ww = (int32_t)gw, hh = (int32_t)gh;
        if (w){ ww = w->w; hh = w->h; if (IS("GetWindowRect")){ x = w->x; y = w->y; } }
        if (p){ mem_commit(p, 16); MR32(p) = (uint32_t)x; MR32(p+4) = (uint32_t)y; MR32(p+8) = (uint32_t)(x + ww); MR32(p+12) = (uint32_t)(y + hh); }
        r = 1;
    }
    else if (IS("SetWindowPos") || IS("MoveWindow")){
        Win *w = win_of(A(c,0));
        if (w){
            int32_t x, y, ww, hh; uint32_t fl = 0;
            if (IS("MoveWindow")){ x = (int32_t)A(c,1); y = (int32_t)A(c,2); ww = (int32_t)A(c,3); hh = (int32_t)A(c,4); }
            else { x = (int32_t)A(c,2); y = (int32_t)A(c,3); ww = (int32_t)A(c,4); hh = (int32_t)A(c,5); fl = A(c,6); }
            if (!(fl & 2u)){ w->x = x; w->y = y; }                     /* SWP_NOMOVE */
            if (!(fl & 1u) && ww > 0 && hh > 0 && (ww != w->w || hh != w->h)){   /* SWP_NOSIZE */
                w->w = ww; w->h = hh; send_msg(c, A(c,0), WM_SIZE, 0, (uint32_t)(ww & 0xFFFF) | ((uint32_t)hh << 16)); }
            if (fl & 0x40u) w->visible = 1;                            /* SWP_SHOWWINDOW */
        }
        r = 1;
    }
    else if (IS("AdjustWindowRect") || IS("AdjustWindowRectEx")) r = 1;   /* no frames */
    else if (IS("IsWindow")) r = win_of(A(c,0)) != 0;
    else if (IS("IsWindowVisible")){ Win *w = win_of(A(c,0)); r = w && w->visible; }
    else if (IS("IsWindowEnabled")) r = win_of(A(c,0)) != 0;
    else if (IS("EnableWindow")) r = 0;
    else if (IS("IsIconic") || IS("IsZoomed")) r = 0;
    else if (IS("GetParent")){ Win *w = win_of(A(c,0)); r = w ? w->parent : 0u; }
    else if (IS("GetFocus")) r = g_focus;
    else if (IS("SetFocus")){ uint32_t h = A(c,0), of = g_focus; r = of;
        if (h != of){ g_focus = h; if (of) send_msg(c, of, WM_KILLFOCUS, h, 0); if (h) send_msg(c, h, WM_SETFOCUS, of, 0); } }
    else if (IS("GetActiveWindow") || IS("GetForegroundWindow")) r = g_active ? g_active : g_main;
    else if (IS("SetActiveWindow") || IS("SetForegroundWindow")){ r = IS("SetActiveWindow") ? g_active : 1u; if (win_of(A(c,0))) activate(c, A(c,0)); }
    else if (IS("GetDesktopWindow")) r = 0x00B00000u;
    else if (IS("GetWindow") || IS("GetTopWindow")) r = 0;
    else if (IS("FindWindowA") || IS("FindWindowExA")) r = 0;       /* no other instance of the game */
    else if (IS("SetWindowTextA")){ Win *w = win_of(A(c,0)); if (w) cstr(A(c,1), w->text, sizeof w->text); r = 1; }
    else if (IS("GetWindowTextA")){ Win *w = win_of(A(c,0)); uint32_t p = A(c,1), n2 = A(c,2);
        if (w && p && n2){ uint32_t l = (uint32_t)strlen(w->text); if (l >= n2) l = n2 - 1; memcpy(g_mem + p, w->text, l); MR8(p + l) = 0; r = l; } }
    else if (IS("GetClassNameA")){ Win *w = win_of(A(c,0)); uint32_t p = A(c,1), n2 = A(c,2);
        if (w && p && n2){ const char *s = g_cls[w->cls].name; uint32_t l = (uint32_t)strlen(s); if (l >= n2) l = n2 - 1; memcpy(g_mem + p, s, l); MR8(p + l) = 0; r = l; } }
    else if (IS("SetPropA") || IS("GetPropA") || IS("RemovePropA")){
        Win *w = win_of(A(c,0)); char nm[32]; cstr(A(c,1), nm, sizeof nm);
        if (w){
            int k = -1, fr = -1;
            for (int i = 0; i < 8; i++){ if (w->prop[i].name[0] && !strcmp(w->prop[i].name, nm)) k = i; if (!w->prop[i].name[0] && fr < 0) fr = i; }
            if (IS("SetPropA")){ if (k < 0) k = fr; if (k >= 0){ snprintf(w->prop[k].name, sizeof w->prop[k].name, "%s", nm); w->prop[k].v = A(c,2); r = 1; } }
            else if (k >= 0){ r = w->prop[k].v; if (IS("RemovePropA")) w->prop[k].name[0] = 0; }
        }
    }
    else if (IS("InvalidateRect") || IS("InvalidateRgn")){ Win *w = win_of(A(c,0)); if (w) g_paint_pending[w - g_win] = 1; r = 1; }
    else if (IS("ValidateRect")){ Win *w = win_of(A(c,0)); if (w) g_paint_pending[w - g_win] = 0; r = 1; }
    else if (IS("BeginPaint")){
        Win *w = win_of(A(c,0)); uint32_t ps = A(c,1);
        if (w) g_paint_pending[w - g_win] = 0;
        if (ps){ mem_commit(ps, 64); memset(g_mem + ps, 0, 64); MR32(ps) = 0x00B20001u;
                 if (w){ MR32(ps+16) = (uint32_t)w->w; MR32(ps+20) = (uint32_t)w->h; } }
        r = 0x00B20001u;                                       /* the window's DC (GDI is not drawn) */
    }
    else if (IS("EndPaint")) r = 1;
    else if (IS("ClientToScreen") || IS("ScreenToClient")){
        Win *w = win_of(A(c,0)); uint32_t p = A(c,1);
        if (w && p){ int32_t dx = IS("ClientToScreen") ? w->x : -w->x, dy = IS("ClientToScreen") ? w->y : -w->y;
                     MR32(p) += (uint32_t)dx; MR32(p+4) += (uint32_t)dy; }
        r = 1;
    }
    else if (IS("MapWindowPoints")) r = 0;
    else if (IS("LoadImageA")) r = 0x00B10002u;                  /* icons and cursors: a placeholder handle (not drawn) */
    else return 0;
#undef IS
    *out = r;
    return 1;
}
