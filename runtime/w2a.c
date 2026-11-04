/* The W (UTF-16) Win32 calls of a Unicode game on the runtime's A ones.
 *
 * w2a_pre: for a W import that has an A twin here, its string arguments are
 * converted into a scratch buffer in guest memory and the stack slots point
 * there for the call; the name becomes the A one.  w2a_post converts what the
 * A call wrote (buffers, WIN32_FIND_DATA) back to UTF-16 and puts the stack
 * slots back.  Calls with no A twin (lstrlenW, GetTempPathW, ...) are served
 * here directly (w2a_direct).  Characters outside Latin-1 become '?'
 * on the way to A: the game's file names are ASCII. */
#include "cpu.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

uint32_t com_scratch(uint32_t n);
const char *fileio_game_dir(void);

#ifndef ARG
#define ARG(c,i) MR32((c)->esp + 4u*(uint32_t)(i))
#endif
#define SCR_SIZE 0x8000u

static __thread uint32_t g_scr, g_scr_used;

static uint32_t scr(uint32_t n){
    if (!g_scr){ g_scr = com_scratch(SCR_SIZE); }
    n = (n + 3u) & ~3u;
    if (g_scr_used + n > SCR_SIZE) g_scr_used = 0;
    uint32_t p = g_scr + g_scr_used; g_scr_used += n;
    mem_commit(p, n);
    return p;
}
static uint32_t wlen(uint32_t p){ uint32_t n = 0; while (mem_is_committed(p + 2*n + 1) && MR16(p + 2*n)) n++; return n; }
/* a UTF-16 string to a fresh A copy in the scratch buffer */
static uint32_t w2a_str(uint32_t p){
    if (p <= 0xFFFFu) return p;                     /* NULL or an atom / resource id */
    uint32_t n = wlen(p), d = scr(n + 1);
    for (uint32_t i = 0; i < n; i++){ uint16_t ch = MR16(p + 2*i); MR8(d + i) = ch < 0x100 ? (uint8_t)ch : '?'; }
    MR8(d + n) = 0;
    return d;
}
/* an A string back into a UTF-16 buffer of cap characters; returns its length */
static uint32_t a2w_buf(uint32_t a, uint32_t w, uint32_t cap){
    uint32_t n = 0;
    if (!cap) return 0;
    while (n + 1 < cap && MR8(a + n)){ MR16(w + 2*n) = MR8(a + n); n++; }
    MR16(w + 2*n) = 0;
    return n;
}

typedef struct { uint32_t slot[6], val[6]; int ns; int kind; uint32_t wbuf, abuf, cap; } W2A;
enum { K_PLAIN, K_OUTBUF, K_FIND, K_CLASS };

static void swap_arg(CPU *c, W2A *x, int i, uint32_t v){
    x->slot[x->ns] = c->esp + 4u*(uint32_t)i; x->val[x->ns] = MR32(c->esp + 4u*(uint32_t)i); x->ns++;
    MR32(c->esp + 4u*(uint32_t)i) = v;
}
static void str_arg(CPU *c, W2A *x, int i){ swap_arg(c, x, i, w2a_str(ARG(c, i))); }

/* served here: the W calls with no A twin in the runtime */
static int w2a_direct(CPU *c, const char *n, uint32_t *r){
    if (!strcmp(n, "lstrlenW")){ *r = ARG(c,0) ? wlen(ARG(c,0)) : 0; return 1; }
    if (!strcmp(n, "GetTempPathW") || !strcmp(n, "GetTempPathA")){
        /* the save folder's temp, as a Windows path */
        const char *t = "C:\\Temp\\"; uint32_t cap = ARG(c,0), b = ARG(c,1), l = (uint32_t)strlen(t);
        if (cap <= l){ *r = l + 1; return 1; }
        for (uint32_t i = 0; i <= l; i++){ if (n[11] == 'W') MR16(b + 2*i) = (uint8_t)t[i]; else MR8(b + i) = (uint8_t)t[i]; }
        *r = l; return 1; }
    if (!strcmp(n, "GetComputerNameW")){
        const char *t = "BULANCI"; uint32_t b = ARG(c,0), ps = ARG(c,1), l = (uint32_t)strlen(t);
        if (!ps || MR32(ps) <= l){ if (ps) MR32(ps) = l + 1; *r = 0; return 1; }
        for (uint32_t i = 0; i <= l; i++) MR16(b + 2*i) = (uint8_t)t[i];
        MR32(ps) = l; *r = 1; return 1; }
    if (!strcmp(n, "WriteConsoleW")){ uint32_t pw = ARG(c,3); if (pw) MR32(pw) = ARG(c,2); *r = 1; return 1; }
    if (!strcmp(n, "FormatMessageW")){ *r = 0; return 1; }      /* no system message texts */
    return 0;
}

/* before the call: returns the A name to dispatch, or NULL to leave it */
const char *w2a_pre(CPU *c, const char *n, void *xv, uint32_t *direct_r, int *direct){
    W2A *x = xv; _Static_assert(sizeof(W2A) <= 128, "w2a ctx");
    memset(x, 0, sizeof *x); *direct = 0;
    size_t l = strlen(n);
    if (l < 2 || n[l-1] != 'W') return 0;
    if (w2a_direct(c, n, direct_r)){ *direct = 1; return 0; }
    static char an[64];
    if (l >= sizeof an) return 0;
    memcpy(an, n, l - 1); an[l-1] = 'A'; an[l] = 0;
    if (!strcmp(n, "GetFileAttributesW") || !strcmp(n, "DeleteFileW") || !strcmp(n, "CreateFileW")) str_arg(c, x, 0);
    else if (!strcmp(n, "CreateEventW")) str_arg(c, x, 3);
    else if (!strcmp(n, "MessageBoxW")){ str_arg(c, x, 1); str_arg(c, x, 2); }
    else if (!strcmp(n, "CreateWindowExW")){ str_arg(c, x, 1); str_arg(c, x, 2); }
    else if (!strcmp(n, "PeekMessageW") || !strcmp(n, "DispatchMessageW") || !strcmp(n, "DefWindowProcW")) {}
    else if (!strcmp(n, "GetModuleFileNameW")){
        x->kind = K_OUTBUF; x->wbuf = ARG(c,1); x->cap = ARG(c,2); x->abuf = scr(x->cap + 1);
        swap_arg(c, x, 1, x->abuf); }
    else if (!strcmp(n, "FindFirstFileW") || !strcmp(n, "FindNextFileW")){
        int di = 1;                                             /* (name | handle, data) */
        if (!strcmp(n, "FindFirstFileW")) str_arg(c, x, 0);
        x->kind = K_FIND; x->wbuf = ARG(c, di); x->abuf = scr(320);
        swap_arg(c, x, di, x->abuf); }
    else if (!strcmp(n, "RegisterClassExW")){
        /* WNDCLASSEXW -> WNDCLASSEXA: the same layout, lpszMenuName (+0x24)
         * and lpszClassName (+0x28) converted */
        uint32_t w = ARG(c,0), a = scr(48);
        memcpy(g_mem + a, g_mem + w, 48);
        MR32(a + 0x24) = w2a_str(MR32(w + 0x24)); MR32(a + 0x28) = w2a_str(MR32(w + 0x28));
        swap_arg(c, x, 0, a); }
    else return 0;                                              /* no A twin: left as it is */
    return an;
}

/* after the call: the outputs back to UTF-16, the stack slots restored */
uint32_t w2a_post(CPU *c, void *xv, uint32_t r){
    W2A *x = xv;
    if (x->kind == K_OUTBUF && x->wbuf && x->cap){ uint32_t m = a2w_buf(x->abuf, x->wbuf, x->cap); if (r) r = m; }
    if (x->kind == K_FIND && x->wbuf && r && r != 0xFFFFFFFFu){
        /* WIN32_FIND_DATAA (320 bytes) -> W (592): 44 bytes alike, then the names */
        memcpy(g_mem + x->wbuf, g_mem + x->abuf, 44);
        a2w_buf(x->abuf + 44, x->wbuf + 44, 260);
        a2w_buf(x->abuf + 44 + 260, x->wbuf + 44 + 520, 14);
    }
    for (int i = 0; i < x->ns; i++) MR32(x->slot[i]) = x->val[i];
    return r;
}
