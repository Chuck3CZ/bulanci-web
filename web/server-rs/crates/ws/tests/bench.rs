//! How fast WsReader reads (ignored by default; in release:
//! `cargo.sh ws test -p nfsu2-ws --release --test bench -- --ignored --nocapture`).

mod common;

use std::time::Instant;

use bytes::Bytes;
use common::*;
use nfsu2_ws::{Message, WsReader};

/// `keep`: payloads held at once (as queues to slow receivers hold them), so that blocks stay pinned and new ones
/// are made
async fn measure(what: &str, payload: usize, frames: usize, keep: usize) {
    let p: Vec<u8> = (0..payload).map(|i| i as u8).collect();
    let one = binary(&p);
    let data = one.repeat(frames);
    let mut held = std::collections::VecDeque::with_capacity(keep + 1);
    let t = Instant::now();
    // from memory through the AsyncRead path, as from a socket (reads of what the block has room for)
    let mut r = WsReader::new(&data[..], Bytes::new(), 65536);
    let (mut n, mut bytes) = (0usize, 0usize);
    while let Ok(m) = r.next().await {
        let Message::Binary(b) = m else { panic!() };
        bytes += b.len();
        n += 1;
        if keep > 0 {
            held.push_back(b);
            if held.len() > keep {
                held.pop_front();
            }
        }
    }
    let s = t.elapsed().as_secs_f64();
    assert_eq!((n, bytes), (frames, frames * payload));
    eprintln!(
        "{what}: {frames} frames of {payload} bytes in {:.1} ms: {:.1} M frames/s, {:.0} MB/s of payload",
        s * 1e3,
        frames as f64 / s / 1e6,
        bytes as f64 / s / 1e6
    );
}

#[tokio::test]
#[ignore]
async fn read_a_million_small_frames() {
    measure("small", 32, 1_000_000, 0).await;
    measure("small, 4096 held", 32, 1_000_000, 4096).await;
    measure("roam-sized", 200, 1_000_000, 0).await;
    measure("roam-sized, 4096 held", 200, 1_000_000, 4096).await;
    measure("large", 8192, 100_000, 0).await;
}
