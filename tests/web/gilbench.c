/* The guest lock's hand-off in the browser (web/tools/gilbench.sh): the FIFO
 * ticket lock of runtime/web/ticketlock.h against wasi-libc's pthread mutex
 * (the old guest lock), with the threads as workers of the real host.
 *  1. ping-pong: 2..8 threads take the lock in turn, each holding it ~0 us;
 *     every acquisition is a hand-off: cost per hand-off, and the share of
 *     acquisitions each thread got (fairness).
 *  2. starvation: a "game" thread re-takes the lock right after releasing it
 *     (busy 2 ms per hold, like an uncapped game loop), a "feeder" sleeps
 *     10 ms and then needs the lock for 50 us (the DirectSound feeder); its
 *     wait for the lock is measured. */
#include "web/ticketlock.h"
#include "web/host_abi.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static ticketlock g_tl = TICKETLOCK_INIT;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static int g_use_mx;
static void lk(void){ if (g_use_mx) pthread_mutex_lock(&g_mx); else ticket_lock(&g_tl); }
static void ul(void){ if (g_use_mx) pthread_mutex_unlock(&g_mx); else ticket_unlock(&g_tl); }
static double now(void){ return nfsu2_now_ms(); }
static void busy(double ms){ double t = now() + ms; while (now() < t) {} }

/* ---- 1. ping-pong ---- */
static volatile int g_stop; static long g_count[8]; static long g_total;
static void *pp(void *a){
    int i = (int)(long)a;
    while (!g_stop){ lk(); g_count[i]++; g_total++; ul(); }
    return 0;
}
static void pingpong(int nt){
    memset(g_count, 0, sizeof g_count); g_total = 0; g_stop = 0;
    pthread_t th[8];
    for (long i = 0; i < nt; i++) pthread_create(&th[i], 0, pp, (void *)i);
    double t0 = now(); usleep(1000000); g_stop = 1; double t1 = now();
    for (int i = 0; i < nt; i++) pthread_join(th[i], 0);
    long mn = g_count[0], mx = g_count[0], sum = 0;
    for (int i = 0; i < nt; i++){ if (g_count[i] < mn) mn = g_count[i]; if (g_count[i] > mx) mx = g_count[i]; sum += g_count[i]; }
    /* g_total is a plain counter bumped under the lock: equal to the sum
     * only if the lock really excluded */
    printf("  %-6s %d threads: %8ld acquisitions/s, %.2f us each; per thread min %ld max %ld (max/min %.1f)%s\n",
           g_use_mx ? "mutex" : "ticket", nt, (long)(g_total / ((t1 - t0) / 1000.0)), (t1 - t0) * 1000.0 / (double)g_total,
           mn, mx, mn ? (double)mx / (double)mn : 1e9, sum == g_total ? "" : "  EXCLUSION BROKEN");
}

/* ---- 2. starvation ---- */
static double g_w[4096]; static int g_nw;
static void *game(void *a){ (void)a; while (!g_stop){ lk(); busy(2.0); ul(); } return 0; }
static void *feeder(void *a){
    (void)a;
    while (!g_stop && g_nw < 4096){
        usleep(10000);
        double t0 = now(); lk(); g_w[g_nw++] = now() - t0; busy(0.05); ul();
    }
    return 0;
}
static int dcmp(const void *a, const void *b){ double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static void starvation(void){
    g_stop = 0; g_nw = 0;
    pthread_t g, f;
    pthread_create(&g, 0, game, 0); pthread_create(&f, 0, feeder, 0);
    usleep(3000000); g_stop = 1;
    pthread_join(g, 0); pthread_join(f, 0);
    qsort(g_w, (size_t)g_nw, sizeof g_w[0], dcmp);
    printf("  %-6s feeder waits (%d): p50 %.3f  p90 %.3f  p99 %.3f  max %.3f ms\n", g_use_mx ? "mutex" : "ticket", g_nw,
           g_w[g_nw / 2], g_w[g_nw * 9 / 10], g_w[g_nw * 99 / 100], g_w[g_nw - 1]);
}

int main(void){
    printf("ping-pong (1 s each):\n");
    for (g_use_mx = 0; g_use_mx < 2; g_use_mx++) for (int nt = 2; nt <= 8; nt *= 2) pingpong(nt);
    printf("starvation (3 s each; game thread holds 2 ms and re-takes at once, feeder every 10 ms):\n");
    for (g_use_mx = 0; g_use_mx < 2; g_use_mx++) starvation();
    return 0;
}
