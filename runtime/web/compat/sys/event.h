/* wasm: no kqueue.  kqueue() fails, so callers take their nanosleep fallback. */
#pragma once
#include <stdint.h>
#include <time.h>
struct kevent { uintptr_t ident; int16_t filter; uint16_t flags; uint32_t fflags; intptr_t data; void *udata; };
#define EVFILT_TIMER (-7)
#define EV_ADD 0x0001
#define EV_ONESHOT 0x0010
#define NOTE_NSECONDS 0x00000004
#define NOTE_CRITICAL 0x00000020
#define EV_SET(kevp, a, b, c, d, e, f) do { (kevp)->ident = (a); (kevp)->filter = (b); (kevp)->flags = (c); \
    (kevp)->fflags = (d); (kevp)->data = (e); (kevp)->udata = (f); } while (0)
static inline int kqueue(void){ return -1; }
static inline int kevent(int kq, const struct kevent *c, int nc, struct kevent *e, int ne, const struct timespec *t){
    (void)kq; (void)c; (void)nc; (void)e; (void)ne; (void)t; return -1; }
