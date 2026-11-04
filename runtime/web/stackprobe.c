/* wasm: stack high-water probe (NFSU2_STACKPROBE=1).
 *
 * Every lifted guest call is a wasm call, so a deep guest call chain uses the
 * engine's native stack, which the page cannot size (about 1 MB in a Chrome
 * worker) and whose overflow is a RangeError that kills the thread.  This
 * records how deep the game goes, per guest thread: at a Win32/COM call
 * (dispatch.c) whose guest stack is deeper than any before on that thread
 * (in 4 KB steps), and otherwise at most every 2 s,
 *   - the guest stack in use: from where the thread started (every guest
 *     thread starts 4 KB below its StackBase: win32_ext.c, main_web.c) down
 *     to esp, and
 *   - the engine call depth: frames on the thread's stack, wasm and JS, from
 *     a stack trace (web/src/host/game-worker.ts, nfsu2.stack_frames).
 * Both are samples at Win32/COM calls only, so the true maxima can be higher
 * (guest code that recurses without calling out is never seen): read them as
 * lower bounds.  Byte usage of the engine stack cannot be read from the page:
 * a recursion probe is not stable in Chrome (4536 to 7352 frames of the same
 * interpreted function in the same place), so frames are the measure, and how
 * far that is from a RangeError is not measured here.  New maxima are logged
 * as [stack] lines; the exit report lists each thread's maxima, with the
 * return address of the call that reached them. */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include "web/host_abi.h"

__attribute__((import_module("nfsu2"), import_name("stack_frames"))) int nfsu2_stack_frames(void);

int g_stackprobe = -1;                  /* -1: not read yet */
#define START_GAP 0x1000u             /* a thread's first esp is StackBase - 4 KB */
typedef struct { uint32_t tid, guest_b, guest_at, frames, frames_at, logged, probes; } Probe;
static Probe g_pr[64]; static int g_npr;  /* each entry written by its own thread, under the guest lock */
static _Thread_local Probe *tl_pr;
static _Thread_local uint32_t tl_low = ~0u;
static _Thread_local double tl_next_ms;

int stackprobe_on(void){
    if (g_stackprobe < 0){ const char *e = getenv("NFSU2_STACKPROBE"); g_stackprobe = e && *e && *e != '0'; }
    return g_stackprobe;
}

void stackprobe_check(CPU *c){
    double now = nfsu2_now_ms();
    int deeper = c->esp + 0x1000u < tl_low;
    if (!deeper && now < tl_next_ms) return;
    tl_next_ms = now + 2000.0;
    if (deeper) tl_low = c->esp;
    if (!tl_pr){
        uint32_t tid = c->fs_base ? MR32(c->fs_base + 0x24u) : 0u;
        for (int i = 0; i < g_npr; i++) if (g_pr[i].tid == tid){ tl_pr = &g_pr[i]; break; }
        if (!tl_pr && g_npr < 64){ tl_pr = &g_pr[g_npr++]; tl_pr->tid = tid; }
        if (!tl_pr) return;
    }
    Probe *p = tl_pr;
    uint32_t top = c->fs_base ? MR32(c->fs_base + 0x04u) : 0u;
    uint32_t start = top > START_GAP ? top - START_GAP : 0u;
    uint32_t gb = start > c->esp ? start - c->esp : 0u;
    /* the guest's call pushed its return address: the call site */
    uint32_t at = mem_is_committed(c->esp) ? MR32(c->esp) : 0u;
    int fr = nfsu2_stack_frames();
    p->probes++;
    if (gb > p->guest_b){ p->guest_b = gb; p->guest_at = at; }
    if (fr > 0 && (uint32_t)fr > p->frames){ p->frames = (uint32_t)fr; p->frames_at = at; }
    if (p->frames >= p->logged + p->logged / 4 + 8){     /* log when the maximum grew by a quarter */
        fprintf(stderr, "[stack] tid %u: engine call depth %u frames, guest stack %.1f KB in use (call returning to 0x%08x)\n",
                p->tid, p->frames, gb / 1024.0, at);
        p->logged = p->frames;
    }
}

void stackprobe_report(void){
    if (!stackprobe_on() || !g_npr) return;
    fprintf(stderr, "\n  stack high-water (NFSU2_STACKPROBE), sampled at Win32/COM calls (lower bounds): engine call depth"
                    " (wasm + JS frames) and guest stack in use, each with the call's return address\n");
    for (int i = 0; i < g_npr; i++)
        fprintf(stderr, "    tid %-4u engine >= %5u frames (0x%08x)  guest >= %6.1f KB (0x%08x)  %u probes\n", g_pr[i].tid,
                g_pr[i].frames, g_pr[i].frames_at, g_pr[i].guest_b / 1024.0, g_pr[i].guest_at, g_pr[i].probes);
}
