/* Stage 3 seed: the Win32 API subset needed to boot + tracing for the rest. */
#include <stdatomic.h>
#include "cpu.h"
#include "module.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>
#include <setjmp.h>
#include <unistd.h>

/* getenv() SCANS the environment array and takes a lock on every call.  On
 * per-draw paths (e.g. the "PDLIST" check in SetVertexShaderConstantF was
 * the FIRST term of the && chain) that has a measurable cost: in the
 * profile getenv+__findenv_locked ate 3% of the guest thread.
 * Environment variables do not change during a run -> resolve once, keep. */
#define ENVC(nm_) ({ static const char *_ev_; static int _ei_; \
                     if (!_ei_){ _ei_ = 1; _ev_ = getenv(nm_); } _ev_; })

jmp_buf g_exit_jmp;
int g_exit_code = 0;
int g_exit_armed = 0;

/* One import index space for every lifted module (module.c, g_modtab order).
 * An IAT slot of a non-lifted DLL holds
 * IMP_MAGIC + index into this table. */
static ImpEnt *g_imps; static uint32_t g_nimps, g_nmodimps;
/* Functions the game only reaches through GetProcAddress, of DLLs that are
 * not lifted (docs/PLAN.md "Dynamické načítání"): they need an import index
 * of their own for the HLE.  The argument counts are Windows'. */
static const ImpEnt g_dynimp[] = {
    {0, "d3d8.dll",     "Direct3DCreate8", 1},
    {0, "DSOUND.DLL",   "DirectSoundCreate", 3},
    {0, "DSOUND.DLL",   "DirectSoundEnumerateA", 2},
    {0, "mswsock.dll",  "AcceptEx", 8},
    {0, "mswsock.dll",  "GetAcceptExSockaddrs", 8},
    {0, "ws2_32.dll",   "WSAIoctl", 9},
    {0, "ws2_32.dll",   "WSARecv", 7},
    {0, "ws2_32.dll",   "WSASend", 7},
    {0, "KERNEL32.dll", "GetDiskFreeSpaceExA", 4},
    {0, "KERNEL32.dll", "GetSystemDefaultUILanguage", 0},
    {0, "KERNEL32.dll", "TryEnterCriticalSection", 1},
    {0, "KERNEL32.dll", "IsProcessorFeaturePresent", 1},
    {0, "KERNEL32.dll", "IsDebuggerPresent", 0},
    {0, "USER32.dll",   "GetActiveWindow", 0},
    {0, "USER32.dll",   "GetLastActivePopup", 1},
    {0, "USER32.dll",   "MessageBoxA", 4},
};
#define NDYNIMP ((uint32_t)(sizeof g_dynimp / sizeof g_dynimp[0]))
/* LoadLibrary of a DLL that is not lifted: the system DLLs the HLE stands in
 * for get a placeholder handle (their functions come from GetProcAddress by
 * name); anything else (Miles' optional VORT_DLS/S3BASE, dbghelp, ddraw,
 * Battle.net's downloaded DLLs) fails as if it were not installed. */
static int system_dll(const char *nm){
    static const char *const sys[] = { "kernel32", "user32", "advapi32", "gdi32", "ws2_32", "wsock32", "mswsock",
        "d3d8", "dsound", "winmm", "ole32", "oleaut32", "shell32", "version", "imm32", "comctl32", "comdlg32",
        "wininet", "ntdll", 0 };
    const char *b = nm;
    for (const char *q = nm; *q; q++) if (*q == '\\' || *q == '/') b = q + 1;
    for (int i = 0; sys[i]; i++){
        size_t l = strlen(sys[i]);
        if (!strncasecmp(b, sys[i], l) && (!b[l] || !strcasecmp(b + l, ".dll"))) return 1;
    }
    return 0;
}
#define SYSDLL_HANDLE 0x20000000u
void mem_commit(uint32_t,uint32_t);
void guest_backtrace(CPU*,int);
int win32_ext_call(CPU*, const char*, uint32_t*);
int user32_call(CPU*, const char*, uint32_t*);
void guest_fair_yield(void);
int fileio_call(CPU*, const char*, uint32_t*);
void fileio_report(void);
int winsock_call(CPU*, const char*, const char*, uint32_t*);   /* ws2_32, netapi32 (winsock.c) */
void winsock_report(void);
uint64_t win32_ext_threads(void);
uint64_t win32_ext_threads_exited(void);
uint32_t com_direct3dcreate9(void);
uint32_t com_directinput8create(uint32_t);
uint32_t com_directsoundcreate(uint32_t);
void com_init(void);
int g_bt_on = 0;

#define IMP_MAGIC 0xF0000000u

/* Guest heap, VirtualAlloc and friends: runtime/heap.c. */
void     heap_init(void);
void     heap_report(void);
int      heap_api_kind(const char *name);
uint32_t heap_api(CPU *c, int kind);
uint32_t gheap_alloc(int heap, uint32_t n, int zero);
uint32_t gheap_usize(uint32_t p);
void     heap_totals(uint64_t *live, uint64_t *peak, uint64_t *va, uint64_t *held);
/* ---- the game's clocks ----------------------------------------------
 * WC3: GetTickCount, timeGetTime and QueryPerformanceCounter are real
 * monotonic time.  nfsu2-web paced NFSU2's clock by its frames (the game
 * entered a catch-up loop on real time); WC3 runs its simulation on fixed
 * game ticks against real time, and a LAN game's lockstep needs every
 * player's clock to run at the real rate. */
#ifdef __wasm__
#include "web/host_abi.h"
#endif
static uint64_t real_us(void){
#ifdef __wasm__
    /* The guest's most frequent host call (QueryPerformanceCounter, timeGetTime:
     * ~1,000 a race frame).  The page's monotonic clock without WASI's
     * clock_time_get, whose i64 argument costs a BigInt on every call into JS. */
    return (uint64_t)(nfsu2_now_ms() * 1000.0);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

/* oracle.c (cmp): the clock stands still while one guest function runs
 * twice (under the emulator, then lifted), so both see the same QPC/ms. */
_Atomic uint64_t g_vclock_freeze = 0;
uint64_t host_us(void){
    { uint64_t fz = atomic_load(&g_vclock_freeze); if (__builtin_expect(fz != 0, 0)) return fz; }
    return real_us();
}
/* com.c calls these at Present: nothing to pace with a real-time clock */
void vclock_tick(void){}
void vclock_report(void){}
uint32_t host_ms(void){ return (uint32_t)(host_us() / 1000ull) + 1000u; }
uint64_t host_qpc(void){ return host_us(); }

uint32_t win32_block_size(uint32_t u){ return gheap_usize(u); }
/* guest address space the heap has reserved (the bump pointer with
 * NFSU2_HEAP=bump), and the bytes in live heap blocks (status line "heap=") */
uint32_t win32_heap_used(void){ uint64_t v = 0; heap_totals(0, 0, &v, 0); return (uint32_t)v; }
uint32_t win32_heap_live(void){ uint64_t l = 0; heap_totals(&l, 0, 0, 0); return (uint32_t)l; }

/* --- tracing --- */
typedef struct { uint32_t calls; uint32_t unimpl; uint32_t from; uint32_t seen; } ImpStat;
static ImpStat *stat_;
static uint32_t trace_n = 0, trace_max = 0;
void win32_set_trace_max(uint32_t v){ trace_max = v; }
static uint32_t unimpl_first = 0xffffffffu;
static const char *unimpl_name = 0;
uint64_t g_win32_calls = 0, g_win32_unimpl = 0;

static uint32_t tls_slot[128]; static int tls_next = 0;
static uint32_t tls_free[128]; static int tls_nfree = 0;
int win32_tls_in_use(void){ return tls_next - tls_nfree; }
static uint32_t g_cmdline = 0;
static uint64_t tick0;

static uint32_t ARG(CPU*c,int i){ return MR32(c->esp + 4u*(uint32_t)i); }  /* after the return address has been POPped */

/* QPCWHO=1: who calls QueryPerformanceCounter (thread, return address, one
 * frame up).  At 120 fps ~21 thousand calls per frame were seen (~2
 * thousand at 60 fps PACE60): a time-based wait loop.  The call runs under
 * the GIL, the table is lock-free. */
typedef struct { uint32_t tid, ra, ra2; uint64_t n; } QpcWho;
static QpcWho g_qw[128];
static void qpc_report(void){
    fprintf(stderr, "\n=== QPCWHO: QueryPerformanceCounter callers ===\n");
    for (int k = 0; k < 128; k++){
        int b = -1;
        for (int i = 0; i < 128; i++) if (g_qw[i].n && (b < 0 || g_qw[i].n > g_qw[b].n)) b = i;
        if (b < 0 || k >= 24) break;
        fprintf(stderr, "  %12llu  thread %5u  return 0x%08x  parent 0x%08x\n",
                (unsigned long long)g_qw[b].n, g_qw[b].tid, g_qw[b].ra, g_qw[b].ra2);
        g_qw[b].n = 0;
    }
}
uint64_t spin_slept_us(void){ return 0; }   /* perflog: no frame-loop sleep in WC3 (yet) */
static void qpc_who(CPU *c){
    static int init = 0; static uint64_t tot = 0;
    if (!init){ init = 1; atexit(qpc_report); }
    if ((++tot & 0xFFFFFu) == 0){ long metal_frames(void);
        fprintf(stderr, "  [qpc] %llu M calls, frame %ld\n", (unsigned long long)(tot >> 20), metal_frames()); }
    uint32_t tid = (c->fs_base && mem_is_committed(c->fs_base+0x24u)) ? MR32(c->fs_base+0x24u) : 0u;
    uint32_t ra = c->eip;                       /* after POP32: return address */
    /* sub_0043bdf0 (the game's time wrapper): sub esp,8; push arg; call.
     * After POP32 [esp]=arg, [esp+4..0xb]=locals, [esp+0xc]=its caller. */
    uint32_t ra2 = (c->ebp && mem_is_committed(c->ebp+4u)) ? MR32(c->ebp+4u) : 0u;
    uint32_t h = (tid * 2654435761u ^ ra * 40503u ^ ra2) & 127u;
    for (int i = 0; i < 128; i++, h = (h + 1) & 127u){
        QpcWho *w = &g_qw[h];
        if (!w->n){ w->tid = tid; w->ra = ra; w->ra2 = ra2; w->n = 1; return; }
        if (w->tid == tid && w->ra == ra && w->ra2 == ra2){ w->n++; return; }
    }
}

/* Import index -> heap.c API kind (0 = not a heap call), filled in win32_init. */
static int8_t *g_hk; static int g_hk_heapalloc;
/* HeapAlloc debugging aids (unchanged from the bump allocator days). */
static void heapalloc_debug(CPU *c, uint32_t r){
    uint32_t sz = ARG(c,2);
    { const char *w = ENVC("TRACE_ALLOC");
      if (w){ uint32_t want = (uint32_t)strtoul(w,0,0);
        if (sz == want){ extern uint32_t g_watch_prev_ip;
          fprintf(stderr,"      [alloc] HeapAlloc(%u) -> 0x%08x   calling guest instruction: 0x%08x\n",
                  sz, r, g_watch_prev_ip);
          guest_backtrace(c, 12); } } }
    /* WATCHALLOC=<size>: set a watchpoint at +16 of the first allocation of
     * that size.  The constant block is written only once, so this is the
     * only way to catch the write. */
    /* WATCHAT=<address>: set a watchpoint on the allocation that returns exactly that address */
    { const char *wa = ENVC("WATCHAT");
      if (wa && r){ static int armed2 = 0;
        uint32_t want3 = (uint32_t)strtoul(wa,0,0);
        if (!armed2 && r == want3){ armed2 = 1;
          void watch_arm(uint32_t);
          const char *wo = ENVC("WATCHOFF");
          uint32_t off3 = wo ? (uint32_t)strtoul(wo,0,0) : 0u;
          fprintf(stderr,"      [alloc] target block 0x%08x (%u bytes) allocated, watching +%u\n",
                  r, sz, off3);
          watch_arm(r + off3);
          if (ENVC("MPROT")){ void mprot_arm(uint32_t); mprot_arm(r + off3); }
        } } }
    { const char *w2 = ENVC("WATCHALLOC");
      if (w2 && r){ static int armed = 0; static int seen = 0;
        uint32_t want2 = (uint32_t)strtoul(w2,0,0);
        const char *ws = ENVC("WATCHALLOC_N");
        int wantn = ws ? atoi(ws) : 1;
        if (!armed && sz == want2 && ++seen == wantn){ armed = 1;
          void watch_arm(uint32_t);
          fprintf(stderr,"      [alloc] %u-byte block 0x%08x allocated; watchpoint at +16\n", sz, r);
          watch_arm(r); } } }
    static int shown=0;
    if (ENVC("VERBOSE") && shown<2 && sz>0x100000u){ shown++;
        fprintf(stderr,"      [heap] HeapAlloc(size=0x%08x) SUSPICIOUS. ebp=0x%08x esp=0x%08x\n", sz, c->ebp, c->esp);
        fprintf(stderr,"        stack (from esp):");
        for(int k=0;k<14;k++) fprintf(stderr," %s%08x", k?"":"", MR32(c->esp+4u*k));
        fprintf(stderr,"\n        around ebp    :");
        for(int k=-4;k<=6;k++) fprintf(stderr," %s%08x", k==0?">":"", MR32(c->ebp+4u*(uint32_t)k));
        fprintf(stderr,"\n        esi=%08x edi=%08x ebx=%08x\n", c->esi,c->edi,c->ebx); }
}

/* Per-thread last error in the TEB (+0x34, the Windows layout): GetLastError /
 * SetLastError, the same slot WSAGetLastError / WSASetLastError use
 * (winsock.c); module.c (server.dll) calls win32_set_last_error too. */
void win32_set_last_error(CPU *c, uint32_t e){
    if (c && c->fs_base && mem_is_committed(c->fs_base + 0x34u)) MR32(c->fs_base + 0x34u) = e;
}
uint32_t win32_get_last_error(CPU *c){
    return (c && c->fs_base && mem_is_committed(c->fs_base + 0x34u)) ? MR32(c->fs_base + 0x34u) : 0u;
}
/* GetModuleHandleA: our exe and the DLLs it imports are "loaded" (they all
 * map to the image, as before, and GetProcAddress resolves by import name);
 * anything else (d3d9d.dll, mscoree.dll, ...) is not. */
static int module_known(uint32_t p){
    if (!mem_is_committed(p)) return 0;
    char nm[64]; snprintf(nm, sizeof nm, "%s", (const char *)(g_mem + p));
    return system_dll(nm);
}
/* RaiseException(code, flags, nargs, args): log everything useful. */
static void raise_exception_report(CPU *c){
    uint32_t code = ARG(c,0), fl = ARG(c,1), na = ARG(c,2), pa = ARG(c,3);
    const char *what = code == 0xE06D7363u ? "C++ exception (throw)"
                     : code == 0xC000008Eu ? "float divide by zero" : code == 0xC000008Fu ? "float inexact result"
                     : code == 0xC0000090u ? "float invalid operation" : code == 0xC0000091u ? "float overflow"
                     : code == 0xC0000093u ? "float underflow" : code == 0xC000008Du ? "float denormal operand"
                     : code == 0xC0000092u ? "float stack check" : "?";
    fprintf(stderr, "\n*** GUEST RAISEEXCEPTION: code 0x%08x (%s), flags 0x%x, %u args, from 0x%08x (thread %u)\n",
            code, what, fl, na, c->eip,
            (c->fs_base && mem_is_committed(c->fs_base + 0x24u)) ? MR32(c->fs_base + 0x24u) : 0u);
    fprintf(stderr, "    x87 control word 0x%04x (exception mask 0x%02x%s, PC %u, RC %u), status 0x%04x\n",
            c->fcw, c->fcw & 0x3Fu, (c->fcw & 0x3Fu) == 0x3Fu ? " = all masked" : " -- UNMASKED exceptions",
            (c->fcw >> 8) & 3u, (c->fcw >> 10) & 3u, c->fsw);
    for (uint32_t i = 0; i < na && i < 8 && pa && mem_is_committed(pa + 4u * i + 3u); i++)
        fprintf(stderr, "    arg[%u] = 0x%08x\n", i, MR32(pa + 4u * i));
    /* MSVC C++ throw: args = { magic, object, ThrowInfo* }; name the type */
    if (code == 0xE06D7363u && na >= 3 && pa && mem_is_committed(pa + 11)){
        uint32_t ti = MR32(pa + 8);
        uint32_t cta = (ti && mem_is_committed(ti + 15)) ? MR32(ti + 12) : 0;
        uint32_t ct  = (cta && mem_is_committed(cta + 7)) ? MR32(cta + 4) : 0;
        uint32_t td  = (ct && mem_is_committed(ct + 7)) ? MR32(ct + 4) : 0;
        if (td && mem_is_committed(td + 8)) fprintf(stderr, "    thrown type %.80s\n", (const char *)(g_mem + td + 8));
        else if (!ti) fprintf(stderr, "    rethrow (throw;) of the current exception\n");
    }
    guest_backtrace(c, 16);
}
/* NFSU2_NETLOG=1: log every network-related import call (ws2_32, netapi32,
 * tapi32, plus LoadLibraryA/GetProcAddress/FreeLibrary) with its raw
 * arguments, a few decoded ones and the result.  Research aid for the LAN
 * track; off by default.  Must run before the stdcall stack cleanup. */
static void netlog(CPU *c, const ImpEnt *e, uint32_t r, int handled){
    static int on = -1;
    if (on < 0){ const char *v = getenv("NFSU2_NETLOG"); on = v && *v && *v != '0'; }
    if (!on) return;
    const char *d = e->dll, *n = e->name;
    int net = !strncasecmp(d, "ws2_32", 6) || !strncasecmp(d, "netapi32", 8) || !strncasecmp(d, "tapi32", 6);
    if (!net && strcmp(n, "LoadLibraryA") && strcmp(n, "GetProcAddress") && strcmp(n, "FreeLibrary")) return;
    char a[160]; int k = 0; a[0] = 0;
    for (int i = 0; i < e->nargs && i < 9 && k < (int)sizeof a - 12; i++)
        k += snprintf(a + k, sizeof a - (size_t)k, "%s0x%x", i ? "," : "", ARG(c, i));
    char x[128]; x[0] = 0;
    int si = !strcmp(n, "LoadLibraryA") || !strcmp(n, "gethostbyname") ? 0 : !strcmp(n, "GetProcAddress") ? 1 : -1;
    int sa = !strcmp(n, "bind") || !strcmp(n, "connect") ? 1 : !strcmp(n, "sendto") ? 4 : -1;
    if (si >= 0){ uint32_t p = ARG(c, si);
        if (p > 0xffffu && mem_is_committed(p) && mem_is_committed(p + 95u))
            snprintf(x, sizeof x, " \"%.96s\"", (const char *)(g_mem + p)); }
    if (sa >= 0){ uint32_t p = ARG(c, sa);
        if (p && mem_is_committed(p) && mem_is_committed(p + 15u))
            snprintf(x, sizeof x, " {af %u %u.%u.%u.%u:%u}", MR16(p), MR8(p + 4), MR8(p + 5), MR8(p + 6), MR8(p + 7),
                     (unsigned)(MR8(p + 2) << 8 | MR8(p + 3))); }
    long metal_frames(void);
    uint32_t tid = (c->fs_base && mem_is_committed(c->fs_base + 0x24u)) ? MR32(c->fs_base + 0x24u) : 0u;
    fprintf(stderr, "[net] f=%ld tid=%u %s!%s(%s)%s -> 0x%x%s from 0x%08x\n",
            metal_frames(), tid, d, n, a, x, r, handled ? "" : " [stub]", c->eip);
    /* NFSU2_NETLOG=2: also the first 64 bytes of each outgoing payload */
    if (*getenv("NFSU2_NETLOG") == '2' && (!strcmp(n, "sendto") || !strcmp(n, "send"))){
        uint32_t p = ARG(c, 1), len = ARG(c, 2), m = len < 64u ? len : 64u;
        if (p && mem_is_committed(p) && mem_is_committed(p + m)){
            char h[200]; int hk = 0;
            for (uint32_t i = 0; i < m; i++) hk += snprintf(h + hk, sizeof h - (size_t)hk, "%02x", MR8(p + i));
            fprintf(stderr, "[net]   payload %u bytes: %s%s\n", len, h, len > m ? "..." : "");
        }
    }
}

/* win32_ext.c asks which module's import is served (nfsu2: the exe or
 * server.dll, for the DLL's thread exit); WC3 gives every module Windows'
 * semantics */
int win32_caller_module(void){ return 0; }

/* WC3_CRYPTWATCH=1 (debugging): Storm's MPQ crypt table ([0x54c2e4], first
 * word 0x55c636e2) checked at every import call; the first call that finds it
 * changed is logged with the guest stack. */
/* the guest's environment variables (GetEnvironmentVariableA) */
#define ENV_MAX 32
static struct { char name[128]; char *val; } g_env[ENV_MAX];
static const char *env_get(const char *nm){
    for (int k = 0; k < ENV_MAX; k++) if (g_env[k].val && !strcasecmp(g_env[k].name, nm)) return g_env[k].val;
    return 0;
}
static void env_set(const char *nm, const char *v){
    int f = -1;
    for (int k = 0; k < ENV_MAX; k++){
        if (g_env[k].val && !strcasecmp(g_env[k].name, nm)){ f = k; break; }
        if (!g_env[k].val && f < 0) f = k;
    }
    if (f < 0 || strlen(nm) >= sizeof g_env[0].name) return;
    free(g_env[f].val); g_env[f].val = 0;
    if (v){ snprintf(g_env[f].name, sizeof g_env[f].name, "%s", nm); g_env[f].val = strdup(v); }
}

/* RT_VERSION of a PE file in the game folder: its size in *len, copied to
 * guest dst (at most *len bytes) when dst != 0.  1 if found. */
int fileio_resolve_game(const char *win, char *out, size_t outsz);
static int version_resource(const char *win, uint32_t dst, uint32_t *len){
    char host[1024]; if (!fileio_resolve_game(win, host, sizeof host)) return 0;
    FILE *f = fopen(host, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 && n < (64 << 20) ? malloc((size_t)n) : 0; int ok = 0;
    if (d && fread(d, 1, (size_t)n, f) == (size_t)n) do {
#define U16(o) ((uint32_t)d[o] | (uint32_t)d[(o)+1] << 8)
#define U32(o) (U16(o) | U16((o)+2) << 16)
#define IN(o, k) ((o) >= 0 && (long)(o) + (k) <= n)
        long pe = IN(0x3c, 4) ? (long)U32(0x3c) : -1; if (!IN(pe, 0xf8) || U32(pe) != 0x4550u) break;
        uint32_t nsec = U16(pe + 6), opt = pe + 24, sec = opt + U16(pe + 20);
        uint32_t rsrc = U32(opt + 112);                         /* data directory 2 (resources) */
        if (!rsrc) break;
        /* RVA -> file offset */
        #define R2O(rva, out) do { out = -1; for (uint32_t k_ = 0; k_ < nsec; k_++){ uint32_t s_ = sec + 40*k_;             if (!IN(s_, 40)) break; uint32_t va_ = U32(s_+12), vs_ = U32(s_+8), rs_ = U32(s_+16), rp_ = U32(s_+20);             if ((rva) >= va_ && (rva) < va_ + (vs_ > rs_ ? vs_ : rs_)){ out = (long)((rva) - va_ + rp_); break; } } } while (0)
        long base; R2O(rsrc, base); if (!IN(base, 16)) break;
        /* type 16 (RT_VERSION) -> first name -> first language -> data entry */
        long dir = base; uint32_t want = 16; long ent = -1;
        for (int level = 0; level < 3; level++){
            if (!IN(dir, 16)) { ent = -1; break; }
            uint32_t cnt = U16(dir + 12) + U16(dir + 14); ent = -1;
            for (uint32_t k = 0; k < cnt; k++){
                long e = dir + 16 + 8*k; if (!IN(e, 8)) break;
                if (level == 0 && U32(e) != want) continue;
                ent = e; break;
            }
            if (ent < 0) break;
            uint32_t off = U32(ent + 4);
            if (level < 2){ if (!(off & 0x80000000u)) { ent = -1; break; } dir = base + (off & 0x7fffffffu); }
            else ent = base + off;
        }
        if (ent < 0 || !IN(ent, 8)) break;
        uint32_t rva = U32(ent), sz = U32(ent + 4); long fo; R2O(rva, fo);
        if (!IN(fo, sz)) break;
        if (dst){ uint32_t k = sz < *len ? sz : *len; mem_commit(dst, k); memcpy(g_mem + dst, d + fo, k); }
        *len = sz; ok = 1;
        #undef R2O
#undef U16
#undef U32
#undef IN
    } while (0);
    free(d); fclose(f);
    return ok;
}

static void cryptwatch(CPU *c, const ImpEnt *e, uint32_t n_call){
    static int on = -1, state = 0;
    if (on < 0){ const char *v = getenv("WC3_CRYPTWATCH"); on = v && *v == '1'; }
    if (!on || state == 2) return;
    uint32_t t = MR32(0x54c2e4u);
    if (!t || !mem_is_committed(t)) return;
    uint32_t w = MR32(t);
    if (w == 0x55c636e2u){ if (!state){ state = 1; fprintf(stderr, "[cryptwatch] table at 0x%08x is right (call %u, %s)\n", t, n_call, e->name); } return; }
    if (state == 1){
        state = 2;
        fprintf(stderr, "[cryptwatch] table at 0x%08x CHANGED: [0] = %08x, found at import call %u (%s!%s) from 0x%08x\n",
                t, w, n_call, e->dll, e->name, MR32(c->esp));
        guest_backtrace(c, 16);
    }
}

int win32_call(CPU *c, uint32_t idx){
    if (idx >= g_nimps) return 0;
    const ImpEnt *e = &g_imps[idx];
    { void win32_thread_term_check(CPU*); win32_thread_term_check(c); }   /* TerminateThread */
    stat_[idx].calls++; stat_[idx].from = MR32(c->esp); g_win32_calls++;
    cryptwatch(c, e, (uint32_t)g_win32_calls);
    guest_fair_yield();          /* give the other guest threads a turn */
    POP32(c->eip);                      /* take the return address */
    int handled = 1;
    const char *n = e->name;
    uint32_t r = 0;

    /* QueryPerformanceCounter is the most frequent call (frame limiter); to
     * skip the name chain's ~20 strcmps the import index is cached on the
     * first match. */
    if (0) ;
    else if (n[0] == 'Q' && !strcmp(n,"QueryPerformanceCounter")){
        if (ENVC("QPCWHO")) qpc_who(c);
        uint64_t v = host_qpc(); uint32_t p=ARG(c,0);
        if(p){ MR32(p)=(uint32_t)v; MR32(p+4)=(uint32_t)(v>>32); } r=1; }
    else if (g_hk[idx] > 0){
        r = heap_api(c, g_hk[idx]);
        if (g_hk[idx] == g_hk_heapalloc) heapalloc_debug(c, r); }
    else if (!strcmp(n,"GetVersion"))            r = 0x0A280105u;      /* 5.1.2600 */
    else if (!strcmp(n,"GetModuleHandleA")){
        /* NULL: the exe's base; a lifted module: its base while loaded, NULL
         * otherwise; a system DLL the HLE stands in for: the placeholder
         * handle (GetProcAddress resolves by name); anything else is not
         * loaded */
        uint32_t p = ARG(c,0);
        const char *nm = p && mem_is_committed(p) ? (const char*)(g_mem + p) : 0;
        if (nm && module_known_name(nm)) r = module_by_name(nm);
        else r = !p ? 0x00400000u : module_known(p) ? SYSDLL_HANDLE : 0u;
        if (!r) win32_set_last_error(c, 126);            /* ERROR_MOD_NOT_FOUND */
        if (p && mem_is_committed(p)){ static char seen[16][40]; static int ns = 0; int k;
            for (k = 0; k < ns && strncasecmp(seen[k], (const char*)(g_mem+p), 39); k++) ;
            if (k == ns && ns < 16){ snprintf(seen[ns++], 40, "%s", (const char*)(g_mem+p));
                fprintf(stderr,"[hle] GetModuleHandleA(\"%s\") -> 0x%08x (from 0x%08x)\n", seen[ns-1], r, c->eip); } } }
    else if (!strcmp(n,"GetCommandLineA"))       r = g_cmdline;
    else if (!strcmp(n,"GetEnvironmentStrings")||!strcmp(n,"GetEnvironmentStringsW")){
        static uint32_t env = 0;               /* empty block; FreeEnvironmentStrings is a no-op */
        if (!env) env = gheap_alloc(1, 8, 1);
        r = env; }
    else if (!strcmp(n,"FreeEnvironmentStringsA")||!strcmp(n,"FreeEnvironmentStringsW")) r=1;
    else if (!strcmp(n,"GetStartupInfoA")){ uint32_t p=ARG(c,0); if(p){ mem_commit(p,68); memset(g_mem+p,0,68); MR32(p)=68; } r=0; }
    else if (!strcmp(n,"GetStdHandle"))          r = 0x10u + (ARG(c,0) & 3u);
    else if (!strcmp(n,"GetFileType")){
        /* The CRT's _open asks right after CreateFileA: files are DISK, the
         * std handles (GetStdHandle: 0x10..0x13) CHAR, anything else UNKNOWN. */
        uint32_t h = ARG(c,0); extern int fileio_is_handle(uint32_t);
        if (h >= 0x10u && h <= 0x13u) r = 2;
        else if (fileio_is_handle(h)) r = 1;
        else { r = 0; win32_set_last_error(c, 6); } }
    else if (!strcmp(n,"SetHandleCount"))        r = ARG(c,0);
    else if (!strcmp(n,"GetVersionExA")||!strcmp(n,"GetVersionExW")){
        uint32_t p=ARG(c,0);
        if(p){ MR32(p+4)=5; MR32(p+8)=1; MR32(p+12)=2600; MR32(p+16)=2; }
        r = 1; }
    else if (!strcmp(n,"GetACP"))                r = 1252;
    else if (!strcmp(n,"GetOEMCP"))              r = 437;
    else if (!strcmp(n,"GetCPInfo")){ uint32_t p=ARG(c,1); if(p){ mem_commit(p,20); memset(g_mem+p,0,20); MR32(p)=1; } r=1; }
    else if (!strcmp(n,"IsValidCodePage"))       r = 1;
    else if (!strcmp(n,"TlsAlloc")){
        /* freed indices first (server.dll's CRT takes one per load and frees
         * it on unload; with no reuse, hosting would stop working after
         * ~120 sessions) */
        if (tls_nfree) r = tls_free[--tls_nfree];
        else r = (tls_next<128)? (uint32_t)tls_next++ : 0xffffffffu; }
    else if (!strcmp(n,"TlsSetValue")){
        uint32_t i=ARG(c,0), v=ARG(c,1);
        uint32_t arr = (c->fs_base && mem_is_committed(c->fs_base+0x2cu)) ? MR32(c->fs_base+0x2cu) : 0;
        if (arr && i<256u && mem_is_committed(arr+4u*i)){ MR32(arr+4u*i)=v; r=1; }
        else { if(i<128) tls_slot[i]=v; r=1; } }
    else if (!strcmp(n,"TlsGetValue")){
        uint32_t i=ARG(c,0);
        uint32_t arr = (c->fs_base && mem_is_committed(c->fs_base+0x2cu)) ? MR32(c->fs_base+0x2cu) : 0;
        if (arr && i<256u && mem_is_committed(arr+4u*i)) r = MR32(arr+4u*i);
        else r = (i<128)?tls_slot[i]:0;
        /* Windows clears the last error when TlsGetValue succeeds, and code
         * tells a stored NULL from a failure by it: WC3's Prop.cpp
         * (Game.dll 0x16d98e0) took a stale ERROR_FILE_NOT_FOUND for "no
         * property table" */
        win32_set_last_error(c, 0); }
    else if (!strcmp(n,"TlsFree")){
        /* Windows clears the slot in every thread, so the next TlsAlloc of
         * this index reads NULL everywhere: here the main TEB, the created
         * threads' TEBs (0x00C20000 + 0x2000 n) and the timer thread's. */
        uint32_t i = ARG(c,0); r = 0;
        if (i < (uint32_t)tls_next){
            int dup = 0; for (int k = 0; k < tls_nfree; k++) dup |= tls_free[k] == i;
            if (!dup && tls_nfree < 128){
                tls_slot[i] = 0;
                for (uint32_t teb = 0x00C00000u; teb < 0x00D00000u; teb += 0x2000u)
                    if (mem_is_committed(teb + 0x2Cu) && MR32(teb + 0x18u) == teb){
                        uint32_t arr = MR32(teb + 0x2Cu);
                        if (arr && mem_is_committed(arr + 4u*i + 3u)) MR32(arr + 4u*i) = 0;
                    }
                tls_free[tls_nfree++] = i; r = 1; } } }
    /* Critical sections are NOT handled here; win32_ext.c implements them
     * with REAL recursive locks (see cs_for).  With a no-op stub the GIL's
     * fair sharing would break mutual exclusion. */
    else if (!strcmp(n,"GetCurrentThreadId")){
        /* the real thread id from the TEB (fs:[0x24]); every thread must be unique */
        r = (c->fs_base && mem_is_committed(c->fs_base+0x24u)) ? MR32(c->fs_base+0x24u) : 1u;
        if (!r) r = 1u; }
    else if (!strcmp(n,"GetCurrentProcessId"))   r = 100;
    else if (!strcmp(n,"GetCurrentProcess"))     r = 0xffffffffu;
    else if (!strcmp(n,"GetLastError"))          r = win32_get_last_error(c);
    else if (!strcmp(n,"SetLastError")){         win32_set_last_error(c, ARG(c,0)); r = 0; }
    else if (!strcmp(n,"GetTickCount")) r = host_ms();
    else if (!strcmp(n,"QueryPerformanceFrequency")){ uint32_t p=ARG(c,0); if(p){MR32(p)=1000000;MR32(p+4)=0;} r=1; }
    else if (!strcmp(n,"SetUnhandledExceptionFilter")) r = 0;
    else if (!strcmp(n,"UnhandledExceptionFilter"))    r = 0;
    else if (!strcmp(n,"IsProcessorFeaturePresent")){
        /* Truthful for a Pentium 4 class CPU: PF_COMPARE_EXCHANGE_DOUBLE (2),
         * PF_MMX (3), PF_XMMI (6), PF_RDTSC (8), PF_XMMI64 (10).  It used to
         * be TRUE for everything, so D3DX's PSGP selector (0x641790, the only
         * caller) installed its 3DNow! kernels (PF 7), which the lifter cannot
         * run (pswapd/pfacc...). */
        uint32_t f = ARG(c,0);
        r = (f == 2u || f == 3u || f == 6u || f == 8u) ? 1u : 0u;   /* no SSE2 (10): as cpuid_shim says */
        { static uint32_t seen = 0; if (f < 32u && !(seen & (1u << f))){ seen |= 1u << f;
            fprintf(stderr,"[hle] IsProcessorFeaturePresent(%u) -> %u (from 0x%08x)\n", f, r, c->eip); } } }
    else if (!strcmp(n,"GetModuleFileNameA")){ uint32_t p=ARG(c,1),sz=ARG(c,2);
        const char*s="C:\\Program Files\\Codemasters\\Colin McRae Rally 2005\\CMR5.EXE"; char ms[300];
        if (module_file_name(ARG(c,0), ms, sizeof ms)) s = ms;   /* C:\Warcraft III\<module> */
        uint32_t l=(uint32_t)strlen(s);
        if(p&&sz){ if(l>=sz) l=sz-1; memcpy(g_mem+p,s,l); MR8(p+l)=0; } r=l; }
    else if (!strcmp(n,"ExitProcess")){ g_exit_code=(int)ARG(c,0);
        fprintf(stderr,"\n*** guest ExitProcess(%u) - exiting\n",ARG(c,0));
        if(g_exit_armed) longjmp(g_exit_jmp,1); return -1; }
    else if (!strcmp(n,"LoadLibraryA")||!strcmp(n,"LoadLibraryW")){
        /* A lifted module is really loaded (module.c); a system DLL gets the
         * placeholder handle; anything else is not there (system_dll). */
        char nm[260] = ""; uint32_t p0 = ARG(c,0), err = 0;
        if (p0 && mem_is_committed(p0)){
            if (n[11] == 'W'){ uint32_t k = 0; for (; k < sizeof nm - 1 && MR16(p0 + 2*k); k++) nm[k] = (char)MR16(p0 + 2*k); nm[k] = 0; }
            else snprintf(nm, sizeof nm, "%s", (const char*)(g_mem + p0)); }
        if (nm[0] && module_known_name(nm)){
            r = module_load(c, nm, &err);
            if (!r){ void win32_set_last_error(CPU*, uint32_t); win32_set_last_error(c, err); }
        } else if (nm[0] && system_dll(nm)) r = SYSDLL_HANDLE;
        else { r = 0; win32_set_last_error(c, 126u); }   /* ERROR_MOD_NOT_FOUND */
        { static int sh=0; uint32_t p2=ARG(c,0);
          if (sh<32){ sh++; fprintf(stderr,"[dll] LoadLibrary(\"%s\") -> 0x%08x\n", p2?nm:"(NULL)", r); } } }
    else if (!strcmp(n,"GetProcAddress")){
        uint32_t pname = ARG(c,1); r = 0;
        if (module_handle(ARG(c,0))){              /* lifted module: its export table */
            uint32_t err = 0; r = module_proc(ARG(c,0), pname, &err);
            if (!r){ void win32_set_last_error(CPU*, uint32_t); win32_set_last_error(c, err); }
        }
        else if (pname > 0xffff){                  /* by name (not ordinal) */
            const char *want = (const char*)(g_mem + pname);
            for (uint32_t i=0;i<g_nimps;i++)
                if (!strcmp(g_imps[i].name, want)){ r = IMP_MAGIC + i; break; }
            { static int sh=0;
              if (sh<32){ sh++; fprintf(stderr,"[dll] GetProcAddress(0x%08x, \"%s\") -> 0x%08x\n", ARG(c,0), want, r); } }
        } }
    else if (!strcmp(n,"FreeLibrary")){ module_free(c, ARG(c,0)); r = 1; }
    else if (!strcmp(n,"InterlockedIncrement")){ uint32_t p=ARG(c,0); r=++MR32(p); }
    else if (!strcmp(n,"InterlockedDecrement")){ uint32_t p=ARG(c,0); r=--MR32(p); }
    else if (!strcmp(n,"InterlockedExchange")){ uint32_t p=ARG(c,0); r=MR32(p); MR32(p)=ARG(c,1); }
    /* (dest, exchange, comperand): atomic as every HLE call is (the guest lock) */
    else if (!strcmp(n,"InterlockedCompareExchange")){ uint32_t p=ARG(c,0); r=MR32(p); if (r == ARG(c,2)) MR32(p)=ARG(c,1); }
    else if (!strcmp(n,"InterlockedExchangeAdd")){ uint32_t p=ARG(c,0); r=MR32(p); MR32(p)=r+ARG(c,1); }
    else if (!strcmp(n,"DisableThreadLibraryCalls")) r = 1;
    else if (!strcmp(n,"GetCommandLineW")){
        static uint32_t w = 0;
        if (!w){ const char *a = (const char *)(g_mem + g_cmdline); uint32_t l = (uint32_t)strlen(a);
            w = gheap_alloc(1, 2*l + 2, 1); for (uint32_t k = 0; k <= l; k++) MR16(w + 2*k) = (uint8_t)a[k]; }
        r = w; }
    /* The process environment: empty at start, what the game sets itself
     * (CMR5's Intel OpenMP registers its library in __KMP_REGISTERED_LIB_<pid>
     * and reads it back, or aborts) */
    else if (!strcmp(n,"GetEnvironmentVariableA")){            /* (name, buf, size) */
        const char *v = env_get((const char *)(g_mem + ARG(c,0)));
        uint32_t buf = ARG(c,1), size = ARG(c,2);
        if (!v){ win32_set_last_error(c, 203u); r = 0; }      /* ERROR_ENVVAR_NOT_FOUND */
        else { uint32_t l = (uint32_t)strlen(v);
            if (l < size && buf){ memcpy(g_mem + buf, v, l + 1); r = l; } else r = l + 1; } }
    else if (!strcmp(n,"SetEnvironmentVariableA")){            /* (name, value or NULL) */
        env_set((const char *)(g_mem + ARG(c,0)), ARG(c,1) ? (const char *)(g_mem + ARG(c,1)) : 0); r = 1; }
    else if (!strcmp(n,"SetEnvironmentVariableW")){
        char nm[256], val[1024]; uint32_t a = ARG(c,0), b = ARG(c,1); int k;
        for (k = 0; k < 255 && MR16(a + 2*k); k++) nm[k] = (char)MR16(a + 2*k); nm[k] = 0;
        if (b){ for (k = 0; k < 1023 && MR16(b + 2*k); k++) val[k] = (char)MR16(b + 2*k); val[k] = 0; }
        env_set(nm, b ? val : 0); r = 1; }
    /* VERSION.dll: the RT_VERSION resource of a file in the game folder (CMR5
     * checks its Math*.dll are v1.06); VerQueryValueA knows "\\" alone */
    else if (!strcmp(n,"GetFileVersionInfoSizeA")){            /* (path, handle*) */
        uint32_t sz = 0; if (ARG(c,1)) MR32(ARG(c,1)) = 0;
        if (!version_resource((const char *)(g_mem + ARG(c,0)), 0, &sz)){ win32_set_last_error(c, 1813u); sz = 0; }
        r = sz; }
    else if (!strcmp(n,"GetFileVersionInfoA")){                /* (path, handle, len, data) */
        uint32_t len = ARG(c,2), d = ARG(c,3);
        r = d && version_resource((const char *)(g_mem + ARG(c,0)), d, &len) ? 1u : 0u;
        if (!r) win32_set_last_error(c, 1813u); }                 /* ERROR_RESOURCE_TYPE_NOT_FOUND */
    else if (!strcmp(n,"VerQueryValueA")){                     /* (block, subblock, &buffer, &len) */
        uint32_t b = ARG(c,0), pp = ARG(c,2), pl = ARG(c,3); const char *sb = (const char *)(g_mem + ARG(c,1));
        r = 0;
        if (b && (!strcmp(sb, "\\") || !*sb) && MR32(b + 40) == 0xFEEF04BDu){
            if (pp) MR32(pp) = b + 40; if (pl) MR32(pl) = MR16(b + 2); r = 1; }
    }
    else if (!strcmp(n,"GetVolumeInformationA")){
        /* (root, volname, volnamesize, serial*, maxcomp*, flags*, fsname, fsnamesize): the game folder's drive */
        uint32_t vn=ARG(c,1), vns=ARG(c,2), ser=ARG(c,3), mc=ARG(c,4), fl=ARG(c,5), fs=ARG(c,6), fss=ARG(c,7);
        if (vn && vns){ const char *v = "CMR2005"; uint32_t l = (uint32_t)strlen(v); if (l >= vns) l = vns - 1; memcpy(g_mem+vn, v, l); MR8(vn+l) = 0; }
        if (ser) MR32(ser) = 0x57A3C0DEu;
        if (mc) MR32(mc) = 255u;
        if (fl) MR32(fl) = 0x000700FFu;                       /* case preserved, unicode, ... */
        if (fs && fss){ const char *v = "NTFS"; uint32_t l = 4; if (l >= fss) l = fss - 1; memcpy(g_mem+fs, v, l); MR8(fs+l) = 0; }
        r = 1; }
    else if (!strcmp(n,"ImmAssociateContextEx")) r = 1;
    else if (!strcmp(n,"ImmAssociateContext")) r = 0;                 /* the previous input context: none */
    /* (app, key, default, buf, size): no win.ini, every key has its default */
    else if (!strcmp(n,"GetProfileStringA")){
        uint32_t def = ARG(c,2), buf = ARG(c,3), sz = ARG(c,4);
        const char *d = def && mem_is_committed(def) ? (const char *)(g_mem + def) : "";
        uint32_t l = (uint32_t)strlen(d);
        if (buf && sz){ if (l >= sz) l = sz - 1; memcpy(g_mem + buf, d, l); MR8(buf + l) = 0; r = l; } else r = 0; }
    else if (!strcmp(n,"GetSystemDefaultUILanguage")) r = 0x0405u;    /* cs-CZ: the folder is the Czech edition */
    else if (!strcmp(n,"OpenEventA")){ win32_set_last_error(c, 2u); r = 0; }   /* no named event of another process */
    else if (!strcmp(n,"SystemTimeToFileTime")){
        /* SYSTEMTIME (wYear wMonth wDayOfWeek wDay wHour wMinute wSecond wMs) -> 100 ns since 1601 */
        uint32_t st = ARG(c,0), ft = ARG(c,1);
        if (st && ft && mem_is_committed(st + 15)){
            int y = MR16(st), mo = MR16(st+2), d = MR16(st+6);
            static const int cum[12] = { 0,31,59,90,120,151,181,212,243,273,304,334 };
            if (mo < 1 || mo > 12){ r = 0; }
            else {
                int64_t yy = y - 1601;
                int64_t days = yy * 365 + yy / 4 - yy / 100 + yy / 400 + cum[mo - 1] + (d - 1);
                if (mo > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
                uint64_t t = ((((uint64_t)days * 24u + MR16(st+8)) * 60u + MR16(st+10)) * 60u + MR16(st+12)) * 10000000ull
                           + (uint64_t)MR16(st+14) * 10000ull;
                mem_commit(ft, 8); MR32(ft) = (uint32_t)t; MR32(ft+4) = (uint32_t)(t >> 32); r = 1;
            }
        } else r = 0; }
    /* COM: no class is registered.  WC3's movie player asks for DirectShow's
     * filter graph (CoCreateInstance(CLSID_FilterGraph)) and skips the movie
     * when that fails; a success without an object sent it into NULL vtables.
     * The movies come back later as AV1 (docs/PLAN.md). */
    else if (!strcmp(n,"CoCreateInstance")){ uint32_t pp = ARG(c,4); if (pp && mem_is_committed(pp)) MR32(pp) = 0; r = 0x80040154u; }   /* REGDB_E_CLASSNOTREG */
    else if (!strcmp(n,"CoInitialize") || !strcmp(n,"CoInitializeEx")) r = 0;
    else if (!strcmp(n,"CoUninitialize")) r = 0;
    else if (!strcmp(n,"MultiByteToWideChar")){ uint32_t src=ARG(c,2); int sl=(int)ARG(c,3);
        uint32_t dst=ARG(c,4); int dl=(int)ARG(c,5);
        if(sl<0) sl=(int)strlen((char*)(g_mem+src))+1;
        if(dl==0){ r=(uint32_t)sl; } else { int k=0; for(;k<sl&&k<dl;k++) MR16(dst+2u*k)=MR8(src+k); r=(uint32_t)k; } }
    else if (!strcmp(n,"WideCharToMultiByte")){ uint32_t src=ARG(c,2); int sl=(int)ARG(c,3);
        uint32_t dst=ARG(c,4); int dl=(int)ARG(c,5);
        if(sl<0){ sl=0; while(MR16(src+2u*sl)) sl++; sl++; }
        if(dl==0){ r=(uint32_t)sl; } else { int k=0; for(;k<sl&&k<dl;k++) MR8(dst+k)=(uint8_t)MR16(src+2u*k); r=(uint32_t)k; } }
    else if (!strcmp(n,"GetStringTypeW")||!strcmp(n,"GetStringTypeA")){
        /* A: (LCID,dwInfoType,src,cch,out)  W: (dwInfoType,src,cch,out) */
        int aform = (n[13]=='A');
        uint32_t info = ARG(c, aform?1:0);
        uint32_t src  = ARG(c, aform?2:1);
        int      cch  = (int)ARG(c, aform?3:2);
        uint32_t out  = ARG(c, aform?4:3);
        int wide = !aform;
        if (cch < 0){ cch = 0; if(wide){ while(MR16(src+2u*cch)) cch++; } else { while(MR8(src+cch)) cch++; } cch++; }
        if (info == 1 && out){                       /* CT_CTYPE1 */
            for (int k=0;k<cch;k++){
                uint32_t ch = wide ? MR16(src+2u*k) : MR8(src+k);
                uint16_t f = 0x0200;                 /* C1_DEFINED */
                if (ch<128){
                    if (ch>='A'&&ch<='Z') f |= 0x0001|0x0100;
                    if (ch>='a'&&ch<='z') f |= 0x0002|0x0100;
                    if (ch>='0'&&ch<='9') f |= 0x0004|0x0080;
                    if ((ch>='a'&&ch<='f')||(ch>='A'&&ch<='F')) f |= 0x0080;
                    if (ch==' ') f |= 0x0008|0x0040;
                    if (ch=='\t'||ch=='\n'||ch=='\r'||ch==0x0b||ch==0x0c) f |= 0x0008;
                    if (ch<0x20||ch==0x7f) f |= 0x0020;
                    if (ch>0x20&&ch<0x7f&&!(f&(0x0100|0x0004))) f |= 0x0010;
                } else f |= 0x0100;
                MR16(out+2u*k) = f;
            }
        }
        r = 1; }
    else if (!strcmp(n,"LCMapStringW")||!strcmp(n,"LCMapStringA")){
        int wide = (n[11]=='W');
        uint32_t flags = ARG(c,1), src = ARG(c,2); int cch = (int)ARG(c,3);
        uint32_t dst = ARG(c,4); int cchd = (int)ARG(c,5);
        if (cch < 0){ cch = 0; if(wide){ while(MR16(src+2u*cch)) cch++; } else { while(MR8(src+cch)) cch++; } cch++; }
        if (cchd == 0){ r = (uint32_t)cch; }
        else {
            int k=0;
            for (;k<cch && k<cchd;k++){
                uint32_t ch = wide ? MR16(src+2u*k) : MR8(src+k);
                if ((flags & 0x00000100u) && ch<128 && ch>='A' && ch<='Z') ch += 32;   /* LOWERCASE */
                if ((flags & 0x00000200u) && ch<128 && ch>='a' && ch<='z') ch -= 32;   /* UPPERCASE */
                if (wide) MR16(dst+2u*k)=(uint16_t)ch; else MR8(dst+k)=(uint8_t)ch;
            }
            r = (uint32_t)k;
        } }
    else if (!strcmp(n,"GetLocaleInfoA")||!strcmp(n,"GetLocaleInfoW")){
        int wide = (n[13]=='W');
        uint32_t lct = ARG(c,1) & 0xffffu, p = ARG(c,2); int cch = (int)ARG(c,3);
        const char *v = 0; char num[16];
        switch (lct){
        case 0x0001: v="0409"; break;                 /* ILANGUAGE            */
        case 0x0002: v="English"; break;              /* SLANGUAGE            */
        case 0x1001: v="English"; break;              /* SENGLANGUAGE         */
        case 0x1002: v="United States"; break;        /* SENGCOUNTRY          */
        case 0x0003: v="ENU"; break;                  /* SABBREVLANGNAME      */
        case 0x0005: v="US"; break;                   /* SABBREVCTRYNAME      */
        case 0x000E: v="."; break;                    /* SDECIMAL             */
        case 0x000F: v=","; break;                    /* STHOUSAND            */
        case 0x0014: v="2"; break;                    /* IDIGITS              */
        case 0x000B: snprintf(num,sizeof num,"%d",437);  v=num; break; /* IDEFAULTCODEPAGE */
        case 0x1004: snprintf(num,sizeof num,"%d",1252); v=num; break; /* IDEFAULTANSICODEPAGE */
        case 0x0019: v="$"; break;                    /* SCURRENCY            */
        case 0x001D: v="/"; break;                    /* SDATE                */
        case 0x001E: v=":"; break;                    /* STIME                */
        default:     v="";  break;
        }
        uint32_t len = (uint32_t)strlen(v) + 1;
        if (cch == 0) r = wide ? len : len;
        else if (p){ uint32_t k=0; for(;k<len-1 && k<(uint32_t)cch-1;k++){ if(wide) MR16(p+2u*k)=v[k]; else MR8(p+k)=v[k]; }
                     if(wide) MR16(p+2u*k)=0; else MR8(p+k)=0; r=k+1; }
        else r = 0; }
    else if (!strcmp(n,"MessageBoxA")){
        uint32_t txt=ARG(c,1), cap=ARG(c,2);
        fprintf(stderr,"\n*** GUEST MESSAGEBOX ***\n  title: %s\n  text : %s\n***\n",
                cap?(char*)(g_mem+cap):"(none)", txt?(char*)(g_mem+txt):"(none)");
        r = 1; }
    else if (!strcmp(n,"Direct3DCreate9")) r = com_direct3dcreate9();
    else if (!strcmp(n,"Direct3DCreate8")){ uint32_t d3d8_create(void); r = d3d8_create(); }
    else if (!strcmp(n,"DirectInput8Create")) r = com_directinput8create(ARG(c,3));
    else if (!strcmp(n,"DirectSoundCreate") || (!strcmp(n,"#1") && !strcasecmp(e->dll,"DSOUND.dll")))
        r = com_directsoundcreate(ARG(c,1));
    /* (callback, context): one device, the primary driver (lpGuid NULL), as
     * Windows lists it first.  CMR5 makes no sound without a device here. */
    else if (!strcmp(n,"DirectSoundEnumerateA")){
        /* then the device itself with its GUID: CMR5's callback counts only
         * a device with a GUID (lpGuid != NULL), else its sound stays off */
        static uint32_t desc = 0, mod = 0, desc2 = 0, guid = 0;
        if (!desc){ desc = gheap_alloc(1, 32, 1); strcpy((char *)(g_mem + desc), "Primary Sound Driver");
                    desc2 = gheap_alloc(1, 32, 1); strcpy((char *)(g_mem + desc2), "Browser audio (Web Audio)");
                    mod = gheap_alloc(1, 16, 1); strcpy((char *)(g_mem + mod), "wdmaud.drv");
                    guid = gheap_alloc(1, 16, 1);
                    MR32(guid) = 0xBD6DD71Au; MR32(guid+4) = 0x11D03E8Cu; MR32(guid+8) = 0xC0001F9Au; MR32(guid+12) = 0x1D00A04Fu; }
        uint32_t cb = ARG(c,0), ctx = ARG(c,1);
        if (cb){ const uint32_t args[4] = { 0u, desc, mod, ctx }, args2[4] = { guid, desc2, mod, ctx };
            uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
            uint32_t more = call_guest(c, cb, 4, args);
            if (more) more = call_guest(c, cb, 4, args2);
            fprintf(stderr, "[dsound] DirectSoundEnumerateA: the primary driver and one device listed (callback 0x%08x -> %u)\n", cb, more); }
        r = 0; }
    else if (!strcmp(n,"SetConsoleCtrlHandler")) r = 1;
    else if (!strcmp(n,"RtlUnwind"))             r = 0;
    else if (!strcmp(n,"RaiseException") && ARG(c,0) == 0x406D1388u){
        /* MS_VC_EXCEPTION: the thread naming a debugger reads; the caller
         * catches it itself (__except), and without a debugger nothing
         * happens.  There is no SEH dispatch here, so it simply returns. */
        static int said; if (said++ < 4 && ARG(c,3) && mem_is_committed(ARG(c,3) + 7)){
            uint32_t nm = MR32(ARG(c,3) + 4);
            fprintf(stderr, "[thread] named \"%.40s\"\n", nm && mem_is_committed(nm) ? (const char *)(g_mem + nm) : "?"); }
        r = 0; }
    else if (!strcmp(n,"RaiseException")){
        /* There is no guest SEH dispatch, so continuing after a raise is
         * undefined behaviour.  Stop loudly instead; NFSU2_RAISE=continue
         * (test-only) restores the old silent return. */
        raise_exception_report(c);
        if (!ENVC("NFSU2_RAISE") || strcmp(ENVC("NFSU2_RAISE"), "continue")){
            extern void (*g_report_fn)(void);
            if (g_report_fn) g_report_fn();
            fflush(stderr); _exit(5); }
        r = 0; }
    else if (!strcmp(n,"TerminateProcess")){ g_exit_code=(int)ARG(c,1);
        fprintf(stderr,"\n*** guest TerminateProcess(%u) - exiting\n",ARG(c,1));
        if(g_exit_armed) longjmp(g_exit_jmp,1); return -1; }
    else if (winsock_call(c, e->dll, n, &r)) { /* ws2_32, netapi32: winsock.c */ }
    else if (fileio_call(c, n, &r)) { /* file I/O */ }
    else if (user32_call(c, n, &r)) { /* windows and messages: user32.c */ }
    else if (win32_ext_call(c, n, &r)) { /* second-round layer */ }
    else { handled = 0; g_win32_unimpl++; stat_[idx].unimpl++;

           if (unimpl_first==0xffffffffu){ unimpl_first=idx; unimpl_name=n; } }

    if (g_bt_on && (!strcmp(n,"GetModuleFileNameA")||!strcmp(n,"ExitProcess")||!strcmp(n,"RaiseException")||!strcmp(n,"MessageBoxA")))
        guest_backtrace(c, 14);
    if (trace_n < trace_max)
        fprintf(stderr, "  [%4u] %-14s %-32s (%d arg) %s-> 0x%08x\n",
                ++trace_n, e->dll, n, e->nargs, handled?"":"[STUB] ", r);
    else trace_n++;
    netlog(c, e, r, handled);

    c->eax = r;
    c->esp += 4u * (uint32_t)e->nargs;      /* stdcall: the callee cleans up the arguments */
    return 1;
}

void win32_init(void){
    /* the modules' imports (module.c's index space), then the GetProcAddress-only ones */
    g_nmodimps = module_imp_total(); g_nimps = g_nmodimps + NDYNIMP;
    g_imps = calloc(g_nimps, sizeof *g_imps);
    for (uint32_t i = 0; i < g_nmodimps; i++) g_imps[i] = *module_imp(i);
    memcpy(g_imps + g_nmodimps, g_dynimp, sizeof g_dynimp);
    stat_ = calloc(g_nimps ? g_nimps : 1, sizeof *stat_);
    heap_init();
    g_hk = calloc(g_nimps ? g_nimps : 1, 1);
    for (uint32_t i=0;i<g_nimps;i++) g_hk[i] = (int8_t)heap_api_kind(g_imps[i].name);
    g_hk_heapalloc = heap_api_kind("HeapAlloc");
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    tick0 = (uint64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
    { const char *cl = getenv("WC3_CMDLINE"); if (!cl || !*cl) cl = "\"C:\\Program Files\\Codemasters\\Colin McRae Rally 2005\\CMR5.EXE\" NOVIDEO";
      g_cmdline = gheap_alloc(1, (uint32_t)strlen(cl) + 1, 1);
      strcpy((char*)(g_mem+g_cmdline), cl);
      fprintf(stderr, "win32: command line: %s\n", cl); }
    /* the IAT slots are bound by module.c when each module is mapped */
    com_init();
    { void watchdog_start(void); watchdog_start(); }      /* NFSU2_WATCHDOG */
    fprintf(stderr,"win32: %u imports of %d modules + %u GetProcAddress-only functions\n", g_nmodimps, g_modtab_count, NDYNIMP);
}

/* For the watchdog: the imports called most since its last report, with the
 * caller of the latest call: what a loop that never presents is doing. */
void win32_hot_report(void){
    if (!stat_) return;
    uint32_t done[40]; int nd = 0;
    fprintf(stderr, "  imports called since the last report:\n");
    for (int k = 0; k < 40; k++){
        uint32_t best = 0, bi = 0;
        for (uint32_t i = 0; i < g_nimps; i++){
            uint32_t d = stat_[i].calls - stat_[i].seen; int skip = 0;
            for (int j = 0; j < nd; j++) if (done[j] == i) skip = 1;
            if (!skip && d > best){ best = d; bi = i; }
        }
        if (!best) break;
        done[nd++] = bi;
        const char *mod = 0; uint32_t f = module_nearest_fn(stat_[bi].from, &mod);
        fprintf(stderr, "    %-28s %8u  last from 0x%08x (%s sub_%08x)\n", g_imps[bi].name, best,
                stat_[bi].from, mod ? mod : "?", f);
    }
    for (uint32_t i = 0; i < g_nimps; i++) stat_[i].seen = stat_[i].calls;
}

void win32_report(void){
    fprintf(stderr,"\n=== WIN32 LAYER ===\n");
    fprintf(stderr,"  import calls in total : %llu\n",(unsigned long long)g_win32_calls);
    fprintf(stderr,"  not implemented       : %llu\n",(unsigned long long)g_win32_unimpl);
    /* the most called unimplemented ones */
    fprintf(stderr,"  guest threads : %llu started, %llu exited\n",
            (unsigned long long)win32_ext_threads(),(unsigned long long)win32_ext_threads_exited());
    fileio_report();
    heap_report();
    winsock_report();
    { extern void win32_obj_report(void); win32_obj_report(); }
    { extern void res_report(void); res_report(); }
    module_report();
    fprintf(stderr,"  --- NOT IMPLEMENTED (called, a stub returned) ---\n");
    for (uint32_t i=0;i<g_nimps;i++)
        if (stat_[i].unimpl)
            fprintf(stderr,"    %-10s %-14s %-32s %u times\n", i < g_nmodimps ? g_modtab[module_of_import(i)].id : "dynamic", g_imps[i].dll, g_imps[i].name, stat_[i].unimpl);
    fprintf(stderr,"  --- 15 MOST CALLED ---\n");
    for (int k=0;k<15;k++){
        uint32_t best=0, bi=0;
        for (uint32_t i=0;i<g_nimps;i++) if (stat_[i].calls>best){ best=stat_[i].calls; bi=i; }
        if(!best) break;
        fprintf(stderr,"    %-14s %-32s %u\n", g_imps[bi].dll, g_imps[bi].name, best);
        stat_[bi].calls = 0;
    }
    int shown=0;
    for (uint32_t i=0;i<g_nimps && shown<15;i++)
        if (stat_[i].calls){
            /* we do not know whether it was handled; just show the frequently called ones */
        }
    for (uint32_t i=0;i<g_nimps;i++)
        if (stat_[i].calls>1 && shown<15){ fprintf(stderr,"    %-30s %u times\n", g_imps[i].name, stat_[i].calls); shown++; }
}
