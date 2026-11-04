//! A counter that many threads add to at once (frames routed, bytes, drops): each thread adds to a shard of its own,
//! on a cache line of its own, and a read sums them.  One shared atomic that every frame touches costs a cache-line
//! transfer between cores per frame (the .NET port's measurement: shared Interlocked counters mattered).

use std::cell::Cell;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};

use crossbeam_utils::CachePadded;

const SHARDS: usize = 32;

static NEXT_SHARD: AtomicUsize = AtomicUsize::new(0);
thread_local! {
    static SHARD: Cell<usize> = const { Cell::new(usize::MAX) };
}

#[inline]
fn shard() -> usize {
    SHARD.with(|s| {
        let v = s.get();
        if v != usize::MAX {
            return v;
        }
        let n = NEXT_SHARD.fetch_add(1, Ordering::Relaxed) % SHARDS;
        s.set(n);
        n
    })
}

pub struct Counter {
    shards: Box<[CachePadded<AtomicU64>]>,
}

impl Default for Counter {
    fn default() -> Self {
        Self::new()
    }
}

impl Counter {
    pub fn new() -> Self {
        Counter { shards: (0..SHARDS).map(|_| CachePadded::new(AtomicU64::new(0))).collect() }
    }
    #[inline]
    pub fn add(&self, n: u64) {
        self.shards[shard()].fetch_add(n, Ordering::Relaxed);
    }
    #[inline]
    pub fn inc(&self) {
        self.add(1);
    }
    pub fn get(&self) -> u64 {
        self.shards.iter().map(|s| s.load(Ordering::Relaxed)).sum()
    }
}

impl std::fmt::Debug for Counter {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.get())
    }
}
