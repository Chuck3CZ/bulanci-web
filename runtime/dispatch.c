/* Indirect call dispatch: guest address -> lifted native function.
 * Two O(1) direct tables over the code of every lifted module (cpu.h:
 * DISPATCH_HI = Game.dll's .text, DISPATCH_LO = war3.exe and the small DLLs
 * below 0x00800000): the function itself (or the run-time patch that replaces
 * it), so a hit is one load and one call.  Every lifted function is in the
 * table from the start, mapped or not: lifted code only reaches a module's
 * addresses once the module is loaded (module.c).
 * Based on nfsu2-web's dispatch.c (one table over SPEED2.EXE's .text). */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "module.h"

typedef void (*guest_fn)(CPU*);
uint64_t g_icall_hits = 0;

guest_fn_t g_dtab_hi[DISPATCH_HI_SIZE];   /* cpu.h */
guest_fn_t g_dtab_lo[DISPATCH_LO_SIZE];
static int g_dtab_ready;
static uint32_t g_icall_want;             /* ICALL=<address> (diagnostics), 0 = off */
static void icall_note(CPU *c, uint32_t addr);

/* the table slot of a code address, or NULL outside both tables */
static guest_fn_t *slot(uint32_t a){
    uint32_t o = a - DISPATCH_HI_BASE;
    if (o < DISPATCH_HI_SIZE) return &g_dtab_hi[o];
    o = a - DISPATCH_LO_BASE;
    return o < DISPATCH_LO_SIZE ? &g_dtab_lo[o] : 0;
}
/* the lifted function at exactly a (binary search in its module's list) */
static guest_fn lifted_at(uint32_t a){
    for (int i = 0; i < g_modtab_count; i++){
        const ModEntry *m = &g_modtab[i];
        if (a - m->info->text >= m->info->text_size) continue;
        uint32_t lo = 0, hi = *m->fn_count;
        while (lo < hi){ uint32_t md = (lo + hi) / 2; if (m->fn_addr[md] < a) lo = md + 1; else hi = md; }
        return lo < *m->fn_count && m->fn_addr[lo] == a ? m->fn_ptr[lo] : 0;
    }
    return 0;
}

/* statistics */
uint64_t g_stat_indirect_ok = 0, g_stat_indirect_miss = 0, g_stat_unsupported = 0;
#define MISS_MAX 64
static uint32_t g_miss[MISS_MAX], g_miss_from[MISS_MAX]; static int g_miss_n = 0;
static uint32_t g_unsup[MISS_MAX]; static const char *g_unsup_mn[MISS_MAX]; static int g_unsup_n = 0;

extern void (*g_backtrace_fn)(struct CPU*,int);
void guest_backtrace(CPU*,int);

/* --- run-time patches: replace indirect call targets ---
 * Without regenerating (only functions that are called indirectly).  A patch
 * goes into the dispatch table itself (at dispatch_init, or at once when that
 * has run). */
#define PATCH_MAX 32
static struct { uint32_t addr; guest_fn fn; } g_patch[PATCH_MAX];
static int g_patch_n = 0;
static void patch_apply(void){
    for (int i = 0; i < g_patch_n; i++){ guest_fn_t *s = slot(g_patch[i].addr); if (s) *s = g_patch[i].fn; }
    if (g_icall_want){ guest_fn_t *s = slot(g_icall_want); if (s) *s = 0; }   /* ICALL: through dispatch_indirect */
}
void dispatch_patch(uint32_t a, guest_fn f){
    if (!slot(a)){ fprintf(stderr, "dispatch_patch: address %08x outside the lifted code, ignored\n", a); return; }
    if (g_patch_n < PATCH_MAX){ g_patch[g_patch_n].addr = a; g_patch[g_patch_n].fn = f; g_patch_n++; }
    if (g_dtab_ready) patch_apply();
}
static guest_fn patch_at(uint32_t addr){
    for (int i = 0; i < g_patch_n; i++) if (g_patch[i].addr == addr) return g_patch[i].fn;
    return 0;
}

void dispatch_init(void){
    if (g_dtab_ready) return;
    g_dtab_ready = 1;
    g_backtrace_fn = (void(*)(struct CPU*,int))guest_backtrace;
    { const char *e = getenv("ICALL"); g_icall_want = e ? (uint32_t)strtoul(e, 0, 0) : 0; }
    uint32_t n = 0, dup = 0, oob = 0;
    for (int i = 0; i < g_modtab_count; i++){
        const ModEntry *m = &g_modtab[i];
        for (uint32_t k = 0; k < *m->fn_count; k++){
            guest_fn_t *s = slot(m->fn_addr[k]);
            if (!s){ oob++; continue; }
            if (*s) dup++;
            *s = m->fn_ptr[k]; n++;
        }
    }
    patch_apply();
    fprintf(stderr, "dispatch: %u functions of %d modules registered (%u duplicates, %u outside the tables), tables %.1f MB\n",
            n, g_modtab_count, dup, oob, (DISPATCH_HI_SIZE + DISPATCH_LO_SIZE) * (double)sizeof(guest_fn) / 1048576.0);
}

int  win32_call(CPU*, uint32_t);
int  run_generated(CPU*, uint32_t);
void genexec_report(void);
int  com_call(CPU*, uint32_t);
int  g_guest_exited = 0;

/* A tail jump (DISPATCH_TAIL in cpu.h): target in c->eip.  Lifted targets
 * and patches are tail-called so the jumping function's frame is gone; COM,
 * Win32 and generated code return here and end the chain like a call would. */
void dispatch_tail(CPU *c){
    uint32_t addr = c->eip;
    guest_fn_t *s = slot(addr);
    guest_fn f = s ? *s : 0;               /* ICALL's target is not in the table: dispatch_indirect counts it */
    if (f){ g_stat_indirect_ok++;
#ifdef __wasm__
        __attribute__((musttail))
#endif
        return f(c); }
    dispatch_indirect(c, addr);
}

/* ICALL=<address>: count the INDIRECT calls to that function and dump the
 * call chain for the first few (virtual calls do not show up in the static
 * call graph). */
static void icall_note(CPU *c, uint32_t addr){
    g_icall_hits++;
    static int shown = 0;
    if (shown < 3){ shown++;
        fprintf(stderr,"  [icall] 0x%08x called (arg1=%08x arg2=%08x arg3=%08x)\n",
                addr, MR32(c->esp+4), MR32(c->esp+8), MR32(c->esp+12));
        guest_backtrace(c, 10);
    }
}

void dispatch_indirect(CPU *c, uint32_t addr){
    /* COM vtable methods (0xE...) and Win32 import bridges (0xF...) are far
     * above the guest code, so they never collide with the tables. */
    if (addr >= 0xE0000000u){
#ifdef __wasm__
        /* engine stack high-water at COM/Win32 calls (runtime/web/stackprobe.c) */
        { extern int g_stackprobe; if (g_stackprobe > 0){ void stackprobe_check(CPU *); stackprobe_check(c); } }
#endif
        if (addr < 0xF0000000u){ com_call(c, addr); return; }
        if (win32_call(c, addr - 0xF0000000u) < 0) g_guest_exited = 1;
        return;
    }
    guest_fn_t *s = slot(addr);
    guest_fn f = s ? *s : 0;
    if (!f && s && addr == g_icall_want){ f = patch_at(addr); if (!f) f = lifted_at(addr); }
    if (f){
        if (__builtin_expect(g_icall_want != 0, 0) && addr == g_icall_want) icall_note(c, addr);
        g_stat_indirect_ok++; f(c); return;
    }
    /* No lifted counterpart.  The game may have generated code at this
     * address at run time.  Try the small x86 interpreter; on an unknown
     * instruction it returns safely (restores esp) and falls through to the
     * diagnosis below. */
    { static int gx = -1;
      if (gx < 0){ const char *e = getenv("GENEXEC"); gx = (e && *e=='0') ? 0 : 1; }
      if (gx && mem_is_committed(addr)){
          uint32_t ret0 = MR32(c->esp);
          if (run_generated(c, addr)){
              static int said;
              if (said++ < 16) fprintf(stderr, "[genexec] ran generated code at 0x%08x (called from 0x%08x)\n", addr, ret0);
              g_stat_indirect_ok++; return;
          }
      } }

    g_stat_indirect_miss++;
    /* the return address is already on the stack (before POP32) */
    { uint32_t ret = MR32(c->esp);
      if (g_miss_n < MISS_MAX){ g_miss_from[g_miss_n]=ret; g_miss[g_miss_n++]=addr; }
      static int shown = 0;
      if (shown < 5){ shown++;
          fprintf(stderr, "\n  [UNRESOLVED INDIRECT CALL] target=0x%08x  return=0x%08x\n"
                  "      eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x\n"
                  "      arg1=%08x arg2=%08x arg3=%08x\n",
                  addr, ret, c->eax, c->ecx, c->edx, c->ebx, c->esi, c->edi,
                  MR32(c->esp+4), MR32(c->esp+8), MR32(c->esp+12));
          /* the bytes at the target: generated code, or a bad pointer? */
          if (mem_is_committed(addr+15)){
              fprintf(stderr, "      96 bytes at the target:\n       ");
              for (int q = 0; q < 96; q++){
                  if (q && !(q & 15)) fprintf(stderr, "\n       ");
                  fprintf(stderr, " %02x", MR8(addr+q));
              }
              fprintf(stderr, "\n");
          } else fprintf(stderr, "      target NOT COMMITTED (not data, a garbage pointer)\n");
          guest_backtrace(c, 12);
      } }
    POP32(c->eip);            /* drop the return address, swallow the call */
}

void unsupported_insn(CPU *c, uint32_t addr, const char *mn){
    g_stat_unsupported++;
    if (g_unsup_n < MISS_MAX){ g_unsup_mn[g_unsup_n] = mn; g_unsup[g_unsup_n++] = addr; }
    static int shown = 0;
    if (shown < 8){ shown++;
        const char *mod = 0; uint32_t f = module_nearest_fn(addr, &mod);
        fprintf(stderr, "[trap] unsupported instruction '%s' at 0x%08x (%s sub_%08x + 0x%x)\n",
                mn, addr, mod ? mod : "?", f, addr - f);
        guest_backtrace(c, 8);
    }
}

void cpuid_shim(CPU *c){
    /* generic Pentium III-like: MMX, SSE, no SSE2 yet (msvcr80 and ijl15 pick
     * their code paths from this; SSE2 once every SSE2 instruction they use
     * passes tools/isatest.py), no 3DNow */
    switch (c->eax){
    case 0: c->eax=1; c->ebx=0x756e6547; c->edx=0x49656e69; c->ecx=0x6c65746e; break;
    case 1: c->eax=0x000006d8; c->ebx=0; c->ecx=0; c->edx=0x0383fbff; break;
    default: c->eax=c->ebx=c->ecx=c->edx=0; break;
    }
}
/* rdtsc: tied to real time, a fixed 3.0 GHz (consistent with QPC).
 * WC3_TSC_MHZ changes the rate. */
uint64_t rdtsc_shim(void){
    static uint64_t mhz = 0;
    if (!mhz){ const char *e = getenv("WC3_TSC_MHZ"); mhz = e && *e ? strtoull(e, 0, 0) : 3000u; }
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return ns * mhz / 1000u;
}

void dispatch_report(void){
    fprintf(stderr,"\n=== runtime ===\n");
    fprintf(stderr,"  indirect calls resolved  : %llu\n", (unsigned long long)g_stat_indirect_ok);
    fprintf(stderr,"  indirect calls unresolved: %llu\n", (unsigned long long)g_stat_indirect_miss);
    fprintf(stderr,"  unsupported instructions : %llu\n", (unsigned long long)g_stat_unsupported);
    genexec_report();
    if (g_miss_n){ fprintf(stderr,"  unresolved indirect calls (target <- calling instruction):\n");
        int shown=0;
        for(int i=0;i<g_miss_n && shown<12;i++){
            int dup=0; for(int j=0;j<i;j++) if(g_miss_from[j]==g_miss_from[i]) dup=1;
            if(dup) continue;
            fprintf(stderr,"    0x%08x  <-  0x%08x\n", g_miss[i], g_miss_from[i]); shown++; }
    }
    if (g_unsup_n){ fprintf(stderr,"  first traps              :");
        for(int i=0;i<g_unsup_n && i<10;i++) fprintf(stderr," 0x%08x (%s)", g_unsup[i], g_unsup_mn[i]); fprintf(stderr,"\n"); }
}

/* --- guest stack tracer: return addresses inside lifted code --- */
void guest_backtrace(CPU *c, int maxn){
    fprintf(stderr,"    guest stack trace (esp=0x%08x):\n", c->esp);
    int shown=0;
    for (uint32_t k=0; k<2048 && shown<maxn; k++){
        uint32_t a = c->esp + 4u*k;
        if (!mem_is_committed(a)) continue;      /* skip an unmapped page */
        uint32_t v = MR32(a);
        const char *mn = 0; uint32_t mf = module_nearest_fn(v, &mn);
        if (mf){
            fprintf(stderr,"      [%2d] +0x%04x  return 0x%08x   (%s sub_%08x + 0x%x)\n",
                    shown, k*4, v, mn, mf, v-mf);
            shown++;
        }
    }
}

/* the guest function from a faulting host PC (the nearest lifted function
 * start below it) */
uint32_t guest_fn_from_host(const void *pc){ return module_fn_from_host(pc); }
