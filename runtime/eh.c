/* C++ exceptions (MSVC, FuncInfo 0x19930520..22) for the lifted code.
 *
 * _CxxThrowException ends in RaiseException(0xE06D7363); win32.c hands it to
 * eh_cxx_throw, which does what the CRT's __CxxFrameHandler would: walk the
 * SEH chain at fs:[0], find the first frame whose try block (by the frame's
 * state) has a catch for the thrown type, run the unwind actions of the frames
 * in between and of the target frame down to the try block, copy the
 * exception object into the catch's variable, call the catch funclet (it
 * returns the address to continue at) and resume the function there.
 *
 * Resuming: the lifter (tools/lift.py, eh_conts) puts EH_SEH(c) after each
 * write of fs:[0] in a function that has catch blocks.  When the write
 * installs a frame, eh_fs_written records it with a jmp_buf the function
 * setjmp()s into; eh_cxx_throw longjmps there with the guest registers set,
 * and the function jumps to the continuation's label (eh_resume_take).
 * Frames that only have unwind actions (destructors) need no jmp_buf.
 *
 * A raise nothing catches returns to win32.c, which reports it and stops. */
#include "cpu.h"
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void dispatch_indirect(CPU *c, uint32_t target);

#define EH_MAX 256
typedef struct { uint32_t rec; jmp_buf jb; } EhEnt;
static __thread EhEnt *g_eh;
static __thread int g_eh_n, g_eh_resume;

static int ehlog(void){ static int on = -1; if (on < 0){ on = getenv("EHLOG") != 0; if (on) setvbuf(stderr, 0, _IONBF, 0); } return on; }

/* after a write of fs:[0]: install (the lifter saw the record's address
 * computed by lea) records the frame and returns the jmp_buf to setjmp; a
 * restore drops the frames inside the new head (the stack grows down) */
jmp_buf *eh_fs_written(CPU *c, int install){
    { static int off = -1; if (off < 0) off = getenv("EHOFF") != 0; if (off) return 0; }
    if (!g_eh && !(g_eh = calloc(EH_MAX, sizeof *g_eh))) return 0;
    uint32_t rec = MR32(c->fs_base);
    if (!install){ while (g_eh_n && g_eh[g_eh_n - 1].rec < rec) g_eh_n--; return 0; }
    while (g_eh_n && g_eh[g_eh_n - 1].rec <= rec) g_eh_n--;     /* returned or stale frames */
    if (g_eh_n == EH_MAX){ memmove(g_eh, g_eh + 1, sizeof g_eh[0] * (EH_MAX - 1)); g_eh_n--; }
    g_eh[g_eh_n].rec = rec;
    return &g_eh[g_eh_n++].jb;
}
int eh_resume_take(void){ int r = g_eh_resume; g_eh_resume = 0; return r; }

static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n - 1); }

/* the frame handler thunk: mov eax, <FuncInfo>; jmp __CxxFrameHandler
 * (VS2005+ puts mov edx,[esp+8] / lea / xor / call __security_check_cookie
 * before it) */
static uint32_t funcinfo_of(uint32_t h){
    if (!ok(h, 64)) return 0;
    for (uint32_t k = 0; k + 10 <= 64; k++)
        if (MR8(h + k) == 0xB8u && MR8(h + k + 5) == 0xE9u){
            uint32_t fi = MR32(h + k + 1);
            if (ok(fi, 20) && (MR32(fi) & ~3u) == 0x19930520u) return fi;
        }
    return 0;
}

/* run a funclet the way _CallSettingFrame does: on the current stack, with
 * ebp the frame's; returns eax */
static uint32_t call_funclet(CPU *c, uint32_t fn, uint32_t ebp, int keep){
    CPU save = *c;
    c->ebp = ebp; c->esp -= 64;
    PUSH32(0xDEADBEEFu);
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    if (!keep) *c = save;
    return r;
}

/* the frame's unwind actions from its state down to `to` */
static void unwind_to(CPU *c, uint32_t rec, uint32_t fi, int to){
    uint32_t umap = MR32(fi + 8); int maxs = (int)MR32(fi + 4);
    int st = (int)MR32(rec + 8);
    while (st != to && st >= 0 && st < maxs){
        uint32_t e = umap + 8u * (uint32_t)st;
        int next = (int)MR32(e); uint32_t act = MR32(e + 4);
        MR32(rec + 8) = (uint32_t)next;
        if (act){
            if (ehlog()) fprintf(stderr, "[eh]   unwind state %d -> %d: 0x%08x\n", st, next, act);
            call_funclet(c, act, rec + 12u, 0);
        }
        st = next;
    }
}

static const char *type_name(uint32_t td){ return ok(td, 12) ? (const char *)(g_mem + td + 8) : ""; }

/* does the catch's HandlerType take this catchable type? */
static int catches(uint32_t ht, uint32_t ct){
    uint32_t want = MR32(ht + 4), have = MR32(ct + 4);
    if (!want) return 1;                                          /* catch (...) */
    return want == have || !strcmp(type_name(want), type_name(have));
}

void eh_cxx_throw(CPU *c){
    if (!g_eh) return;
    uint32_t pa = MR32(c->esp + 12), na = MR32(c->esp + 8);      /* RaiseException(code, flags, n, args); the return address is popped */
    if (na < 3 || !ok(pa, 12)) return;
    uint32_t obj = MR32(pa + 4), ti = MR32(pa + 8);
    if (!ti || !ok(ti, 16)) return;                               /* rethrow: not handled */
    uint32_t cta = MR32(ti + 12); if (!ok(cta, 4)) return;
    uint32_t nct = MR32(cta);
    if (ehlog()){ void guest_backtrace(CPU *, int);
        fprintf(stderr, "[eh] throw %s\n", nct && ok(MR32(cta + 4), 8) ? type_name(MR32(MR32(cta + 4) + 4)) : "?");
        guest_backtrace(c, 6); }
    for (uint32_t rec = MR32(c->fs_base); rec != 0xFFFFFFFFu && ok(rec, 12); rec = MR32(rec)){
        uint32_t fi = funcinfo_of(MR32(rec + 4));
        if (!fi) continue;                                        /* not a C++ frame (__try, CRT) */
        int st = (int)MR32(rec + 8), ntry = (int)MR32(fi + 12); uint32_t tmap = MR32(fi + 16);
        for (int t = 0; t < ntry; t++){
            uint32_t tb = tmap + 20u * (uint32_t)t;
            int lo = (int)MR32(tb), hi = (int)MR32(tb + 4), nc = (int)MR32(tb + 12);
            uint32_t harr = MR32(tb + 16);
            if (st < lo || st > hi) continue;
            for (int h = 0; h < nc; h++){
                uint32_t ht = harr + 16u * (uint32_t)h;
                for (uint32_t k = 0; k < nct; k++){
                    uint32_t ct = MR32(cta + 4 + 4 * k);
                    if (!ok(ct, 28) || !catches(ht, ct)) continue;
                    /* the jmp_buf of the function this frame belongs to */
                    int e = g_eh_n - 1;
                    while (e >= 0 && g_eh[e].rec != rec) e--;
                    if (e < 0){
                        fprintf(stderr, "[eh] catch at frame 0x%08x but the function has no resume point\n", rec);
                        return;
                    }
                    uint32_t ebp = rec + 12u;
                    if (ehlog()) fprintf(stderr, "[eh] %s caught by frame 0x%08x (state %d, try %d..%d), catch 0x%08x\n",
                                         type_name(MR32(ct + 4)), rec, st, lo, hi, MR32(ht + 12));
                    /* unwind the inner frames, then this one down to the try block */
                    for (uint32_t r2 = MR32(c->fs_base); r2 != rec && r2 != 0xFFFFFFFFu; r2 = MR32(r2)){
                        uint32_t f2 = funcinfo_of(MR32(r2 + 4));
                        if (f2) unwind_to(c, r2, f2, -1);
                    }
                    MR32(c->fs_base) = rec;
                    unwind_to(c, rec, fi, lo - 1);
                    /* the catch's variable */
                    uint32_t adj = MR32(ht), disp = MR32(ht + 8), props = MR32(ct);
                    int32_t mdisp = (int32_t)MR32(ct + 8);
                    uint32_t size = MR32(ct + 20), copy = MR32(ct + 24);
                    if (disp && MR32(ht + 4)){
                        uint32_t dst = ebp + disp;
                        if (adj & 8u) MR32(dst) = obj + (uint32_t)mdisp;                 /* by reference */
                        else if (props & 1u){                                            /* simple type / pointer */
                            memcpy(g_mem + dst, g_mem + obj, size);
                            if (size == 4 && MR32(dst)) MR32(dst) += (uint32_t)mdisp;
                        } else if (copy){
                            CPU save = *c; c->ecx = dst; c->esp -= 64; PUSH32(obj + (uint32_t)mdisp);
                            PUSH32(0xDEADBEEFu); dispatch_indirect(c, copy); *c = save;
                        } else memcpy(g_mem + dst, g_mem + obj + mdisp, size);
                    }
                    uint32_t cont = call_funclet(c, MR32(ht + 12), ebp, 1);
                    /* the exception object's destructor (a thrown pointer has none) */
                    uint32_t dtor = MR32(ti + 4);
                    if (dtor){ CPU save = *c; c->ecx = obj; c->esp -= 64; PUSH32(0xDEADBEEFu);
                               dispatch_indirect(c, dtor); *c = save; }
                    if (ehlog()) fprintf(stderr, "[eh]   continue at 0x%08x\n", cont);
                    c->ebp = ebp; c->esp = MR32(rec - 4); c->eip = cont;
                    MR32(c->fs_base) = rec;
                    g_eh_n = e + 1; g_eh_resume = 1;
                    longjmp(g_eh[e].jb, 1);
                }
            }
        }
    }
}
