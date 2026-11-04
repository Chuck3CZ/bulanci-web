//! Threads of their own for the CPU work of the game data's runs (reading a chunk and compressing it, the probe):
//! a crowd of downloads queues here, never in front of the switch on the runtime's workers.  Half the cores, at least
//! two.  A job is a closure that owns what it works on (the encoder's state moves with it from chunk to chunk), and
//! its answer comes back through a oneshot.  The threads run at a lower priority (a nice value 10 above the process's,
//! nfsu2_util::cpu::lower_this_thread): a burst of downloads takes what the switch leaves of the cores, never the
//! other way round (a burst of 40 downloads put the switch's ping 20 % lower at p50 with it, at the same throughput).

use std::future::Future;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::{Arc, mpsc};

use parking_lot::Mutex;

type Job = Box<dyn FnOnce() + Send>;

pub struct CpuPool {
    tx: mpsc::Sender<Job>,
}

impl CpuPool {
    pub fn new(threads: usize, name: &str) -> CpuPool {
        let (tx, rx) = mpsc::channel::<Job>();
        let rx = Arc::new(Mutex::new(rx));
        for i in 0..threads.max(1) {
            let rx = rx.clone();
            std::thread::Builder::new()
                .name(format!("{name}-{i}"))
                .spawn(move || {
                    nfsu2_util::cpu::lower_this_thread(10);
                    loop {
                        // one thread waits on the queue at a time; the others wait for it to take a job
                        let job = rx.lock().recv();
                        match job {
                            // a job that panics loses its answer (the caller sees None), not the thread
                            Ok(job) => {
                                let _ = catch_unwind(AssertUnwindSafe(job));
                            }
                            Err(_) => break,
                        }
                    }
                })
                .expect("a compression thread");
        }
        CpuPool { tx }
    }

    /// f queued for one of the pool's threads now (not when the answer is first awaited: a run's next chunk
    /// compresses while this one goes out); the answer is None if f panicked
    pub fn submit<T: Send + 'static>(&self, f: impl FnOnce() -> T + Send + 'static) -> impl Future<Output = Option<T>> + Send + 'static {
        let (otx, orx) = tokio::sync::oneshot::channel();
        let job: Job = Box::new(move || {
            // nobody waits for the answer any more (its client went, with the next chunk of its run queued): the work
            // is skipped, and what it holds (the open file, the encoder's state) goes now
            if otx.is_closed() {
                return;
            }
            let _ = otx.send(f());
        });
        let queued = self.tx.send(job).is_ok();
        async move {
            if !queued {
                return None;
            }
            orx.await.ok()
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn runs_jobs_and_survives_a_panic() {
        let p = CpuPool::new(2, "test-pool");
        assert_eq!(p.submit(|| 6 * 7).await, Some(42));
        let bad: Option<u32> = p.submit(|| panic!("a broken job")).await;
        assert_eq!(bad, None);
        let all = futures_util::future::join_all((0..32).map(|i| p.submit(move || i * 2))).await;
        assert_eq!(all.into_iter().map(Option::unwrap).sum::<u32>(), (0..32u32).map(|i| i * 2).sum::<u32>());
    }

    // A run's next chunk is queued before this one goes out, so a client that goes (or resets right after its request)
    // leaves a job behind: it compressed a chunk for nobody, holding the file open and the encoder's state until then.
    #[tokio::test]
    async fn a_job_nobody_waits_for_is_not_run() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        let p = CpuPool::new(1, "test-pool");
        // the one thread busy until the gate opens, and jobs queued behind it
        let (gate, wait) = std::sync::mpsc::channel::<()>();
        let busy = p.submit(move || wait.recv().is_ok());
        let ran = Arc::new(AtomicUsize::new(0));
        let held = Arc::new(());
        let gone: Vec<_> = (0..10)
            .map(|_| {
                let (ran, held) = (ran.clone(), held.clone());
                p.submit(move || {
                    let _held = held;
                    ran.fetch_add(1, Ordering::Relaxed);
                })
            })
            .collect();
        drop(gone);
        let wanted = p.submit(|| 7);
        gate.send(()).unwrap();
        assert_eq!(busy.await, Some(true));
        assert_eq!(wanted.await, Some(7));
        assert_eq!(ran.load(Ordering::Relaxed), 0, "jobs whose answers nobody waits for");
        assert_eq!(Arc::strong_count(&held), 1, "what they held is let go");
    }
}
