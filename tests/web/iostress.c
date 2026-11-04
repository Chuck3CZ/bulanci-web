/* Host I/O round trips under load (web/tools/iostress.sh): 8 threads each do
 * 20000 small preads at pseudo-random offsets of one /data file and compare
 * them with a copy read up front.  Every read is one RPC to the page's I/O
 * worker, answered within microseconds from its block cache, which is where
 * a waiter that misses the "done" notification hangs for good. */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#define NT 8
#define N 20000
static const char *path = "/data/GLOBAL/GLOBALB.BUN";
static unsigned char *ref; static long size;
static int bad[NT];
static void *run(void *a){
    int t = (int)(long)a;
    int fd = open(path, O_RDONLY);
    if (fd < 0){ bad[t] = -1; return 0; }
    unsigned x = 12345u + (unsigned)t * 7919u;
    for (int i = 0; i < N; i++){
        x = x * 1103515245u + 12345u;
        long off = (long)(x % (unsigned)(size - 64));
        unsigned char b[64];
        if (pread(fd, b, 64, off) != 64 || memcmp(b, ref + off, 64)) bad[t]++;
    }
    close(fd);
    return 0;
}
int main(void){
    struct stat st; if (stat(path, &st)) { printf("FAIL stat\n"); return 1; }
    size = (long)st.st_size; if (size > (8 << 20)) size = 8 << 20;     /* the first 8 MB */
    ref = malloc((size_t)size);
    int fd = open(path, O_RDONLY); if (fd < 0 || pread(fd, ref, (size_t)size, 0) != size){ printf("FAIL read\n"); return 1; }
    close(fd);
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_t th[NT];
    for (long i = 0; i < NT; i++) pthread_create(&th[i], 0, run, (void *)i);
    for (int i = 0; i < NT; i++) pthread_join(th[i], 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    int errs = 0; for (int i = 0; i < NT; i++) errs += bad[i];
    printf("%s: %d threads x %d reads in %.0f ms (%.1f us per round trip), %d bad\n", errs ? "FAIL" : "OK",
           NT, N, ms, ms * 1000.0 / ((double)NT * N) * NT, errs);
    return errs ? 1 : 0;
}
