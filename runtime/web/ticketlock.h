/* wasm: a FIFO ticket lock on wasm atomics (memory.atomic.wait32/notify).
 *
 * The guest lock (win32_ext.c) uses it in the browser.  wasi-libc's
 * pthread_mutex is a futex lock with no order: a thread that unlocks and
 * relocks at once (the fair yield, a 10 ms Sleep loop) usually wins again,
 * and the threads parked on the lock (the DirectSound feeder, the loader,
 * the winmm timer) can starve.  Here every locker draws a ticket and the
 * lock is handed over in ticket order.
 *
 * Unlock wakes every waiter and each re-checks its ticket: the game has at
 * most ~8 threads, so a targeted wake is not worth a per-ticket word.  All
 * operations are sequentially consistent (as every wasm atomic is), which the
 * "waiters?" check in unlock relies on: either the unlocker sees a new ticket
 * and notifies, or the new waiter sees the new owner number.  Must not be
 * used on the page's main thread (Atomics.wait is not allowed there); every
 * guest thread is a worker.
 *
 * The owner is whoever turned `held` from 0 to nonzero: a served ticket (1)
 * or a barger (2).  ticket_barge takes a free lock ahead of the queue, for
 * the frame pacer's thread at its slot boundary.  A woken worker sometimes
 * runs only 3-4.6 ms after its notify (Chrome 155 on Windows 11: about 1 in
 * 1,800 wakes, measured between two plain workers), and a served ticket that
 * is still waking holds up everyone behind it: the game thread waited that
 * long behind a sound thread, and missed a 120 Hz slot. */
#ifndef NFSU2_TICKETLOCK_H
#define NFSU2_TICKETLOCK_H
#include <stdatomic.h>
#include <stdint.h>

typedef struct { _Atomic uint32_t next, serving, held; } ticketlock;
#define TICKETLOCK_INIT { 0, 0, 0 }

static inline uint32_t ticket_take(ticketlock *l){ return atomic_fetch_add(&l->next, 1u); }
/* Ticket `me` is served and nobody holds the lock: take it (1), else 0. */
static inline int ticket_try(ticketlock *l, uint32_t me){
    uint32_t z = 0;
    return atomic_load(&l->serving) == me && atomic_compare_exchange_strong(&l->held, &z, 1u);
}
/* Wait until ticket `me` is served and no barger holds the lock, and take it.
 * Returns 0 if it was taken at once. */
static inline int ticket_wait(ticketlock *l, uint32_t me){
    int waited = 0;
    uint32_t s = atomic_load(&l->serving);
    if (s != me){
        waited = 1;
        do __builtin_wasm_memory_atomic_wait32((int *)&l->serving, (int)s, -1);
        while ((s = atomic_load(&l->serving)) != me);
    }
    for (;;){                               /* served: the lock is ours unless a barger has it */
        uint32_t z = 0;
        if (atomic_compare_exchange_strong(&l->held, &z, 1u)) return waited;
        waited = 1;
        __builtin_wasm_memory_atomic_wait32((int *)&l->held, (int)z, -1);
    }
}
static inline void ticket_lock(ticketlock *l){ ticket_wait(l, ticket_take(l)); }
/* Threads waiting for the lock (0 = nobody). */
static inline uint32_t ticket_queued(ticketlock *l){
    return atomic_load(&l->next) - atomic_load(&l->serving) - (atomic_load(&l->held) == 1u ? 1u : 0u);
}
/* Hand the lock to the next ticket; returns how many threads still queue. */
static inline uint32_t ticket_unlock(ticketlock *l){
    atomic_store(&l->held, 0u);             /* before serving moves: the next ticket must find it free */
    uint32_t s = atomic_fetch_add(&l->serving, 1u) + 1u;
    uint32_t q = atomic_load(&l->next) - s;
    if (q) __builtin_wasm_memory_atomic_notify((int *)&l->serving, ~0u);
    return q;
}
/* Take the lock if nobody holds it, ahead of the queue, polling up to `polls`
 * times while someone does: 1 = taken (release with ticket_barge_unlock). */
static inline int ticket_barge(ticketlock *l, uint32_t polls){
    for (uint32_t i = 0; i < polls; i++){
        uint32_t z = 0;
        if (atomic_load(&l->held) == 0u && atomic_compare_exchange_strong(&l->held, &z, 2u)) return 1;
    }
    return 0;
}
static inline void ticket_barge_unlock(ticketlock *l){
    atomic_store(&l->held, 0u);
    __builtin_wasm_memory_atomic_notify((int *)&l->held, ~0u);   /* a served ticket may wait for it */
}
#endif
