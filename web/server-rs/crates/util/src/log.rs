//! The console: one line at a time to stdout (or stderr), and [`limited`]: the lines a client causes (refused
//! requests and upgrades, joins and leaves, the access log, Online lines), at most `per` a minute of one topic for
//! one client, then one line that says how many were left out.  So one client can no longer fill the journal, push
//! the VPS's other logs out or make journald drop real players' lines (the audit's DEP-4).  The client is only the
//! key of the count: no line names its address.

use std::io::Write;
use std::sync::OnceLock;
use std::time::Duration;

use std::collections::{BTreeMap, HashMap};

use parking_lot::Mutex;

use crate::clock::now_ms;

/// one line to stdout (console.log)
pub fn line(s: &str) {
    write_line(&mut console(), s);
}

fn write_line(o: &mut impl Write, s: &str) {
    let _ = o.write_all(s.as_bytes());
    let _ = o.write_all(b"\n");
    let _ = o.flush();
}

/// stdout, locked: one writer at a time (in this crate's own tests, a buffer they read back)
#[cfg(not(test))]
fn console() -> std::io::StdoutLock<'static> {
    std::io::stdout().lock()
}
#[cfg(test)]
use tests::console;

/// one line to stderr (console.error)
pub fn error(s: &str) {
    let out = std::io::stderr();
    let mut o = out.lock();
    let _ = o.write_all(s.as_bytes());
    let _ = o.write_all(b"\n");
    let _ = o.flush();
}

struct Quiet {
    t: i64,
    n: u32,
    left: u32,
    kind: String,
    topic: String,
}

/// The counts by topic and client, in the order they began (the lines that say how many were left out come in that
/// order).  A count leaves and comes back at the end each minute, among up to 10,000 of them: a place
/// number per count keeps the order with no shift of the others (an IndexMap's ordered removal moved them all, under
/// the lock every client's line takes).
#[derive(Default)]
struct Counts {
    place: HashMap<String, u64>,
    order: BTreeMap<u64, (String, Quiet)>,
    next: u64,
}

impl Counts {
    fn get(&self, k: &str) -> Option<&Quiet> {
        self.place.get(k).and_then(|p| self.order.get(p)).map(|(_, q)| q)
    }
    fn get_mut(&mut self, k: &str) -> Option<&mut Quiet> {
        let p = *self.place.get(k)?;
        self.order.get_mut(&p).map(|(_, q)| q)
    }
    fn contains_key(&self, k: &str) -> bool {
        self.place.contains_key(k)
    }
    fn len(&self) -> usize {
        self.place.len()
    }
    /// at the end of the order
    fn insert(&mut self, k: String, q: Quiet) {
        let p = self.next;
        self.next += 1;
        if let Some(old) = self.place.insert(k.clone(), p) {
            self.order.remove(&old);
        }
        self.order.insert(p, (k, q));
    }
    fn remove(&mut self, k: &str) -> Option<Quiet> {
        let p = self.place.remove(k)?;
        self.order.remove(&p).map(|(_, q)| q)
    }
    /// every count, in order
    fn drain(&mut self) -> Vec<Quiet> {
        self.place.clear();
        std::mem::take(&mut self.order).into_values().map(|(_, q)| q).collect()
    }
    /// the counts `keep` says no to go, in order, each seen by `keep`
    fn retain(&mut self, mut keep: impl FnMut(&Quiet) -> bool) {
        let gone: Vec<u64> = self.order.iter().filter(|(_, (_, q))| !keep(q)).map(|(p, _)| *p).collect();
        for p in gone {
            if let Some((k, _)) = self.order.remove(&p) {
                self.place.remove(&k);
            }
        }
    }
}

fn quiet() -> &'static Mutex<Counts> {
    static Q: OnceLock<Mutex<Counts>> = OnceLock::new();
    Q.get_or_init(|| {
        // every 15 s: the counts of the minutes that are over
        std::thread::Builder::new()
            .name("log-quiet".into())
            .spawn(|| {
                loop {
                    std::thread::sleep(Duration::from_secs(15));
                    flush_all(false);
                }
            })
            .ok();
        Mutex::new(Counts::default())
    })
}

/// the line that says how many of a minute's lines were left out, if any were
fn left_out(b: &Quiet) -> Option<String> {
    (b.left > 0).then(|| format!("[{}] {} more {} from one client in that minute (not shown)", b.kind, b.left, b.topic))
}

fn flush_all(force: bool) {
    let now = now_ms();
    let mut out = Vec::new();
    let mut q = quiet().lock();
    q.retain(|b| {
        let over = force || now - b.t >= 60_000;
        if over {
            out.extend(left_out(b));
        }
        !over
    });
    if out.is_empty() {
        return;
    }
    // (the console taken before the counts are let go: see `limited`)
    let mut c = console();
    drop(q);
    for l in out {
        write_line(&mut c, &l);
    }
}

/// A line client `key` caused: shown while it is among the first `per` of its `kind` and `topic` in a minute, else
/// counted, and the count of those left out said once the minute is over ("[kind] N more topic from one client in
/// that minute (not shown)").  What goes out is decided under the counts' lock and written after it (with
/// --access-log every request comes here: none waits on another's write to the console).  A call that ends a minute
/// with lines left out takes the console before it lets the counts go, so that its "N more" line comes before every
/// line decided after it (a client over its share: once a minute at most).
pub fn limited(kind: &str, topic: &str, key: &str, text: &str, per: u32) {
    let now = now_ms();
    let k = format!("{kind} {topic} {key}");
    let mut out: Vec<String> = Vec::new();
    let (show, held) = {
        let mut q = quiet().lock();
        if q.get(&k).is_some_and(|b| now - b.t >= 60_000)
            && let Some(b) = q.remove(&k)
        {
            out.extend(left_out(&b));
        }
        if !q.contains_key(&k) {
            if q.len() >= 10_000 {
                // every count goes out at once: at most 10,000 are kept
                out.extend(q.drain().iter().filter_map(left_out));
            }
            q.insert(k.clone(), Quiet { t: now, n: 0, left: 0, kind: kind.to_string(), topic: topic.to_string() });
        }
        let b = q.get_mut(&k).expect("just made");
        b.n += 1;
        if b.n > per {
            b.left += 1;
        }
        (b.n <= per, (!out.is_empty()).then(console))
    };
    match held {
        Some(mut c) => {
            for l in &out {
                write_line(&mut c, l);
            }
            if show {
                write_line(&mut c, text);
            }
        }
        None if show => line(text),
        None => {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use parking_lot::{ReentrantMutex, ReentrantMutexGuard};
    use std::cell::RefCell;
    use std::time::Instant;

    /// the console of this crate's tests: what was written, for them to read back (reentrant, as stdout's lock is)
    static SINK: ReentrantMutex<RefCell<Vec<u8>>> = parking_lot::const_reentrant_mutex(RefCell::new(Vec::new()));

    pub(super) struct Console(ReentrantMutexGuard<'static, RefCell<Vec<u8>>>);

    impl Write for Console {
        fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
            self.0.borrow_mut().extend_from_slice(b);
            Ok(b.len())
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    pub(super) fn console() -> Console {
        Console(SINK.lock())
    }

    fn written(prefix: &str) -> Vec<String> {
        let g = SINK.lock();
        let all = String::from_utf8_lossy(&g.borrow()).into_owned();
        all.lines().filter(|l| l.starts_with(prefix)).map(str::to_string).collect()
    }

    // the counts keep the order they began in, a count that comes back goes to the end
    #[test]
    fn counts_keep_the_maps_order() {
        let q = |t: i64| Quiet { t, n: 0, left: 0, kind: String::new(), topic: String::new() };
        let mut c = Counts::default();
        for (k, t) in [("a", 1), ("b", 2), ("c", 3)] {
            c.insert(k.into(), q(t));
        }
        assert_eq!(c.remove("a").map(|x| x.t), Some(1));
        c.insert("a".into(), q(4));
        c.retain(|x| x.t != 2);
        assert_eq!(c.len(), 2);
        assert!(c.get("b").is_none() && c.contains_key("c"));
        c.get_mut("c").unwrap().n = 7;
        assert_eq!(c.drain().iter().map(|x| (x.t, x.n)).collect::<Vec<_>>(), [(3, 7), (4, 0)]);
        assert_eq!(c.len(), 0);
    }

    // The line that says how many of a client's lines a minute left out comes before any line decided after it.  The
    // line is decided under the counts' lock and written after it, so another thread's line decided later could be
    // written first, while the console was busy: "[order] third" before "1 more".
    #[test]
    fn a_minutes_count_comes_before_the_lines_decided_after_it() {
        let (kind, topic, key) = ("order", "test lines", "client a");
        let k = format!("{kind} {topic} {key}");
        limited(kind, topic, key, "[order] first", 1);
        limited(kind, topic, key, "[order] left out", 1);
        // that minute is over
        quiet().lock().get_mut(&k).expect("its count").t -= 60_000;
        // the console busy (another line being written) while a call ends the minute: "1 more", then its own line
        let busy = console();
        let a = std::thread::spawn(move || limited(kind, topic, key, "[order] second", 2));
        // a call decided after that one: made here once that one has let the counts go (its line can be written at
        // once: this thread holds the console); not made while that one holds the counts until it has the console
        let t0 = Instant::now();
        let mut held_since: Option<Instant> = None;
        let mut third = false;
        while !third {
            assert!(t0.elapsed() < Duration::from_secs(5), "the other call did not come");
            match quiet().try_lock() {
                Some(q) => {
                    held_since = None;
                    if q.get(&k).is_some_and(|b| b.n == 1) {
                        drop(q);
                        limited(kind, topic, key, "[order] third", 2);
                        third = true;
                    }
                }
                None if held_since.is_some_and(|t| t.elapsed() > Duration::from_millis(100)) => break,
                None => held_since = held_since.or(Some(Instant::now())),
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        drop(busy);
        a.join().unwrap();
        if !third {
            limited(kind, topic, key, "[order] third", 2);
        }
        assert_eq!(
            written("[order]"),
            ["[order] first", "[order] 1 more test lines from one client in that minute (not shown)", "[order] second", "[order] third"]
        );
    }
}
