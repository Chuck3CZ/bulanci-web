//! Requests a second, the engine against Node's http server (tests/node/ref_server.mjs), the same client for both:
//! many keep-alive connections, one request at a time on each.  Not a test of correctness; run by hand:
//!   cargo test --release -p nfsu2-http --test bench -- --ignored --nocapture

mod common;

use std::net::SocketAddr;
use std::time::{Duration, Instant};

use common::{NodeRef, TestService};
use nfsu2_http::Limits;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

/// an answer's whole length once its head is in: the head and its Content-Length
fn answer_len(b: &[u8]) -> Option<usize> {
    let end = b.windows(4).position(|w| w == b"\r\n\r\n")?;
    let head = String::from_utf8_lossy(&b[..end]).to_ascii_lowercase();
    let len: usize = head.split("content-length: ").nth(1).and_then(|r| r.split("\r\n").next()).and_then(|v| v.parse().ok()).unwrap_or(0);
    Some(end + 4 + len)
}

/// `conns` connections, `per` requests each: requests a second (the client on a runtime of its own)
fn load(addr: SocketAddr, conns: usize, per: usize, path: &'static str) -> f64 {
    std::thread::spawn(move || {
        let rt = tokio::runtime::Builder::new_multi_thread().worker_threads(4).enable_all().build().expect("runtime");
        rt.block_on(async move {
            let req = format!("GET {path} HTTP/1.1\r\nHost: localhost\r\nUser-Agent: bench\r\nAccept: */*\r\n\r\n");
            let t0 = Instant::now();
            let mut tasks = Vec::new();
            for _ in 0..conns {
                let req = req.clone();
                tasks.push(tokio::spawn(async move {
                    let mut s = TcpStream::connect(addr).await.expect("connect");
                    s.set_nodelay(true).expect("nodelay");
                    let mut buf = vec![0u8; 1 << 16];
                    for _ in 0..per {
                        s.write_all(req.as_bytes()).await.expect("write");
                        // one whole answer: its head, then Content-Length bytes
                        let mut got = 0usize;
                        let mut want = usize::MAX;
                        while got < want {
                            if buf.len() - got < 16 << 10 {
                                buf.resize(buf.len() * 2, 0);
                            }
                            let n = s.read(&mut buf[got..]).await.expect("read");
                            assert!(n > 0, "closed");
                            got += n;
                            if want == usize::MAX {
                                want = answer_len(&buf[..got]).unwrap_or(usize::MAX);
                            }
                        }
                    }
                }));
            }
            for t in tasks {
                t.await.expect("task");
            }
            (conns * per) as f64 / t0.elapsed().as_secs_f64()
        })
    })
    .join()
    .expect("load")
}

#[tokio::test(flavor = "multi_thread")]
#[ignore]
async fn requests_a_second_against_node() {
    let rust = common::start(Limits::default(), TestService::new(Duration::from_secs(65))).await;
    let node = NodeRef::start(serde_json::json!({})).await;
    for (conns, per, path) in [(1, 20_000, "/ka2"), (64, 2_000, "/ka2"), (256, 500, "/"), (64, 200, "/big")] {
        let r = load(rust, conns, per, path);
        let n = node.as_ref().map(|n| load(n.addr, conns, per, path));
        eprintln!(
            "{conns:>4} connections x {per:>6} {path:<5}: engine {r:>9.0}/s   node {}",
            n.map_or("(no node)".to_string(), |n| format!("{n:>9.0}/s   ({:.1}x)", r / n))
        );
    }
}
