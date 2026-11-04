/* threadcpu: per-thread CPU time and counters of a process (macOS), for web/tools/bench.mjs.
 *   threadcpu <pid>...
 * One line per thread:
 *   pid thread_id cpu_ns name  P_instructions P_cycles P_ns  E_instructions E_cycles E_ns
 * (P = performance cores, E = efficiency cores).  The browser has no thread CPU clock, so the
 * benchmark's busy time per frame is wall time: on a loaded machine it includes preemption and
 * time on the slower E cores.  Instructions retired per frame do not depend on the load, and the
 * per-core-type split shows how much of the game thread ran on E cores.  The counters come from
 * proc_pidinfo(PROC_PIDTHREADCOUNTS), a private flavour of the public call (the one Activity
 * Monitor and the Mac port's thread_selfcounts use), with its XNU layout below.  Build:
 *   clang -O2 -o <dir>/threadcpu web/tools/threadcpu.c */
#include <libproc.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc_info.h>

#define PIDLISTTHREADIDS   28
#define PIDTHREADID64INFO  15
#define PIDTHREADCOUNTS    34
struct tcd { uint64_t instructions, cycles, user_mach, system_mach, energy_nj; };
struct tc  { uint16_t len, r0; uint32_t r1; struct tcd c[4]; };

int main(int argc, char **argv){
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    for (int a = 1; a < argc; a++){
        int pid = atoi(argv[a]);
        static uint64_t ids[8192];
        int n = proc_pidinfo(pid, PIDLISTTHREADIDS, 0, ids, sizeof ids);
        if (n <= 0) continue;
        n /= (int)sizeof ids[0];
        for (int i = 0; i < n; i++){
            struct proc_threadinfo ti;
            if (proc_pidinfo(pid, PIDTHREADID64INFO, ids[i], &ti, sizeof ti) != (int)sizeof ti) continue;
            for (char *p = ti.pth_name; *p; p++) if (*p == ' ') *p = '_';
            struct tc t; memset(&t, 0, sizeof t);
            int r = proc_pidinfo(pid, PIDTHREADCOUNTS, ids[i], &t, sizeof t);
            printf("%d %llu %llu %s", pid, (unsigned long long)ids[i],
                   (unsigned long long)(ti.pth_user_time + ti.pth_system_time), ti.pth_name[0] ? ti.pth_name : "-");
            for (int k = 0; k < 2; k++){
                const struct tcd *d = r > 8 && k < t.len ? &t.c[k] : NULL;
                unsigned long long ns = d ? (d->user_mach + d->system_mach) * tb.numer / tb.denom : 0;
                printf(" %llu %llu %llu", d ? (unsigned long long)d->instructions : 0ull, d ? (unsigned long long)d->cycles : 0ull, ns);
            }
            printf("\n");
        }
    }
    return 0;
}
