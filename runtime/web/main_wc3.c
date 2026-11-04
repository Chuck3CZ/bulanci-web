/* The entry point of the WC3 module (wasm32-wasip1-threads).
 *
 * Order as the Windows loader: memory, war3.exe and every lifted DLL it
 * imports (module.c: mapped, relocated, bound), TEB/PEB, file roots,
 * dispatch, win32; then the DLLs' DllMain and the exe's entry point on the
 * guest main thread.
 *
 *   wc3.wasm [<path of war3.exe>]       (default /data/war3.exe)
 *
 * In the browser the page (web/src/host) runs this _start on a game worker
 * and gives every guest thread a worker of its own; /data is the game folder
 * (read-only), /save the save overlay.  WC3_NOTHREAD=1 runs the guest on
 * this thread instead (node:wasi without thread-spawn: the start-up tests,
 * web/tools/wasi-run.mjs), until the game starts a thread of its own.
 * WC3_CMDLINE overrides the command line ("C:\Warcraft III\war3.exe" -window).
 * Based on nfsu2-web's main_web.c. */
#include "cpu.h"
#include "module.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <setjmp.h>
#include <pthread.h>

extern jmp_buf g_exit_jmp; extern int g_exit_code, g_exit_armed; extern int g_bt_on;
void dispatch_init(void); void dispatch_report(void);
void win32_init(void); void win32_report(void);
void com_report(void); void com_set_trace(int);
void fileio_set_root(const char*);
void fileio_set_write_root(const char*);
void fileio_set_readonly_root(int);
void lifecycle_guest_exited(int code); int lifecycle_drain_writes(double max_s);
int win32_freeze_guest(int max_ms);
void guest_lock(CPU*); void guest_unlock(void);
void watch_arm(uint32_t); void watch_dump(const char*,int);
void win32_set_trace_max(uint32_t);
extern uint64_t g_hook_hits, g_gs_fail;
extern void (*g_report_fn)(void);

/* Guest layout (docs/PLAN.md "Rozložení paměti"): TEBs from 0x00C00000, the
 * main stack below the thread stacks (0x06000000, win32_ext.c) and the heap
 * (0x08000000, heap.c). */
#define TEB_BASE   0x00C00000u
#define PEB_BASE   0x00C10000u
#define STACK_LO   0x05000000u
#define STACK_HI   0x05FF0000u

static CPU cpu; static CPU *const c = &cpu;

/* DUMP=<addr>:<len>[,...] (debugging): guest memory as hex lines at the end
 * of the run; "*<addr>:<len>" dumps from the pointer stored at addr. */
static void mem_dumps(void){
    const char *e = getenv("DUMP");
    if (!e || !*e) return;
    char buf[512]; snprintf(buf, sizeof buf, "%s", e);
    for (char *save = 0, *t = strtok_r(buf, ",", &save); t; t = strtok_r(0, ",", &save)){
        int ind = *t == '*'; if (ind) t++;
        uint32_t a = (uint32_t)strtoul(t, 0, 0), n = 0;
        char *colon = strchr(t, ':'); if (colon) n = (uint32_t)strtoul(colon + 1, 0, 0);
        if (ind) a = mem_is_committed(a) ? MR32(a) : 0;
        fprintf(stderr, "[dump] 0x%08x %u bytes\n", a, n);
        for (uint32_t o = 0; o < n; o += 32){
            if (!mem_is_committed(a + o)){ fprintf(stderr, "[dump] %08x: not committed\n", a + o); break; }
            fprintf(stderr, "[dump] %08x:", a + o);
            for (uint32_t k = 0; k < 32 && o + k < n; k += 4) fprintf(stderr, " %08x", MR32(a + o + k));
            fprintf(stderr, "\n");
        }
    }
}

static void all_reports(void){
    static int done; if (done) return; done = 1;
    mem_dumps();
    if (getenv("WATCH_ADDR")) watch_dump("end of run / crash", 24);
    com_report(); win32_report();
    { void gcpu_report(void); gcpu_report(); }
    { void stackprobe_report(void); stackprobe_report(); }
    dispatch_report();
}

/* WC3_SELFTEST=crypt: Storm's MPQ crypt table (Storm.dll 0x517650, seed in
 * eax, table pointer cached at [0x54c2e4]) built by the lifted code into a
 * buffer of ours, against the MPQ reference; the run then exits. */
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
static int selftest_crypt(CPU *cp){
    const uint32_t buf = 0x00C80000u;               /* TEB area, unused this early */
    mem_commit(buf, 0x2000); memset(g_mem + buf, 0, 0x1400);
    MR32(0x54c2e4u) = buf;
    cp->eax = 0x100001u;
    call_guest(cp, 0x00517650u, 0, 0);
    uint32_t seed = 0x00100001u, bad = 0, first_bad = 0xFFFFFFFFu;
    for (uint32_t i1 = 0; i1 < 0x100; i1++)
        for (uint32_t i2 = i1, k = 0; k < 5; k++, i2 += 0x100){
            seed = (seed * 125u + 3u) % 0x2AAAABu; uint32_t hi = (seed & 0xFFFFu) << 16;
            seed = (seed * 125u + 3u) % 0x2AAAABu; uint32_t v = hi | (seed & 0xFFFFu);
            if (MR32(buf + 4u*i2) != v){ if (!bad++) first_bad = i2; }
        }
    fprintf(stderr, "[selftest] crypt table: %u of 1280 words wrong%s", bad, bad ? "" : "\n");
    if (bad) fprintf(stderr, " (first [0x%x] = %08x, want ...); [0] = %08x (want 55c636e2)\n",
                     first_bad, MR32(buf + 4u*first_bad), MR32(buf));
    MR32(0x54c2e4u) = 0;
    return bad ? 1 : 0;
}

static void *guest_main_thread(void *unused){
    (void)unused;
    if (setjmp(g_exit_jmp) == 0){
        g_exit_armed = 1;
        guest_lock(&cpu);
        { const char *st = getenv("WC3_SELFTEST");
          if (st && !strcmp(st, "crypt")){ g_exit_code = selftest_crypt(&cpu); guest_unlock(); lifecycle_guest_exited(g_exit_code); return 0; } }
        if (module_init_static(&cpu) == 0){
            uint32_t ep = module_exe_entry();
            fprintf(stderr, "\n=== running war3.exe's entry point: 0x%08x ===\n\n", ep);
            PUSH32(0xDEADC0DEu);
            cpu.eip = ep;
            dispatch_indirect(&cpu, ep);
            fprintf(stderr, "\n*** guest entry point returned (eip=0x%08x, eax=0x%08x)\n", cpu.eip, cpu.eax);
        } else g_exit_code = 1;
        guest_unlock();
    } else {
        fprintf(stderr, "\n*** guest exited with code %d\n", g_exit_code);
        guest_unlock();          /* ExitProcess longjmp'd out of a call holding it */
    }
    lifecycle_guest_exited(g_exit_code);
    return 0;
}

int main(int argc, char **argv){
    const char *exe = argc > 1 ? argv[1] : "/data/CMR5.EXE";

    mem_init();
    fileio_set_root(exe);
    { const char *sd = getenv("WC3_SAVEDIR"); if (sd && *sd) fileio_set_write_root(sd); }
    { const char *ro = getenv("WC3_READONLY_DATA");
      if (ro && *ro && *ro != '0'){ fileio_set_readonly_root(1);
          fprintf(stderr, "file: game folder is read-only (WC3_READONLY_DATA)\n"); } }
    if (module_load_exe(exe)) return 1;
    mem_commit(TEB_BASE, 0x20000);
    mem_commit(STACK_LO, STACK_HI - STACK_LO);
    dispatch_init(); win32_init();
    { extern void status_web_start(void); status_web_start(); }  /* counters for the page overlay */
    { extern void input_web_init(void); input_web_init(); }     /* the page's keys, mouse, focus */
    if (getenv("BT")) g_bt_on = 1;
    { int stackprobe_on(void); stackprobe_on(); }   /* WC3_STACKPROBE (runtime/web/stackprobe.c) */
    g_report_fn = all_reports;
    com_set_trace(getenv("COMTRACE") ? 1 : 0);
    if (getenv("TRACE_MAX")) win32_set_trace_max((uint32_t)atoi(getenv("TRACE_MAX")));
    else if (getenv("VERBOSE")) win32_set_trace_max(400u);
    { const char *w = getenv("WATCH_ADDR"); if (w) watch_arm((uint32_t)strtoul(w, 0, 0)); }

    /* TEB (fs:) and PEB of the main thread */
    MR32(TEB_BASE+0x00) = 0xFFFFFFFFu;      /* end of the SEH chain */
    MR32(TEB_BASE+0x04) = STACK_HI;         /* StackBase  */
    MR32(TEB_BASE+0x08) = STACK_LO;         /* StackLimit */
    MR32(TEB_BASE+0x18) = TEB_BASE;         /* Self       */
    MR32(TEB_BASE+0x20) = 100;              /* ProcessId  */
    MR32(TEB_BASE+0x24) = 1;                /* ThreadId   */
    MR32(TEB_BASE+0x2C) = TEB_BASE+0x1000;  /* TLS array  */
    MR32(TEB_BASE+0x30) = PEB_BASE;         /* PEB        */
    mem_commit(PEB_BASE, 0x1000);
    MR32(PEB_BASE+0x08) = 0x00400000u;      /* ImageBaseAddress */
    MR32(PEB_BASE+0xA4) = 5; MR32(PEB_BASE+0xA8) = 1;   /* OS 5.1 (XP) */

    { extern CPU *g_cur_cpu; g_cur_cpu = &cpu; }
    memset(&cpu, 0, sizeof cpu);
    cpu.esp = STACK_HI - 0x1000u;
    cpu.top = 0; cpu.fcw = 0x027f;   /* Windows default: PC=53, RC near, masked */
    cpu.fs_base = TEB_BASE;
    cpu.gs_base = TEB_BASE;
    fprintf(stderr, "    TEB=0x%08x  stack=0x%08x..0x%08x  esp=0x%08x\n", TEB_BASE, STACK_LO, STACK_HI, cpu.esp);

    const char *nt = getenv("WC3_NOTHREAD");
    if (nt && *nt && *nt != '0') guest_main_thread(0);
    else {
        pthread_attr_t at; pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 16u * 1024u * 1024u);   /* shadow stack only; see win32_ext.c */
        pthread_t th;
        if (pthread_create(&th, &at, guest_main_thread, 0) != 0){
            fprintf(stderr, "could not start the guest main thread\n"); return 1; }
        pthread_join(th, 0);
    }
    lifecycle_drain_writes(5.0);
    if (!win32_freeze_guest(2000)) fprintf(stderr, "app: a guest thread still holds the guest lock\n");
    fprintf(stderr, "\n  hook hits: %llu   /GS failures: %llu\n",
            (unsigned long long)g_hook_hits, (unsigned long long)g_gs_fail);
    if (getenv("WATCH_ADDR")) watch_dump("end of run", 20);
    all_reports();
    return g_exit_code;
}
