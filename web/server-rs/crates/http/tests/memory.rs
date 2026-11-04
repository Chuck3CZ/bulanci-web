//! What an idle keep-alive connection holds (thousands of them sit idle between a page's requests, for 65 s, or
//! 905 s for Cloudflare's): the bytes allocated, counted by a global allocator of this test's own, after one exchange
//! of a few bytes each way and after one with a head and an answer just under 16 KiB.  (A connection kept the buffers
//! its largest exchange had grown, its parser's for the head and its answer's for the head and a body written with
//! it, for as long as it lived: about 32 KiB each, for whoever sends one large head on each of its connections.)

mod common;

use std::alloc::{GlobalAlloc, Layout, System};
use std::net::SocketAddr;
use std::sync::atomic::{AtomicIsize, Ordering::Relaxed};
use std::time::Duration;

use common::TestService;
use nfsu2_http::Limits;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

/// bytes allocated now, in this test binary
static LIVE: AtomicIsize = AtomicIsize::new(0);

struct Counting;

// SAFETY: every call goes to the system allocator as it came; only the count is added
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        let p = unsafe { System.alloc(l) };
        if !p.is_null() {
            LIVE.fetch_add(l.size() as isize, Relaxed);
        }
        p
    }
    unsafe fn alloc_zeroed(&self, l: Layout) -> *mut u8 {
        let p = unsafe { System.alloc_zeroed(l) };
        if !p.is_null() {
            LIVE.fetch_add(l.size() as isize, Relaxed);
        }
        p
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        unsafe { System.dealloc(p, l) };
        LIVE.fetch_sub(l.size() as isize, Relaxed);
    }
    unsafe fn realloc(&self, p: *mut u8, l: Layout, new: usize) -> *mut u8 {
        let q = unsafe { System.realloc(p, l, new) };
        if !q.is_null() {
            LIVE.fetch_add(new as isize - l.size() as isize, Relaxed);
        }
        q
    }
}

#[global_allocator]
static ALLOC: Counting = Counting;

/// an answer's whole length once its head is in: the head and its Content-Length
fn answer_len(b: &[u8]) -> Option<usize> {
    let end = b.windows(4).position(|w| w == b"\r\n\r\n")?;
    let head = String::from_utf8_lossy(&b[..end]).to_ascii_lowercase();
    let len: usize = head.split("content-length: ").nth(1).and_then(|r| r.split("\r\n").next()).and_then(|v| v.parse().ok()).unwrap_or(0);
    Some(end + 4 + len)
}

/// `n` connections, each one request (a head of about `head` bytes) and its answer (`body` bytes in one write, so
/// that the head and the body go out together), then all of them idle: the bytes allocated per connection
async fn held(addr: SocketAddr, n: usize, head: usize, body: usize) -> f64 {
    let pad: String = (0..head / 1000).map(|i| format!("X-Pad-{i:02}: {}\r\n", "p".repeat(990))).collect();
    let req = format!("GET /huge?{body} HTTP/1.1\r\nHost: localhost\r\n{pad}\r\n");
    let mut buf = vec![0u8; 64 << 10];
    let mut conns = Vec::with_capacity(n);
    let before = LIVE.load(Relaxed);
    for _ in 0..n {
        let mut s = TcpStream::connect(addr).await.expect("connect");
        s.write_all(req.as_bytes()).await.expect("write");
        let (mut got, mut want) = (0usize, usize::MAX);
        while got < want {
            let k = s.read(&mut buf[got..]).await.expect("read");
            assert!(k > 0, "closed");
            got += k;
            if want == usize::MAX {
                want = answer_len(&buf[..got]).unwrap_or(usize::MAX);
            }
        }
        conns.push(s);
    }
    // (the connections' tasks reach their wait for the next request)
    tokio::time::sleep(Duration::from_millis(300)).await;
    let per = (LIVE.load(Relaxed) - before) as f64 / n as f64;
    drop(conns);
    tokio::time::sleep(Duration::from_millis(300)).await;
    per
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn an_idle_connection_keeps_no_buffer_its_last_exchange_grew() {
    let n = 300;
    let addr = common::start(Limits { max_connections: 4 * n, ..Limits::default() }, TestService::new(Duration::from_secs(65))).await;
    let small = held(addr, n, 0, 100).await;
    let large = held(addr, n, 15_000, 15_000).await;
    eprintln!("an idle connection holds {small:.0} bytes after a small exchange, {large:.0} after a large one");
    assert!(large - small < 4096.0, "{:.0} bytes more after a large exchange", large - small);
}
