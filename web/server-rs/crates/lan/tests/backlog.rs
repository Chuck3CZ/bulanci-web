//! The Online host behind: what a client sends it waits in that client's socket once the host's backlog passes 4096
//! frames (nfsu2_util::Backlog), as Node's one busy thread stopped reading; nobody else waits, and once the host
//! catches up the rest comes in order.  (Before, the switch's readers posted to the host's queue without end: memory
//! grew while its thread was behind, and the lobby lagged by the whole backlog.)

mod harness;

use std::collections::HashSet;
use std::sync::{Arc, Mutex};

use bytes::Bytes;
use harness::*;
use nfsu2_lan::LanOptions;
use nfsu2_util::{FrameCtx, Host};
use serde_json::{Value, json};
use tokio::sync::oneshot;

const ONLINE: u32 = 0x0A4D_0001;

/// an Online host whose thread is stuck: it keeps every frame (and with it the frame's place in the backlog)
#[derive(Default)]
struct Stuck {
    held: Mutex<Vec<(Bytes, FrameCtx)>>,
    got: Mutex<usize>,
}

impl Stuck {
    fn got(&self) -> usize {
        *self.got.lock().unwrap()
    }
    /// the thread catches up: what it held is handled, and from now on each frame at once
    fn release(&self) {
        self.held.lock().unwrap().clear();
    }
}

struct StuckHost(Arc<Stuck>, Arc<Mutex<bool>>);

impl Host for StuckHost {
    fn vip(&self) -> u32 {
        ONLINE
    }
    fn name(&self) -> &str {
        "NFSU2 Online"
    }
    fn listed(&self) -> bool {
        true
    }
    fn frame(&self, _from: u32, data: Bytes, ctx: FrameCtx) {
        *self.0.got.lock().unwrap() += 1;
        if *self.1.lock().unwrap() {
            self.0.held.lock().unwrap().push((data, ctx));
        }
    }
    fn peer_gone(&self, _vip: u32) {}
    fn info(&self) -> oneshot::Receiver<Value> {
        let (tx, rx) = oneshot::channel();
        let _ = tx.send(json!({}));
        rx
    }
    fn vips(&self) -> oneshot::Receiver<HashSet<u32>> {
        let (tx, rx) = oneshot::channel();
        let _ = tx.send(HashSet::new());
        rx
    }
    fn stop(&self) {}
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_host_behind_slows_down_only_the_clients_that_feed_it() {
    let stuck = Arc::new(Stuck::default());
    let holding = Arc::new(Mutex::new(true));
    // budgets that never stop these frames: the backlog alone must
    let o = LanOptions {
        frame_rate: 1e7,
        byte_rate: 1e10,
        group_frame_rate: 1e7,
        group_byte_rate: 1e10,
        site_frame_rate: 1e7,
        site_byte_rate: 1e10,
        ..opts()
    };
    let sw = start_full(o, "", Some(Arc::new(StuckHost(stuck.clone(), holding.clone()))), None, None).await;
    let mut a = Client::with(sw.port, "Alice", "?g=a", Opts::default()).await;
    let mut b = Client::with(sw.port, "Bob", "?g=b", Opts::default()).await;
    let mut c = Client::with(sw.port, "Carol", "?g=c", Opts::default()).await;
    for x in [&mut a, &mut b, &mut c] {
        x.welcome().await;
    }
    // Alice sends the host 8000 stream frames, 100 to a message
    let f = frame_of(ONLINE, b"lobby bytes", 4, 40000, 20922);
    for _ in 0..80 {
        a.send_bundle(&vec![(f.clone(), vec![]); 100]);
    }
    a.flush().await;
    sleep(500).await;
    let held = stuck.got();
    assert!((4096..=4096 + 200).contains(&held), "the host was given {held} while it was stuck: 4096 and one message more at most");
    // and nobody else waits: Bob's datagram reaches Carol
    b.udp(c.vip, 47210, 47210, b"free roam goes on", 1, 0);
    c.until_ms(2000, |g| g.dgrams().iter().any(|d| d.data == "free roam goes on").then_some(())).await;
    assert_eq!(stuck.got(), held, "nothing more of Alice's while the host is behind");
    // the host catches up: the rest of Alice's frames come
    *holding.lock().unwrap() = false;
    stuck.release();
    wait_for(5000, || stuck.got() == 8000).await;
    assert_eq!(stuck.got(), 8000);
    sw.close().await;
}

// A reader that waits for the host still ends when its connection must go: Alice's page comes back on a new connection
// while her old one waits, the new one takes her address over and the old one is terminated (its socket closes only
// once its reader has returned), with the host still stuck.  Her frames the host has not handled keep their places.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_client_waiting_for_the_host_still_goes_when_it_must() {
    let stuck = Arc::new(Stuck::default());
    let holding = Arc::new(Mutex::new(true));
    let o = LanOptions { frame_rate: 1e7, byte_rate: 1e10, group_frame_rate: 1e7, group_byte_rate: 1e10, ..opts() };
    let sw = start_full(o, "", Some(Arc::new(StuckHost(stuck.clone(), holding.clone()))), None, None).await;
    let mut a = Client::with(sw.port, "Alice", "?g=a", Opts::default()).await;
    a.welcome().await;
    let f = frame_of(ONLINE, b"lobby bytes", 4, 40000, 20922);
    for _ in 0..80 {
        a.send_bundle(&vec![(f.clone(), vec![]); 100]);
    }
    a.flush().await;
    wait_for(2000, || stuck.got() >= 4096).await;
    sleep(200).await;
    let held = stuck.got();
    assert!(held < 8000, "Alice's reader waits");
    let mut again = Client::new(sw.port, "Alice", "?g=a", Some((a.vip, &a.token)), Opts::default()).await;
    assert_eq!(again.welcome().await["vip"].as_u64(), Some(a.vip as u64), "her address, on the new connection");
    assert_eq!(a.closed().await, (1006, String::new()), "the old connection ended: no close frame (terminate)");
    assert_eq!(sw.peers().len(), 1);
    assert_eq!(stuck.got(), held, "nothing more of the old connection's");
    *holding.lock().unwrap() = false;
    stuck.release();
    sw.close().await;
}
