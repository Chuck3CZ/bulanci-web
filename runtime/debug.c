/* Debugging: guest watchpoint + instruction hooks */
#include "cpu.h"
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


/* resolve the nearest function with the function address table */
#include "module.h"
static uint32_t near_fn(uint32_t a){ return module_nearest_fn(a, 0); }

void itrace_dump(int n){
    if (!g_itrace_n){ fprintf(stderr,"  (instruction trace empty - not recorded without WATCH=1)\n"); return; }
    uint64_t start = g_itrace_n > (uint64_t)n ? g_itrace_n - n : 0;
    fprintf(stderr,"\n=== LAST %llu GUEST INSTRUCTIONS (executed in total: %llu) ===\n",
            (unsigned long long)(g_itrace_n-start), (unsigned long long)g_itrace_n);
    uint32_t last_fn = 0;
    for (uint64_t k=start; k<g_itrace_n; k++){
        uint32_t ip = g_itrace[(uint32_t)k & ITRACE_MASK];
        uint32_t f  = near_fn(ip);
        if (f != last_fn){ fprintf(stderr,"  --- sub_%08x ---\n", f); last_fn = f; }
        fprintf(stderr,"    %llu  0x%08x  (+0x%x)\n",
                (unsigned long long)k, ip, ip - f);
    }
}
void guest_backtrace(CPU*,int);

#define WRING 128
typedef struct { uint32_t ip, oldv, newv; uint64_t seq; } WEnt;
static WEnt g_ring[WRING];
static uint64_t g_wcount = 0;

extern void (*g_itrace_dump_fn)(int);
void watch_report_change(uint32_t culprit_ip, uint32_t oldv, uint32_t newv){
    g_ring[g_wcount % WRING] = (WEnt){culprit_ip, oldv, newv, g_wcount};
    g_wcount++;
    /* On the first change dump the instruction trace: it shows exactly
     * which call chain led to the write. */
    if (g_wcount == 1 && getenv("WATCH_ITRACE")){
        fprintf(stderr,"\n*** watchpoint first change: 0x%08x  %08x -> %08x\n",
                culprit_ip, oldv, newv);
        itrace_dump(getenv("WATCH_ITRACE_N") ? atoi(getenv("WATCH_ITRACE_N")) : 120);
    }
}

static void __attribute__((constructor)) _dbg_init(void){ g_itrace_dump_fn = itrace_dump; }
void mem_commit(uint32_t,uint32_t);
void watch_arm(uint32_t addr){
    if (addr) mem_commit(addr & ~0x3FFFu, 0x4000u);   /* make sure the watched page exists */
    g_watch_addr = addr;
    g_watch_val  = addr ? MR32(addr) : 0;
    g_wcount = 0;
    fprintf(stderr,"watchpoint: guest 0x%08x armed (initial value 0x%08x)\n", addr, g_watch_val);
}

void watch_dump(const char *why, int maxn){
    fprintf(stderr,"\n=== WATCHPOINT DUMP (%s) ===\n", why);
    fprintf(stderr,"  watched address: 0x%08x   changes in total: %llu\n",
            g_watch_addr, (unsigned long long)g_wcount);
    uint64_t start = g_wcount > (uint64_t)maxn ? g_wcount - maxn : 0;
    for (uint64_t k = start; k < g_wcount; k++){
        WEnt *e = &g_ring[k % WRING];
        fprintf(stderr,"  #%-6llu insn 0x%08x   0x%08x -> 0x%08x\n",
                (unsigned long long)e->seq, e->ip, e->oldv, e->newv);
    }
}

/* --- instruction hooks --- */
uint64_t g_hook_hits = 0, g_gs_fail = 0;
static int g_gs_reported = 0;

static const struct { uint32_t ip; const char *what; } g_hook_names[] = {
    {0x0076115eu,"__security_check_cookie entry"},
    {0x00761167u,"jne -> __report_gsfailure"},
    {0x0076112du,"__report_gsfailure body"},
    {0x00761141u,"call reporter(1,0)"},
    {0x0076c25cu,"reporter function entry"},
    {0x0076c2b9u,"took the 'Buffer overrun' branch"},
    {0x0076c2a0u,"reporter: error type switch"},
};
/* Instruction hooks (HOOKS=<addr,...> at lift time: tools/genall.py puts a
 * guest_hook call before each): the registers, and with
 * WC3_HOOKDUMP=<reg>+<off>[,...] (reg: eax..edi, esp) the dwords there and the
 * one each points to, for the first WC3_HOOKMAX (default 20) hits. */
void guest_hook(CPU *c, uint32_t ip){
    static int max = -1; static uint64_t n = 0;
    g_hook_hits++;
    /* WC3_HOOKAT=<hex>: print only that hook (the others still count) */
    static long at = -2; if (at == -2){ const char *e = getenv("WC3_HOOKAT"); at = e && *e ? (long)strtoul(e, 0, 16) : -1; }
    if (at >= 0 && ip != (uint32_t)at) return;
    /* WC3_HOOKBAD=<reg>: print only when that register is not a committed address */
    static int bad = -2; if (bad == -2){ const char *e = getenv("WC3_HOOKBAD"); static const char *const rn[8] = { "eax","ecx","edx","ebx","esp","ebp","esi","edi" };
        bad = -1; for (int k = 0; e && k < 8; k++) if (!strncmp(e, rn[k], 3)) bad = k; }
    if (bad >= 0 && c->r[bad] && mem_is_committed(c->r[bad])) return;
    if (bad >= 0){ fprintf(stderr, "[hook] bad %s at %08x, caller %08x:", "reg", ip, mem_is_committed(c->esp+12) ? MR32(c->esp+12) : 0); }
    if (max < 0){ const char *e = getenv("WC3_HOOKMAX"); max = e && *e ? atoi(e) : 20; }
    if (n++ >= (uint64_t)max) return;
    fprintf(stderr, "[hook] %08x  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
            ip, c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    const char *d = getenv("WC3_HOOKDUMP");
    if (!d || !*d) return;
    char buf[256]; snprintf(buf, sizeof buf, "%s", d);
    for (char *save = 0, *t = strtok_r(buf, ",", &save); t; t = strtok_r(0, ",", &save)){
        static const char *const rn[8] = { "eax","ecx","edx","ebx","esp","ebp","esi","edi" };
        int r = -1; for (int k = 0; k < 8; k++) if (!strncmp(t, rn[k], 3)) r = k;
        if (r < 0) continue;
        uint32_t a = c->r[r] + (uint32_t)strtol(t + 3, 0, 0);     /* "edi-4", "esp+20" */
        if (!mem_is_committed(a)){ fprintf(stderr, "[hook]   [%s] = %08x: not committed\n", t, a); continue; }
        uint32_t v = MR32(a);
        fprintf(stderr, "[hook]   [%s] @%08x = %08x", t, a, v);
        if (v && mem_is_committed(v)) fprintf(stderr, "  -> %08x %08x %08x %08x", MR32(v), MR32(v+4), MR32(v+8), MR32(v+12));
        fprintf(stderr, "\n");
    }
}


/* ---- function coverage buffer ------------------------------------------ */
#define COV_SIZE 3676913u
uint32_t *g_cov = 0;
uint32_t g_last_fn = 0;
int      g_prof_on = 0;
static void __attribute__((constructor)) _cov_init(void){
    if (getenv("COVREPORT")){ g_cov = calloc(COV_SIZE, sizeof *g_cov); g_prof_on = 1; }
}
void cov_reset(void){
    if (g_cov) memset(g_cov, 0, (size_t)COV_SIZE * sizeof *g_cov);
}
/* Plain dump (no top-30 list, counts kept): autotest "covdump". */
void cov_dump(const char *path){
    if (!g_cov) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    unsigned long n = 0;
    for (uint32_t i = 0; i < COV_SIZE; i++)
        if (g_cov[i]){ fprintf(f, "%08x %u\n", 0x00401000u + i, g_cov[i]); n++; }
    fclose(f);
    fprintf(stderr, "  coverage: %lu functions -> %s\n", n, path);
}
void cov_report(const char *path){
    if (!g_cov) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    unsigned long n = 0;
    for (uint32_t i = 0; i < COV_SIZE; i++)
        if (g_cov[i]){ fprintf(f, "%08x %u\n", 0x00401000u + i, g_cov[i]); n++; }
    fclose(f);
    fprintf(stderr, "  coverage: %lu functions ran -> %s\n", n, path);
    /* MOST CALLED: a direct measurement of "what is the game doing?".
     * COVTOP=<n> lists n of them (30; tools/inline_pick.py reads ~1000). */
    { unsigned long long tot = 0; for (uint32_t i = 0; i < COV_SIZE; i++) tot += g_cov[i];
      fprintf(stderr, "  coverage: %llu calls in all\n", tot); }
    const char *ct = getenv("COVTOP");
    int top = ct && *ct ? atoi(ct) : 30;
    fprintf(stderr, "  --- MOST CALLED GUEST FUNCTIONS ---\n");
    for (int k = 0; k < top; k++){
        uint32_t bi = 0, best = 0;
        for (uint32_t i = 0; i < COV_SIZE; i++)
            if (g_cov[i] > best){ best = g_cov[i]; bi = i; }
        if (!best) break;
        fprintf(stderr, "    %10u  sub_%08x\n", best, 0x00401000u + bi);
        g_cov[bi] = 0;
    }
}
