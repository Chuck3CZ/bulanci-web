/* oracle.c -- differential oracle: the ORIGINAL x86 code of selected guest
 * functions runs under an x86 emulator (Unicorn) from the same guest state as
 * the lifted C, and the results are compared.
 *
 * Always built: the wrappers at the bottom.  For every listed function the
 * lifted body is generated as sub_X_orig (gen.sh picks the _orig names up from
 * this file) and sub_X here calls it.  Without the oracle that is one load and
 * a predictable branch per call.
 *
 * Built with the emulator only for test binaries:
 *     ORACLE=1 UNICORN_DIR=<unicorn 2.1.x build+source> ./link.sh orig
 * (link.sh adds -DNFSU2_ORACLE and links libunicorn.a; Unicorn is built
 * natively for arm64 from source with cmake, x86 target only.)
 *
 * Run-time options (read once, at the first wrapped call):
 *   NFSU2_ORACLE = cmp | exec
 *     cmp   Unicorn runs the function (and everything it calls) on private
 *           copies of the guest pages it touches, then the lifted C runs on the
 *           real memory as usual.  General registers, the x87 stack top and
 *           ST(0) of a float result, and every page Unicorn touched are
 *           compared; differing dwords are classified as float32 ulps.
 *     exec  Unicorn runs the function on the real guest memory and the lifted
 *           C does not run: the game continues on the original code's
 *           results (a whole-trajectory oracle).  Calls into HLE imports are
 *           bridged to the runtime.
 *   NFSU2_ORACLE_FNS   = hex,hex,...   subset of the wrapped functions (all)
 *   NFSU2_ORACLE_PC    = 24 | 53 | 64  x87 precision control under Unicorn
 *                        (24: the game thread on Windows, whose CreateDevice
 *                        call has no D3DCREATE_FPU_PRESERVE)
 *   NFSU2_ORACLE_EVERY = n             cmp: every n-th call of a function (1)
 *   NFSU2_ORACLE_LOG   = file          cmp: one line per differing dword
 *                                      (first NFSU2_ORACLE_MAXLOG, 2000)
 *   NFSU2_ORACLE_OUT   = file          summary table at exit (also stderr)
 *   NFSU2_ORACLE_WATCH = addr          cmp: log every Unicorn write to the
 *                                      dword at addr (eip, value), to locate
 *                                      the instruction behind a difference;
 *                        stackptr      log stores of stack addresses outside
 *                                      the stack (slow: hooks every write)
 *
 * Unicorn's x87 is QEMU softfloat floatx80 and honours the precision and
 * rounding control fields for FADD/FSUB/FMUL/FDIV/FSQRT, like the hardware.
 * Its FSIN/FCOS/FPTAN/FPATAN/FYL2X/F2XM1 go through host double libm, i.e.
 * the same as the lifter: the oracle cannot check those against silicon.
 */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef void (*guest_fn)(CPU *);
int g_oracle_on = 0;                       /* 1 once NFSU2_ORACLE is active */
static int g_oracle_inited = 0;
void oracle_call(CPU *c, uint32_t va, guest_fn lifted);

/* first call: decide whether the oracle is active */
static inline void oracle_maybe_init(void){
    if (__builtin_expect(g_oracle_inited, 1)) return;
    g_oracle_inited = 1;
#ifdef NFSU2_ORACLE
    const char *m = getenv("NFSU2_ORACLE");
    if (m && *m && strcmp(m, "0")) g_oracle_on = 1;
#else
    if (getenv("NFSU2_ORACLE"))
        fprintf(stderr, "[oracle] NFSU2_ORACLE ignored: this binary was linked without ORACLE=1\n");
#endif
}

#define OW(VA, NAME, ORIG) \
    void ORIG(CPU *); \
    void NAME(CPU *c){ \
        oracle_maybe_init(); \
        if (__builtin_expect(g_oracle_on, 0)) oracle_call(c, VA, ORIG); \
        else ORIG(c); }

#ifndef NFSU2_ORACLE
void oracle_call(CPU *c, uint32_t va, guest_fn lifted){ (void)va; lifted(c); }
#else
/* ======================================================================= */
#include <unicorn/unicorn.h>
#include <pthread.h>

extern void (*g_report_fn)(void);
void dispatch_indirect(CPU *, uint32_t);
#include <stdatomic.h>
extern _Atomic uint64_t g_vclock_freeze;    /* win32.c */
uint64_t host_us(void); uint32_t host_ms(void);
typedef struct { uint32_t iat; const char *dll; const char *name; int nargs; } ImpEnt;
extern const ImpEnt g_imp[]; extern const uint32_t g_imp_count;

#define PG       0x1000u
#define NPOOL    8192u                     /* shadow pages per call (32 MB) */
#define GDT_VA   0xFFFF0000u               /* oracle-private, never guest-committed */
#define MAXFN    16

enum { MODE_CMP = 1, MODE_EXEC = 2 };
static int g_mode = 0, g_pc = 24, g_every = 1, g_maxlog = 2000;
static FILE *g_log = 0;
static const char *g_out = 0;
static uc_engine *g_uc;
static CPU *g_owner;                       /* the guest thread the oracle serves */
static int g_busy;                         /* a comparison is in progress */
static uint8_t *g_pool;                    /* NPOOL shadow pages */
static uint32_t g_shadow_va[NPOOL]; static uint32_t g_nshadow;
static int g_abort; static uint32_t g_abort_addr; static const char *g_abort_why;
static uint8_t g_gdt[PG];
static uint8_t g_exec_mapped[1u << 15];    /* exec: 16 KB blocks mapped onto g_mem */
static uint32_t g_ret_addr;
static uint32_t g_watch;                   /* NFSU2_ORACLE_WATCH */

typedef struct {
    uint32_t va; int enabled;
    uint64_t calls, compared, aborted, identical, differ;
    uint64_t reg_diff, top_diff, st0_diff;
    uint64_t dw_live, dw_stack;            /* differing dwords */
    uint64_t ulp_hist[6];                  /* live: 1, 2-15, 16-255, 256-64K, >64K, non-float */
    uint32_t max_ulp; double max_rel;
    double st0_max_rel;
    uint64_t pages;                        /* pages copied, total */
    uint64_t imports;                      /* exec: HLE calls bridged */
    char why[64];                          /* last abort reason */
} FnStat;
static FnStat g_fs[MAXFN]; static int g_nfs;

static FnStat *fn_stat(uint32_t va){
    for (int i = 0; i < g_nfs; i++) if (g_fs[i].va == va) return &g_fs[i];
    if (g_nfs == MAXFN) return &g_fs[MAXFN - 1];
    FnStat *s = &g_fs[g_nfs++]; memset(s, 0, sizeof *s); s->va = va; s->enabled = 1;
    const char *f = getenv("NFSU2_ORACLE_FNS");
    if (f && *f){
        s->enabled = 0;
        for (const char *p = f; *p; ){
            uint32_t a = (uint32_t)strtoul(p, (char **)&p, 16);
            if (a == va) s->enabled = 1;
            while (*p == ',' || *p == ' ') p++;
            if (*p && !((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F') || *p == 'x' || *p == 'X')) p++;
        }
    }
    return s;
}

/* ---- x87 80-bit <-> double ------------------------------------------- */
static void d_to_f80(double v, uint8_t out[10]){ f80_store(out, v); }
/* round-to-nearest-even (f80_load in cpu.h truncates; exact for PC 24/53) */
static double f80_to_d(const uint8_t in[10]){
    uint64_t m; uint16_t se; memcpy(&m, in, 8); memcpy(&se, in + 8, 2);
    int sign = se >> 15; int e = se & 0x7FFF;
    if (e == 0 && m == 0) return sign ? -0.0 : 0.0;
    if (e == 0x7FFF) return (m << 1) ? (sign ? -NAN : NAN) : (sign ? -INFINITY : INFINITY);
    if (!(m >> 63)) {                     /* unnormal/denormal: tiny for us */
        double v = ldexp((double)m, e - 16383 - 63 + (e == 0));
        return sign ? -v : v;
    }
    uint64_t hi = m >> 11, lo = m & 0x7FF;
    if (lo > 0x400 || (lo == 0x400 && (hi & 1))) hi++;
    double v = ldexp((double)hi, e - 16383 - 52);
    return sign ? -v : v;
}

/* ---- memory hooks ----------------------------------------------------- */
static void abort_run(const char *why, uint32_t a){
    if (!g_abort){ g_abort = 1; g_abort_why = why; g_abort_addr = a; }
}
/* cmp: give Unicorn a private copy of the guest page holding a */
static bool shadow_map(uc_engine *uc, uint32_t a){
    if (!mem_is_committed(a)){ abort_run("unmapped guest address", a); return false; }
    if (g_nshadow == NPOOL){ abort_run("shadow pool full", a); return false; }
    uint32_t pg = a & ~(PG - 1);
    uint8_t *buf = g_pool + (size_t)g_nshadow * PG;
    memcpy(buf, g_mem + pg, PG);
    if (uc_mem_map_ptr(uc, pg, PG, UC_PROT_ALL, buf)){ abort_run("map_ptr", a); return false; }
    g_shadow_va[g_nshadow++] = pg;
    return true;
}
static bool on_unmapped(uc_engine *uc, uc_mem_type type, uint64_t addr, int size,
                        int64_t value, void *ud){
    (void)size; (void)value; (void)ud;
    uint32_t a = (uint32_t)addr;
    if (type == UC_MEM_FETCH_UNMAPPED && a >= 0xE0000000u){
        abort_run("import", a); return false;          /* HLE thunk / COM method */
    }
    if (!mem_is_committed(a)){ abort_run("unmapped guest address", a); return false; }
    if (g_mode == MODE_EXEC){
        uint32_t blk = a >> 14;
        if (g_exec_mapped[blk >> 3] & (1u << (blk & 7))){ abort_run("remap", a); return false; }
        if (uc_mem_map_ptr(uc, (uint64_t)blk << 14, 0x4000, UC_PROT_ALL, g_mem + ((uint64_t)blk << 14))){
            abort_run("map_ptr", a); return false; }
        g_exec_mapped[blk >> 3] |= (uint8_t)(1u << (blk & 7));
        return true;
    }
    return shadow_map(uc, a);
}
/* cmp: pure imports reached from the compared functions: the timing calls of
 * the profiling helpers (0x43BD90/0x43BDF0/0x43BE20; the clock is frozen for
 * the comparison, so the lifted run gets the same values from the runtime's
 * HLE) and the pointer checks of the pool code (0x43BD60, HLE: valid).
 * Returns 1 if emulated. */
static int emulate_import(uint32_t idx){
    if (idx >= g_imp_count) return 0;
    const char *n = g_imp[idx].name;
    uint32_t esp, ret, p = 0, eax; uc_reg_read(g_uc, UC_X86_REG_ESP, &esp);
    if (uc_mem_read(g_uc, esp, &ret, 4)) return 0;
    int nargs;
    if (!strcmp(n, "QueryPerformanceCounter") || !strcmp(n, "QueryPerformanceFrequency")){
        if (uc_mem_read(g_uc, esp + 4, &p, 4)) return 0;
        uint64_t v = !strcmp(n, "QueryPerformanceFrequency") ? 1000000ull : host_us();
        if (p && uc_mem_write(g_uc, p, &v, 8)){
            /* not mapped yet (uc_mem_* calls do not run the hooks) */
            if (!shadow_map(g_uc, p) || (((p + 7) ^ p) & ~(PG - 1) && !shadow_map(g_uc, p + 7)) ||
                uc_mem_write(g_uc, p, &v, 8)) return 0;
            g_abort = 0;
        }
        eax = 1; nargs = 1;
    } else if (!strcmp(n, "timeGetTime") || !strcmp(n, "GetTickCount")){
        eax = host_ms(); nargs = 0;
    } else if (!strcmp(n, "IsBadReadPtr") || !strcmp(n, "IsBadWritePtr")){
        eax = 0; nargs = 2;                  /* as the runtime's HLE: valid */
    } else if (!strcmp(n, "IsBadCodePtr")){
        eax = 0; nargs = 1;
    } else return 0;
    esp += 4u + 4u * (uint32_t)nargs;
    uc_reg_write(g_uc, UC_X86_REG_ESP, &esp); uc_reg_write(g_uc, UC_X86_REG_EAX, &eax);
    uc_reg_write(g_uc, UC_X86_REG_EIP, &ret);
    return 1;
}

static bool on_prot(uc_engine *uc, uc_mem_type type, uint64_t addr, int size,
                    int64_t value, void *ud){
    (void)uc; (void)type; (void)size; (void)value; (void)ud;
    abort_run("write to read-only image section", (uint32_t)addr);
    return false;
}

static uint32_t g_esp_entry;
static void on_watch(uc_engine *uc, uc_mem_type type, uint64_t addr, int size,
                     int64_t value, void *ud){
    (void)type; (void)ud;
    if (!g_log) return;
    if (g_watch == 1){   /* "stackptr": stack addresses stored outside the stack */
        uint32_t v = (uint32_t)value;
        if (size != 4 || v > g_esp_entry || v + 0x100000u < g_esp_entry ||
            ((uint32_t)addr <= g_esp_entry && (uint32_t)addr + 0x100000u >= g_esp_entry)) return;
    } else if (addr + (uint64_t)size <= g_watch || addr >= (uint64_t)g_watch + 4) return;
    uint32_t eip, esp; uc_reg_read(uc, UC_X86_REG_EIP, &eip); uc_reg_read(uc, UC_X86_REG_ESP, &esp);
    fprintf(g_log, "  watch: eip=%08x esp=%08x write [%08x]/%d = %08llx\n", eip, esp,
            (uint32_t)addr, size, (unsigned long long)value);
}

/* cmp: guest functions Unicorn should skip, returning eax = 0 (none needed
 * now: the timing imports of the profiling helpers 0x43BD90/0x43BDF0/
 * 0x43BE20 are emulated against the frozen clock, see emulate_import) */
static const uint32_t g_stub_ret0[] = { 0 };   /* none needed since the clock freeze */
static void on_stub(uc_engine *uc, uint64_t addr, uint32_t size, void *ud){
    (void)size; (void)ud;
    if (g_mode != MODE_CMP) return;
    for (unsigned i = 0; i < sizeof g_stub_ret0 / sizeof *g_stub_ret0; i++)
        if (g_stub_ret0[i] && (uint32_t)addr == g_stub_ret0[i]){
            uint32_t esp, ret, z = 0; uc_reg_read(uc, UC_X86_REG_ESP, &esp);
            uc_mem_read(uc, esp, &ret, 4); esp += 4;
            uc_reg_write(uc, UC_X86_REG_ESP, &esp); uc_reg_write(uc, UC_X86_REG_EAX, &z);
            uc_reg_write(uc, UC_X86_REG_EIP, &ret);
            return;
        }
}

/* ---- GDT: flat code/data plus a data segment for fs (TEB) ------------- */
static void gdt_entry(int i, uint32_t base, uint32_t limit, int code, int dpl){
    uint8_t *d = g_gdt + 8 * i;
    limit >>= 12;                          /* 4 KB granularity */
    d[0] = limit & 0xFF; d[1] = (limit >> 8) & 0xFF;
    d[2] = base & 0xFF; d[3] = (base >> 8) & 0xFF; d[4] = (base >> 16) & 0xFF;
    d[5] = (uint8_t)(0x80 | (dpl << 5) | 0x10 | (code ? 0x0A : 0x02));   /* present, S, type */
    d[6] = (uint8_t)(0xC0 | ((limit >> 16) & 0x0F));                     /* G, D/B */
    d[7] = (base >> 24) & 0xFF;
}
static void set_fs(uint32_t base){
    gdt_entry(4, base, 0xFFFFF000u, 0, 3);
    uc_mem_write(g_uc, GDT_VA, g_gdt, 8 * 6);
    uint32_t fs = (4 << 3) | 3; uc_reg_write(g_uc, UC_X86_REG_FS, &fs);
}

static bool on_sentinel(uc_engine *uc, uc_mem_type type, uint64_t addr, int size,
                        int64_t value, void *ud);
static void oracle_report(void);
static void (*g_prev_report)(void);
static void chained_report(void){ oracle_report(); if (g_prev_report) g_prev_report(); }

static int oracle_setup(CPU *c){
    const char *m = getenv("NFSU2_ORACLE");
    g_mode = (m && !strcmp(m, "exec")) ? MODE_EXEC : MODE_CMP;
    const char *e;
    if ((e = getenv("NFSU2_ORACLE_PC"))) g_pc = atoi(e);
    if ((e = getenv("NFSU2_ORACLE_EVERY"))) g_every = atoi(e) > 0 ? atoi(e) : 1;
    if ((e = getenv("NFSU2_ORACLE_MAXLOG"))) g_maxlog = atoi(e);
    if ((e = getenv("NFSU2_ORACLE_LOG")) && *e) g_log = fopen(e, "w");
    g_out = getenv("NFSU2_ORACLE_OUT");
    if (uc_open(UC_ARCH_X86, UC_MODE_32, &g_uc)){ fprintf(stderr, "[oracle] uc_open failed\n"); return 0; }
    /* image: .text and .rdata alias the guest memory (read-only + exec in
     * cmp; writable in exec so the game's own state stays in one place) */
    int rw = g_mode == MODE_EXEC ? UC_PROT_WRITE : 0;
    if (uc_mem_map_ptr(g_uc, 0x00401000u, 0x00382000u, UC_PROT_READ | UC_PROT_EXEC | rw, g_mem + 0x00401000u) ||
        uc_mem_map_ptr(g_uc, 0x00783000u, 0x00065000u, UC_PROT_READ | UC_PROT_EXEC | rw, g_mem + 0x00783000u)){
        fprintf(stderr, "[oracle] cannot map the image\n"); return 0; }
    if (g_mode == MODE_EXEC)
        for (uint32_t b = 0x00401000u >> 14; b < (0x00783000u + 0x65000u + 0x3FFFu) >> 14; b++)
            g_exec_mapped[b >> 3] |= (uint8_t)(1u << (b & 7));   /* never remap over the image */
    if (mem_is_committed(GDT_VA)){ fprintf(stderr, "[oracle] GDT address is guest memory\n"); return 0; }
    uc_mem_map_ptr(g_uc, GDT_VA, PG, UC_PROT_READ | UC_PROT_WRITE, g_gdt);
    memset(g_gdt, 0, sizeof g_gdt);
    gdt_entry(1, 0, 0xFFFFF000u, 1, 3);    /* code, ring 3 */
    gdt_entry(2, 0, 0xFFFFF000u, 0, 3);    /* data, ring 3 */
    gdt_entry(3, 0, 0xFFFFF000u, 0, 0);    /* data, ring 0 (ss while cpl = 0) */
    gdt_entry(5, 0, 0xFFFFF000u, 1, 0);    /* code, ring 0 */
    uc_mem_write(g_uc, GDT_VA, g_gdt, 8 * 6);
    uc_x86_mmr gdtr = { 0, GDT_VA, 8 * 6 - 1, 0 };
    uc_reg_write(g_uc, UC_X86_REG_GDTR, &gdtr);
    uint32_t sel;
    sel = (3 << 3) | 0; uc_reg_write(g_uc, UC_X86_REG_SS, &sel);
    sel = (2 << 3) | 3; uc_reg_write(g_uc, UC_X86_REG_DS, &sel); uc_reg_write(g_uc, UC_X86_REG_ES, &sel);
    uc_reg_write(g_uc, UC_X86_REG_GS, &sel);
    set_fs(c->fs_base);
    if (g_mode == MODE_CMP) g_pool = malloc((size_t)NPOOL * PG);
    uc_hook h;
    uc_hook_add(g_uc, &h, UC_HOOK_MEM_UNMAPPED, (void *)on_sentinel, 0, 1, 0);
    uc_hook_add(g_uc, &h, UC_HOOK_MEM_WRITE_PROT, (void *)on_prot, 0, 1, 0);
    for (unsigned i = 0; i < sizeof g_stub_ret0 / sizeof *g_stub_ret0; i++)
        if (g_stub_ret0[i]) uc_hook_add(g_uc, &h, UC_HOOK_CODE, (void *)on_stub, 0, g_stub_ret0[i], g_stub_ret0[i]);
    if ((e = getenv("NFSU2_ORACLE_WATCH")) && *e){
        g_watch = !strcmp(e, "stackptr") ? 1u : (uint32_t)strtoul(e, 0, 0);
        if (g_watch == 1) uc_hook_add(g_uc, &h, UC_HOOK_MEM_WRITE, (void *)on_watch, 0, 1, 0);
        else uc_hook_add(g_uc, &h, UC_HOOK_MEM_WRITE, (void *)on_watch, 0, g_watch & ~3u, (g_watch & ~3u) + 3);
    }
    g_owner = c;
    g_prev_report = g_report_fn; g_report_fn = chained_report;
    atexit(oracle_report);
    fprintf(stderr, "[oracle] %s mode, x87 PC=%d, every %d, Unicorn %s\n",
            g_mode == MODE_EXEC ? "exec" : "cmp", g_pc, g_every, uc_version(0, 0) ? "2.x" : "?");
    return 1;
}

static uint16_t uc_fcw(const CPU *c){
    uint16_t pcb = g_pc == 24 ? 0 : g_pc == 53 ? 2 : 3;
    return (uint16_t)((c->fcw & ~0x0300u) | (pcb << 8));
}
static void cpu_to_uc(const CPU *c, uint32_t eip){
    static const int R[8] = { UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                              UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI };
    for (int i = 0; i < 8; i++) uc_reg_write(g_uc, R[i], &c->r[i]);
    uint32_t fl = eflags_get(c) & ~0x00240000u;          /* no ID/AC games here */
    uc_reg_write(g_uc, UC_X86_REG_EFLAGS, &fl);
    uc_reg_write(g_uc, UC_X86_REG_EIP, &eip);
    uint16_t cw = uc_fcw(c); uc_reg_write(g_uc, UC_X86_REG_FPCW, &cw);
    uint16_t sw = (uint16_t)((c->fsw & ~0x3800u) | ((c->top & 7) << 11));
    uc_reg_write(g_uc, UC_X86_REG_FPSW, &sw);
    uint16_t tag = 0;                                     /* all valid: no stack faults */
    uc_reg_write(g_uc, UC_X86_REG_FPTAG, &tag);
    for (int i = 0; i < 8; i++){ uint8_t f[10]; d_to_f80(c->st[i], f); uc_reg_write(g_uc, UC_X86_REG_FP0 + i, f); }
    for (int i = 0; i < 8; i++) uc_reg_write(g_uc, UC_X86_REG_XMM0 + i, c->xmm[i].b);
    uint32_t mx = 0x1F80; uc_reg_write(g_uc, UC_X86_REG_MXCSR, &mx);
    set_fs(c->fs_base);
}
static void uc_to_cpu(CPU *c){
    static const int R[8] = { UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                              UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI };
    for (int i = 0; i < 8; i++) uc_reg_read(g_uc, R[i], &c->r[i]);
    uint32_t fl; uc_reg_read(g_uc, UC_X86_REG_EFLAGS, &fl);
    eflags_set(c, (fl & ~0x00240000u) | (c->sysfl & 0x00240000u));
    uint16_t sw; uc_reg_read(g_uc, UC_X86_REG_FPSW, &sw);
    c->top = (sw >> 11) & 7; c->fsw = (uint16_t)(sw & ~0x3800u);
    uint16_t cw; uc_reg_read(g_uc, UC_X86_REG_FPCW, &cw);
    c->fcw = (uint16_t)((cw & ~0x0300u) | (c->fcw & 0x0300u));   /* PC stays the lifted thread's */
    for (int i = 0; i < 8; i++){ uint8_t f[10]; uc_reg_read(g_uc, UC_X86_REG_FP0 + i, f); c->st[i] = f80_to_d(f); }
    for (int i = 0; i < 8; i++) uc_reg_read(g_uc, UC_X86_REG_XMM0 + i, c->xmm[i].b);
}

static void unmap_shadow(void){
    for (uint32_t i = 0; i < g_nshadow; i++) uc_mem_unmap(g_uc, g_shadow_va[i], PG);
    g_nshadow = 0;
}

/* The function returns into SENTINEL: its return address on the emulated
 * stack is replaced, so the return is caught as a fetch from an unmapped
 * page (uc_emu_start's "until" is not reliable once that address sits in
 * the middle of a cached translation block, which let runs continue into
 * the caller).  Recursion through the caller cannot fake it either. */
#define SENTINEL 0xFFFE0000u
static int g_returned;
static bool on_sentinel(uc_engine *uc, uc_mem_type type, uint64_t addr, int size,
                        int64_t value, void *ud){
    (void)uc; (void)type; (void)size; (void)value; (void)ud;
    if ((uint32_t)addr == SENTINEL){ g_returned = 1; return false; }
    return on_unmapped(uc, type, addr, size, value, ud);
}
static int exec_map(uint32_t a){
    uint32_t blk = a >> 14;
    if (g_exec_mapped[blk >> 3] & (1u << (blk & 7))) return 1;
    if (!mem_is_committed(a)) return 0;
    if (uc_mem_map_ptr(g_uc, (uint64_t)blk << 14, 0x4000, UC_PROT_ALL, g_mem + ((uint64_t)blk << 14))) return 0;
    g_exec_mapped[blk >> 3] |= (uint8_t)(1u << (blk & 7));
    return 1;
}

/* run the function at va until it returns; exec mode bridges import calls */
static int emulate(CPU *c, uint32_t va, FnStat *s){
    uint32_t esp0 = c->esp, eip = va, sent = SENTINEL;
    /* plant the sentinel (cmp: in the private copy of the stack page) */
    if (g_mode == MODE_CMP){
        if (uc_mem_write(g_uc, esp0, &sent, 4) &&
            (!shadow_map(g_uc, esp0) || uc_mem_write(g_uc, esp0, &sent, 4))){
            abort_run("cannot plant the return sentinel", esp0); return 0; }
    } else {
        if (!exec_map(esp0) || uc_mem_write(g_uc, esp0, &sent, 4)){
            abort_run("cannot plant the return sentinel", esp0); return 0; }
    }
    g_returned = 0;
    int rc = 0;
    for (int guard = 0; guard < 1000000; guard++){
        g_abort = 0;
        uc_err err = uc_emu_start(g_uc, eip, 0, 0, 0);
        uint32_t pc, esp; uc_reg_read(g_uc, UC_X86_REG_EIP, &pc); uc_reg_read(g_uc, UC_X86_REG_ESP, &esp);
        if (g_returned){
            if (esp > esp0){ rc = 1; break; }
            abort_run("sentinel reached with the stack below the entry", pc); break;
        }
        if (g_mode == MODE_CMP && g_abort && !strcmp(g_abort_why, "import") &&
            g_abort_addr >= 0xF0000000u && emulate_import(g_abort_addr - 0xF0000000u)){
            s->imports++;
            uc_reg_read(g_uc, UC_X86_REG_EIP, &eip);
            continue;
        }
        if (g_mode == MODE_EXEC && g_abort && !strcmp(g_abort_why, "import")){
            /* the call pushed its return address; let the runtime's HLE run it */
            uc_to_cpu(c);
            dispatch_indirect(c, g_abort_addr);
            s->imports++;
            eip = c->eip;
            cpu_to_uc(c, eip);
            continue;
        }
        if (!g_abort) abort_run(err ? uc_strerror(err) : "stopped", pc);
        break;
    }
    if (g_mode == MODE_EXEC) MR32(esp0) = g_ret_addr;   /* the real stack keeps the real one */
    if (!rc && !g_abort) abort_run("too many import bridges", eip);
    return rc;
}

static int g_nlog; static uint32_t g_esp0;
static void log_diff(FnStat *s, uint64_t callno, const char *what, uint32_t a, uint32_t ov, uint32_t lv){
    if (!g_log || g_nlog >= g_maxlog) return;
    g_nlog++;
    float of, lf; memcpy(&of, &ov, 4); memcpy(&lf, &lv, 4);
    fprintf(g_log, "%06x #%llu %s %08x oracle=%08x (%.9g) lifted=%08x (%.9g)  [esp0=%08x ret=%08x]\n",
            s->va, (unsigned long long)callno, what, a, ov, (double)of, lv, (double)lf, g_esp0, g_ret_addr);
}
static int is_floaty(uint32_t v){ uint32_t e = (v >> 23) & 0xFF; return v == 0 || v == 0x80000000u || (e > 0x40 && e < 0xBF); }

void oracle_call(CPU *c, uint32_t va, guest_fn lifted){
    static int setup_ok = -1;
    if (setup_ok < 0) setup_ok = oracle_setup(c);
    if (!setup_ok || c != g_owner || g_busy){ lifted(c); return; }
    FnStat *s = fn_stat(va);
    s->calls++;
    if (!s->enabled || (g_mode == MODE_CMP && (s->calls - 1) % (uint64_t)g_every)){ lifted(c); return; }
    g_ret_addr = MR32(c->esp);

    if (g_mode == MODE_EXEC){
        cpu_to_uc(c, va);
        if (!emulate(c, va, s)){
            /* the state is partly updated: nothing sane to fall back to */
            fprintf(stderr, "[oracle] exec %06x aborted (%s at %08x) -- the run is invalid from here\n",
                    va, g_abort_why, g_abort_addr);
            s->aborted++; snprintf(s->why, sizeof s->why, "%s@%08x", g_abort_why, g_abort_addr);
            uc_to_cpu(c); c->eip = g_ret_addr; return;
        }
        uc_to_cpu(c); c->eip = g_ret_addr; s->compared++;
        return;
    }

    /* ---- cmp ---- */
    CPU pre = *c;
    atomic_store(&g_vclock_freeze, host_us());
    g_busy = 1; g_esp0 = c->esp; g_esp_entry = c->esp;
    if (g_watch && g_log) fprintf(g_log, "%06x #%llu begin esp=%08x ret=%08x\n", va,
                                  (unsigned long long)s->calls, c->esp, g_ret_addr);
    cpu_to_uc(c, va);
    int ok = emulate(c, va, s);
    CPU orc = *c; uc_to_cpu(&orc);
    uc_mem_write(g_uc, c->esp, &g_ret_addr, 4);   /* undo the sentinel before comparing */
    lifted(c);                              /* the real run, on the real memory */
    g_busy = 0;
    atomic_store(&g_vclock_freeze, 0);
    if (!ok){
        s->aborted++; snprintf(s->why, sizeof s->why, "%s@%08x", g_abort_why, g_abort_addr);
        if (g_abort_addr >= 0xF0000000u && g_abort_addr - 0xF0000000u < g_imp_count)
            snprintf(s->why, sizeof s->why, "import %s", g_imp[g_abort_addr - 0xF0000000u].name);
        if (g_log && g_nlog < g_maxlog){
            /* where: .text return addresses on the emulated stack */
            uint32_t esp; uc_reg_read(g_uc, UC_X86_REG_ESP, &esp);
            g_nlog++;
            fprintf(g_log, "%06x #%llu abort: %s at %08x; stack:", va, (unsigned long long)s->calls,
                    g_abort_why, g_abort_addr);
            int n = 0;
            for (uint32_t a = esp; a < g_esp0 + 4 && n < 16; a += 4){
                uint32_t v; if (uc_mem_read(g_uc, a, &v, 4)) break;
                if (v >= 0x00401000u && v < 0x00783000u){ fprintf(g_log, " %08x", v); n++; }
            }
            fprintf(g_log, "\n");
        }
        unmap_shadow(); return;
    }
    s->compared++; s->pages += g_nshadow;
    int differ = 0;
    for (int i = 0; i < 8; i++) if (orc.r[i] != c->r[i]){
        differ = 1; s->reg_diff++;
        log_diff(s, s->calls, "reg", (uint32_t)i, orc.r[i], c->r[i]);
    }
    if ((orc.top & 7) != (c->top & 7)){ differ = 1; s->top_diff++; log_diff(s, s->calls, "x87top", 0, (uint32_t)orc.top, (uint32_t)c->top); }
    else if (((pre.top - 1) & 7) == (c->top & 7)){          /* float result in ST(0) */
        double o = orc.st[c->top & 7], l = c->st[c->top & 7];
        if (memcmp(&o, &l, 8) && !(o != o && l != l)){
            differ = 1; s->st0_diff++;
            double rel = fabs(o - l) / (fabs(o) > 1e-30 ? fabs(o) : 1e-30);
            if (rel > s->st0_max_rel) s->st0_max_rel = rel;
            if (g_log && g_nlog < g_maxlog){ g_nlog++;
                fprintf(g_log, "%06x #%llu st0 oracle=%.17g lifted=%.17g\n", va, (unsigned long long)s->calls, o, l); }
        }
    }
    uint32_t esp_after = c->esp;
    for (uint32_t k = 0; k < g_nshadow; k++){
        uint32_t pg = g_shadow_va[k];
        const uint32_t *o = (const uint32_t *)(g_pool + (size_t)k * PG);
        const uint32_t *l = (const uint32_t *)(g_mem + pg);
        if (!memcmp(o, l, PG)) continue;
        for (uint32_t w = 0; w < PG / 4; w++){
            if (o[w] == l[w]) continue;
            uint32_t a = pg + 4 * w;
            /* below the final stack pointer = the callee's dead locals */
            if (a < esp_after && a + 0x20000u >= esp_after){
                s->dw_stack++;
                if (getenv("NFSU2_ORACLE_LOGSTACK")) log_diff(s, s->calls, "deadstack", a, o[w], l[w]);
                continue; }
            s->dw_live++; differ = 1;
            uint32_t ov = o[w], lv = l[w];
            int b;
            if (is_floaty(ov) && is_floaty(lv) && ((ov ^ lv) >> 31) == 0){
                uint32_t ulp = ov > lv ? ov - lv : lv - ov;
                if (ulp > s->max_ulp) s->max_ulp = ulp;
                float of, lf; memcpy(&of, &ov, 4); memcpy(&lf, &lv, 4);
                double rel = fabs((double)of - (double)lf) / (fabs((double)of) > 1e-30 ? fabs((double)of) : 1e-30);
                if (rel > s->max_rel) s->max_rel = rel;
                b = ulp <= 1 ? 0 : ulp < 16 ? 1 : ulp < 256 ? 2 : ulp < 65536 ? 3 : 4;
            } else b = 5;
            s->ulp_hist[b]++;
            log_diff(s, s->calls, b == 5 ? "mem(int)" : "mem", a, ov, lv);
        }
    }
    if (differ) s->differ++; else s->identical++;
    unmap_shadow();
}

static void oracle_report(void){
    static int done; if (done) return; done = 1;
    FILE *outs[2] = { stderr, g_out && *g_out ? fopen(g_out, "w") : 0 };
    for (int k = 0; k < 2; k++){
        FILE *f = outs[k]; if (!f) continue;
        fprintf(f, "\n=== oracle (%s, x87 PC=%d under Unicorn) ===\n", g_mode == MODE_EXEC ? "exec" : "cmp", g_pc);
        fprintf(f, "(differ = registers, x87 result or live memory; dw_stack = dead locals below the final esp, not counted)\n");
        fprintf(f, "%-8s %9s %9s %7s %9s %9s %6s %6s %6s %9s %9s  live ulp: %s  %9s %10s %10s %7s %s\n",
                "fn", "calls", "compared", "abort", "identical", "differ", "regs", "x87top", "st0",
                "dw_live", "dw_stack", "1/2-15/16-255/256-64K/>64K/int", "max_ulp", "max_rel", "st0_rel",
                "pages", "imports/why");
        for (int i = 0; i < g_nfs; i++){
            FnStat *s = &g_fs[i];
            fprintf(f, "%06x %9llu %9llu %7llu %9llu %9llu %6llu %6llu %6llu %9llu %9llu  %llu/%llu/%llu/%llu/%llu/%llu  %9u %10.3g %10.3g %7.0f %llu %s\n",
                    s->va, (unsigned long long)s->calls, (unsigned long long)s->compared,
                    (unsigned long long)s->aborted, (unsigned long long)s->identical,
                    (unsigned long long)s->differ, (unsigned long long)s->reg_diff,
                    (unsigned long long)s->top_diff, (unsigned long long)s->st0_diff,
                    (unsigned long long)s->dw_live, (unsigned long long)s->dw_stack,
                    (unsigned long long)s->ulp_hist[0], (unsigned long long)s->ulp_hist[1],
                    (unsigned long long)s->ulp_hist[2], (unsigned long long)s->ulp_hist[3],
                    (unsigned long long)s->ulp_hist[4], (unsigned long long)s->ulp_hist[5],
                    s->max_ulp, s->max_rel, s->st0_max_rel,
                    s->compared ? (double)s->pages / (double)s->compared : 0.0,
                    (unsigned long long)s->imports, s->why);
        }
        if (k) fclose(f);
    }
    if (g_log) fflush(g_log);
}
#endif /* NFSU2_ORACLE */

/* ---- wrapped guest functions (addresses verified in SPEED2.EXE v1.2 NA) */
OW(0x00609ce0u, sub_00609ce0, sub_00609ce0_orig)   /* World::DoTimestep(dt): the whole simulation step */
OW(0x005b72d0u, sub_005b72d0, sub_005b72d0_orig)   /* PhysicsMover::Update(dt): one car, one frame */
OW(0x005aa480u, sub_005aa480, sub_005aa480_orig)   /* rigid-body substep integrator (n per frame) */
OW(0x005ac260u, sub_005ac260, sub_005ac260_orig)   /* drift-event handling: yaw assist + rear grip */
OW(0x0056ed40u, sub_0056ed40, sub_0056ed40_orig)   /* DriftCar::Update(dt): detection, multiplier, crash */
OW(0x0056fd10u, sub_0056fd10, sub_0056fd10_orig)   /* DriftScore::Update: accrual (x dt) and banking */
OW(0x0056c940u, sub_0056c940, sub_0056c940_orig)   /* drift point rate: |slip| * (v / 8.33)^2 */
