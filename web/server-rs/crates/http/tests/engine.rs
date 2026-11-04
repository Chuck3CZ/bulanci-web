//! The engine's rules over raw TCP, with short limits: the timers (Node's header and request times checked on its
//! interval, the socket timeout, the keep-alive cut), the pipelining cut (`Limits::pipelined`) and the rule that an
//! answer must move (`Limits::progress`), the connection limits, upgrades, and what a service may do (panic,
//! destroy, close, forget to end).
//! (tests/cross_node.rs compares the answers themselves with Node's, byte for byte.)

mod common;

use std::sync::Arc;
use std::sync::atomic::Ordering::Relaxed;
use std::time::Duration;

use common::{Case, TestService, exchange, read_all};
use nfsu2_http::Limits;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{Instant, sleep};

fn short() -> Limits {
    Limits {
        headers_timeout: Duration::from_millis(400),
        request_timeout: Duration::from_millis(1500),
        check_interval: Duration::from_millis(100),
        socket_idle: Duration::from_secs(10),
        ..Limits::default()
    }
}

async fn engine(limits: Limits, keep_alive: Duration) -> (std::net::SocketAddr, Arc<TestService>) {
    let svc = TestService::new(keep_alive);
    (common::start(limits, svc.clone()).await, svc)
}

fn text(b: &[u8]) -> String {
    common::mask_dates(b)
}

fn count(hay: &str, needle: &str) -> usize {
    hay.matches(needle).count()
}

// ---- keep-alive ----

#[tokio::test]
async fn requests_one_after_another_on_one_connection_then_the_keep_alive_cut() {
    let (addr, _) = engine(short(), Duration::from_millis(300)).await;
    let mut s = TcpStream::connect(addr).await.unwrap();
    for i in 0..5 {
        s.write_all(format!("GET /n{i} HTTP/1.1\r\nHost: a\r\n\r\n").as_bytes()).await.unwrap();
        let mut got = Vec::new();
        while !text(&got).contains("\"h\":[[\"host\",\"a\"]]}") {
            let mut b = [0u8; 4096];
            let n = s.read(&mut b).await.unwrap();
            assert!(n > 0, "closed after {i} answers");
            got.extend_from_slice(&b[..n]);
        }
        let t = text(&got);
        assert!(t.starts_with("HTTP/1.1 200 OK\r\n"), "{t}");
        // whole seconds, floored, as Node writes them
        assert!(t.contains("\r\nConnection: keep-alive\r\nKeep-Alive: timeout=0\r\n"), "{t}");
    }
    // idle: Node keeps it its keep-alive time and one second more, then closes without a word
    let t0 = Instant::now();
    let (rest, closed) = read_all(&mut s, 3000).await;
    let dt = t0.elapsed();
    assert!(closed && rest.is_empty(), "{:?}", text(&rest));
    assert!(dt >= Duration::from_millis(1250) && dt < Duration::from_millis(1800), "closed after {dt:?}");
}

#[tokio::test]
async fn a_head_that_comes_in_slowly_after_an_answer_is_cut_by_the_header_time() {
    // the keep-alive time is long; the next request's first byte starts Node's header clock
    let (addr, _) = engine(short(), Duration::from_secs(30)).await;
    let mut s = TcpStream::connect(addr).await.unwrap();
    s.write_all(b"GET / HTTP/1.1\r\n\r\n").await.unwrap();
    let (first, _) = read_all(&mut s, 300).await;
    assert!(text(&first).starts_with("HTTP/1.1 200 OK"));
    sleep(Duration::from_millis(600)).await;
    let t0 = Instant::now();
    s.write_all(b"GET / HTTP/1.1\r\n").await.unwrap();
    let (got, closed) = read_all(&mut s, 2000).await;
    let dt = t0.elapsed();
    assert_eq!(text(&got), "HTTP/1.1 408 Request Timeout\r\ncontent-length: 0\r\nconnection: close\r\n\r\n");
    assert!(closed);
    // 400 ms, at the next check (100 ms)
    assert!(dt >= Duration::from_millis(390) && dt < Duration::from_millis(800), "{dt:?}");
}

// ---- Node's header and request times (slow-loris) ----

#[tokio::test]
async fn slow_headers_get_408_at_the_check_after_the_header_time() {
    let (addr, _) = engine(short(), Duration::from_secs(65)).await;
    let mut s = TcpStream::connect(addr).await.unwrap();
    let t0 = Instant::now();
    s.write_all(b"GET / HTTP/1.1\r\nHost: localhost\r\n").await.unwrap();
    let (mut r, mut w) = s.into_split();
    let drip = tokio::spawn(async move {
        for i in 0..40 {
            sleep(Duration::from_millis(50)).await;
            if w.write_all(format!("X-Drip-{i}: 1\r\n").as_bytes()).await.is_err() {
                return;
            }
        }
    });
    let mut got = Vec::new();
    let _ = tokio::time::timeout(Duration::from_secs(3), r.read_to_end(&mut got)).await;
    let dt = t0.elapsed();
    drip.abort();
    assert!(text(&got).starts_with("HTTP/1.1 408 Request Timeout\r\n"), "{:?}", text(&got));
    assert!(dt >= Duration::from_millis(400) && dt < Duration::from_millis(900), "{dt:?}");
}

#[tokio::test]
async fn a_connection_that_sends_nothing_gets_408_too() {
    // Node counts from the connection's start until its first byte
    let (addr, _) = engine(short(), Duration::from_secs(65)).await;
    let mut s = TcpStream::connect(addr).await.unwrap();
    let t0 = Instant::now();
    let (got, closed) = read_all(&mut s, 2000).await;
    assert!(closed);
    assert!(text(&got).starts_with("HTTP/1.1 408"), "{:?}", text(&got));
    assert!(t0.elapsed() < Duration::from_millis(900));
}

#[tokio::test]
async fn a_body_that_trickles_gets_408_after_the_request_time() {
    let (addr, _) = engine(short(), Duration::from_secs(65)).await;
    let mut s = TcpStream::connect(addr).await.unwrap();
    let t0 = Instant::now();
    s.write_all(b"POST /body HTTP/1.1\r\nContent-Length: 100\r\n\r\n").await.unwrap();
    let (mut r, mut w) = s.into_split();
    let drip = tokio::spawn(async move {
        for _ in 0..100 {
            sleep(Duration::from_millis(100)).await;
            if w.write_all(b"x").await.is_err() {
                return;
            }
        }
    });
    let mut got = Vec::new();
    let _ = tokio::time::timeout(Duration::from_secs(4), r.read_to_end(&mut got)).await;
    let dt = t0.elapsed();
    drip.abort();
    // the service was waiting for the body and had written nothing: the refusal, not its answer
    assert_eq!(text(&got), "HTTP/1.1 408 Request Timeout\r\ncontent-length: 0\r\nconnection: close\r\n\r\n");
    assert!(dt >= Duration::from_millis(1500) && dt < Duration::from_millis(2100), "{dt:?}");
}

#[tokio::test]
async fn the_socket_timeout_closes_without_an_answer() {
    let limits = Limits {
        headers_timeout: Duration::ZERO,
        request_timeout: Duration::ZERO,
        socket_idle: Duration::from_millis(300),
        ..Limits::default()
    };
    let (addr, _) = engine(limits, Duration::ZERO).await;
    // nothing at all
    let mut s = TcpStream::connect(addr).await.unwrap();
    let t0 = Instant::now();
    let (got, closed) = read_all(&mut s, 2000).await;
    assert!(closed && got.is_empty());
    assert!(t0.elapsed() >= Duration::from_millis(290) && t0.elapsed() < Duration::from_millis(700));
    // activity resets it: a byte every 200 ms keeps the connection
    let mut s = TcpStream::connect(addr).await.unwrap();
    for _ in 0..5 {
        sleep(Duration::from_millis(200)).await;
        s.write_all(b"\r\n").await.unwrap();
    }
    s.write_all(b"GET / HTTP/1.1\r\n\r\n").await.unwrap();
    let (got, _) = read_all(&mut s, 200).await;
    assert!(text(&got).starts_with("HTTP/1.1 200 OK"), "{:?}", text(&got));
}

// ---- pipelining: at most `pipelined` requests on a connection without a finished answer ----

#[tokio::test]
async fn pipelined_requests_within_the_limit_are_answered_in_order() {
    let (addr, _) = engine(short(), Duration::from_secs(65)).await;
    let got =
        exchange(addr, &Case::new("GET /a HTTP/1.1\r\n\r\nGET /slow HTTP/1.1\r\n\r\nGET /c HTTP/1.1\r\n\r\nGET /d HTTP/1.1\r\n\r\n")).await;
    assert_eq!(count(&got.text, "HTTP/1.1 200 OK"), 4);
    let (a, b, c) = (got.text.find("\"u\":\"/a\"").unwrap(), got.text.find("slow\n").unwrap(), got.text.find("\"u\":\"/c\"").unwrap());
    assert!(a < b && b < c);
    assert!(!got.closed);
}

#[tokio::test]
async fn thousands_queued_and_never_read_cut_the_connection_at_once() {
    let (addr, svc) = engine(short(), Duration::from_secs(65)).await;
    let mut s = TcpStream::connect(addr).await.unwrap();
    let t0 = Instant::now();
    // each answer 1 MiB, and the client reads nothing
    let _ = s.write_all("GET /huge?1048576 HTTP/1.1\r\n\r\n".repeat(5000).as_bytes()).await;
    sleep(Duration::from_millis(300)).await;
    let (got, closed) = read_all(&mut s, 2000).await;
    assert!(closed, "the connection is cut");
    assert!(got.len() < 4 << 20, "{} bytes", got.len());
    assert!(t0.elapsed() < Duration::from_secs(3));
    sleep(Duration::from_millis(100)).await;
    assert_eq!(svc.held.load(Relaxed), 0, "the connection's share given back");
    // the server is fine
    let ok = exchange(addr, &Case::new("GET / HTTP/1.1\r\nConnection: close\r\n\r\n")).await;
    assert!(ok.text.starts_with("HTTP/1.1 200 OK"));
}

#[tokio::test]
async fn requests_the_pipelining_count_never_sees_cannot_pile_up_behind_an_answer() {
    // (an Expect other than 100-continue goes to Node's checkExpectation, which the Node server's request handler
    // never saw: such requests do not count toward `pipelined`, and behind an answer that took its time the
    // connection read and queued them without end, each a whole Request in memory)
    let (addr, _) = engine(short(), Duration::from_secs(65)).await;
    let s = TcpStream::connect(addr).await.unwrap();
    socket2::SockRef::from(&s).set_send_buffer_size(64 << 10).unwrap();
    let (mut r, mut w) = s.into_split();
    let reader = tokio::spawn(async move {
        let mut got = Vec::new();
        let _ = tokio::time::timeout(Duration::from_secs(6), r.read_to_end(&mut got)).await;
        got
    });
    w.write_all(b"GET /wait HTTP/1.1\r\n\r\n").await.unwrap();
    let block = format!("GET / HTTP/1.1\r\nExpect: x\r\nX-Pad: {}\r\n\r\n", "p".repeat(1000)).repeat(64);
    let mut sent = 0usize;
    let until = Instant::now() + Duration::from_millis(1500);
    while sent < 48 << 20 {
        match tokio::time::timeout_at(until, w.write_all(block.as_bytes())).await {
            Ok(Ok(())) => sent += block.len(),
            _ => break,
        }
    }
    // what the kernel's buffers and the connection's own read buffer hold, not 48 MiB of requests
    assert!(sent < 16 << 20, "{} MiB taken while the first answer was under way", sent >> 20);
    drop(w);
    let got = text(&reader.await.unwrap());
    // its answer, then the first 417 (which closes the connection)
    assert!(got.starts_with("HTTP/1.1 200 OK\r\n") && got.contains("\r\n\r\nwaited\n"), "{}", &got[..got.len().min(300)]);
    // the server is fine
    let ok = exchange(addr, &Case::new("GET / HTTP/1.1\r\nConnection: close\r\n\r\n")).await;
    assert!(ok.text.starts_with("HTTP/1.1 200 OK"), "{ok:?}");
}

// ---- an answer must move: `progress_bytes` in every `progress` ----

#[tokio::test]
async fn an_answer_the_client_stops_reading_is_cut() {
    let limits = Limits { progress: Duration::from_millis(400), ..short() };
    let (addr, svc) = engine(limits, Duration::from_secs(65)).await;
    let mut s = TcpStream::connect(addr).await.unwrap();
    s.write_all(b"GET /huge?67108864 HTTP/1.1\r\n\r\n").await.unwrap();
    // never read: the kernel's buffers fill, the answer stops moving
    sleep(Duration::from_millis(2000)).await;
    assert_eq!(svc.held.load(Relaxed), 0, "cut: the connection went");
    let (got, closed) = read_all(&mut s, 3000).await;
    assert!(closed);
    assert!(got.len() < 64 << 20);
}

#[tokio::test]
async fn an_answer_that_starts_late_and_stops_moving_is_cut_on_the_progress_clock() {
    // (the first byte goes once the request was read in full, as a file's answer once the file is open: the
    // connection's one timer must move up to the progress checks, not wait for the request or socket times; it
    // waited for the 408 check, so a stalled download held its connection for 35 s instead of 25)
    let limits = Limits {
        progress: Duration::from_millis(400),
        headers_timeout: Duration::from_secs(10),
        request_timeout: Duration::from_secs(20),
        socket_idle: Duration::from_secs(30),
        ..short()
    };
    let (addr, svc) = engine(limits, Duration::from_secs(65)).await;
    let mut s = TcpStream::connect(addr).await.unwrap();
    let t0 = Instant::now();
    s.write_all(b"GET /huge-late?67108864 HTTP/1.1\r\n\r\n").await.unwrap();
    while svc.held.load(Relaxed) == 0 && t0.elapsed() < Duration::from_secs(1) {
        sleep(Duration::from_millis(1)).await;
    }
    assert_eq!(svc.held.load(Relaxed), 1, "the connection is the server's");
    while svc.held.load(Relaxed) > 0 && t0.elapsed() < Duration::from_secs(5) {
        sleep(Duration::from_millis(20)).await;
    }
    let dt = t0.elapsed();
    assert_eq!(svc.held.load(Relaxed), 0, "cut: the connection went");
    // 50 ms to its first byte, 400 ms without progress, the check after that (every 50 ms)
    assert!(dt < Duration::from_millis(1000), "cut after {dt:?}");
    let (got, closed) = read_all(&mut s, 3000).await;
    assert!(closed);
    assert!(got.len() < 64 << 20);
}

#[tokio::test]
async fn an_answer_read_as_a_trickle_is_cut_after_response_max() {
    // (what is measured is the server letting go, not the end the client sees: that comes once the kernel has sent
    // what it holds, megabytes on loopback, through the client's small window.  And the progress window is longer
    // than response_max: the kernel takes the first megabytes of the answer at once, and the next write waits until
    // half of them drained, so even a steady trickle can show no progress for a while; Node counted the same way)
    let limits =
        Limits { progress: Duration::from_millis(2000), progress_bytes: 1024, response_max: Duration::from_millis(800), ..short() };
    let (addr, svc) = engine(limits, Duration::from_secs(65)).await;
    let s = TcpStream::connect(addr).await.unwrap();
    // a small receive buffer: the answer moves only as fast as the client reads
    socket2::SockRef::from(&s).set_recv_buffer_size(4096).unwrap();
    let (mut r, mut w) = s.into_split();
    w.write_all(b"GET /huge?67108864 HTTP/1.1\r\n\r\n").await.unwrap();
    let t0 = Instant::now();
    let reader = tokio::spawn(async move {
        let mut buf = [0u8; 2048];
        while let Ok(Ok(1..)) = tokio::time::timeout(Duration::from_secs(5), r.read(&mut buf)).await {
            sleep(Duration::from_millis(5)).await;
        }
    });
    while svc.held.load(Relaxed) == 0 && t0.elapsed() < Duration::from_secs(1) {
        sleep(Duration::from_millis(1)).await;
    }
    while svc.held.load(Relaxed) > 0 && t0.elapsed() < Duration::from_secs(5) {
        sleep(Duration::from_millis(10)).await;
    }
    let dt = t0.elapsed();
    assert_eq!(svc.held.load(Relaxed), 0, "cut: the connection went");
    // response_max, at the check after it (every 250 ms here)
    assert!(dt >= Duration::from_millis(800) && dt < Duration::from_millis(1600), "cut after {dt:?}");
    reader.abort();
}

// ---- connections ----

#[tokio::test]
async fn more_connections_than_the_listener_takes_are_closed_at_once() {
    let limits = Limits { max_connections: 2, ..short() };
    let (addr, _) = engine(limits, Duration::from_secs(65)).await;
    let a = TcpStream::connect(addr).await.unwrap();
    let b = TcpStream::connect(addr).await.unwrap();
    sleep(Duration::from_millis(100)).await;
    let mut c = TcpStream::connect(addr).await.unwrap();
    let t0 = Instant::now();
    let (got, closed) = read_all(&mut c, 1000).await;
    assert!(closed && got.is_empty() && t0.elapsed() < Duration::from_millis(300));
    drop((a, b));
    sleep(Duration::from_millis(100)).await;
    let ok = exchange(addr, &Case::new("GET / HTTP/1.1\r\nConnection: close\r\n\r\n")).await;
    assert!(ok.text.starts_with("HTTP/1.1 200 OK"), "free again: {:?}", ok.text);
}

#[tokio::test]
async fn a_connection_admit_refuses_is_closed_before_a_byte() {
    let (addr, svc) = engine(short(), Duration::from_secs(65)).await;
    svc.refuse.store(true, Relaxed);
    let got = exchange(addr, &Case::new("GET / HTTP/1.1\r\n\r\n")).await;
    assert!(got.closed && got.text.is_empty(), "{got:?}");
    svc.refuse.store(false, Relaxed);
    let ok = exchange(addr, &Case::new("GET / HTTP/1.1\r\nConnection: close\r\n\r\n")).await;
    assert!(ok.text.starts_with("HTTP/1.1 200"));
    sleep(Duration::from_millis(50)).await;
    assert_eq!(svc.held.load(Relaxed), 0);
}

#[tokio::test]
async fn remote_address_as_node_gives_it() {
    let svc = TestService::new(Duration::from_secs(65));
    let v4 = common::start_on("127.0.0.1", short(), svc.clone()).await;
    let got = exchange(v4, &Case::new("GET /conninfo HTTP/1.1\r\nConnection: close\r\n\r\n")).await;
    assert!(got.text.contains("\r\n\r\n127.0.0.1 false 127.0.0.1:"), "{}", got.text);
    let Ok((l, host)) = nfsu2_http::bind("::", 0, Some("0.0.0.0")).await else { return };
    let port = l.local_addr().unwrap().port();
    nfsu2_http::serve(l, None, Arc::new(short()), svc);
    if host != "::" {
        eprintln!("no IPv6 here: the dual-stack check is skipped");
        return;
    }
    let mapped = exchange(([127, 0, 0, 1], port).into(), &Case::new("GET /conninfo HTTP/1.1\r\nConnection: close\r\n\r\n")).await;
    assert!(mapped.text.contains("\r\n\r\n::ffff:127.0.0.1 false"), "{}", mapped.text);
    let six = exchange(
        std::net::SocketAddr::from((std::net::Ipv6Addr::LOCALHOST, port)),
        &Case::new("GET /conninfo HTTP/1.1\r\nConnection: close\r\n\r\n"),
    )
    .await;
    assert!(six.text.contains("\r\n\r\n::1 false"), "{}", six.text);
}

#[tokio::test]
async fn bind_says_why_it_failed() {
    let (_held, port) = common::taken_port().await;
    let e = nfsu2_http::bind("127.0.0.1", port, None).await.err();
    assert_eq!(e.as_deref(), Some(format!("port {port} is in use").as_str()));
    // an address of no interface here, with a fallback
    let (l, host) = nfsu2_http::bind("192.0.2.1", 0, Some("127.0.0.1")).await.expect("the fallback");
    assert_eq!(host, "127.0.0.1");
    assert!(l.local_addr().unwrap().ip().is_loopback());
    let e = nfsu2_http::bind("192.0.2.1", 0, None).await.err().unwrap_or_default();
    assert!(e.starts_with("listen EADDRNOTAVAIL: address not available 192.0.2.1:0"), "{e}");
}

// ---- upgrades ----

#[tokio::test]
async fn an_upgrade_hands_over_the_connection_and_what_came_after_its_head() {
    let (addr, _) = engine(short(), Duration::from_secs(65)).await;
    let got = exchange(addr, &Case::new(&b"GET /lan HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n\x81\x05hello"[..])).await;
    assert!(got.text.starts_with("HTTP/1.1 101 Switching Protocols\r\nx-up: {\"m\":\"GET\",\"u\":\"/lan\""), "{}", got.text);
    assert!(got.text.contains("x-pre: 810568656c6c6f\r\n"), "{}", got.text);
    assert!(got.closed);
}

// ---- what a service may do ----

#[tokio::test]
async fn a_panic_in_one_request_closes_its_connection_only() {
    let (addr, svc) = engine(short(), Duration::from_secs(65)).await;
    let got = exchange(addr, &Case::new("GET /panic HTTP/1.1\r\n\r\n")).await;
    assert!(got.closed && got.text.is_empty(), "{got:?}");
    assert_eq!(svc.panics.load(Relaxed), 1);
    let ok = exchange(addr, &Case::new("GET / HTTP/1.1\r\nConnection: close\r\n\r\n")).await;
    assert!(ok.text.starts_with("HTTP/1.1 200 OK"));
    sleep(Duration::from_millis(50)).await;
    assert_eq!(svc.held.load(Relaxed), 0, "the panicking connection gave its share back");
}

#[tokio::test]
async fn a_panic_in_admit_costs_that_connection_only() {
    // (admit runs on the listener's accept loop: a panic there ended the loop, and the port took no connection more)
    let (addr, svc) = engine(short(), Duration::from_secs(65)).await;
    svc.panic_admit.store(true, Relaxed);
    let got = exchange(addr, &Case::new("GET / HTTP/1.1\r\n\r\n")).await;
    assert!(got.closed && got.text.is_empty(), "{got:?}");
    let ok = exchange(addr, &Case::new("GET / HTTP/1.1\r\nConnection: close\r\n\r\n")).await;
    assert!(ok.text.starts_with("HTTP/1.1 200 OK"), "{ok:?}");
    sleep(Duration::from_millis(50)).await;
    assert_eq!(svc.held.load(Relaxed), 0);
}

#[tokio::test]
async fn destroy_close_after_and_a_service_that_forgets_to_end() {
    let (addr, _) = engine(short(), Duration::from_secs(65)).await;
    let d = exchange(addr, &Case::new("GET /destroy HTTP/1.1\r\n\r\n")).await;
    assert!(d.closed && d.text.ends_with("\r\n\r\n12345"), "{d:?}");
    let c = exchange(addr, &Case::new("GET /close-after HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n\r\n")).await;
    assert!(c.closed);
    assert!(c.text.contains("\r\nConnection: close\r\n\r\nok") && count(&c.text, "HTTP/1.1") == 1, "{c:?}");
    let n = exchange(addr, &Case::new("GET /noend HTTP/1.1\r\n\r\n")).await;
    assert!(n.text.ends_with("\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n") && !n.closed, "{n:?}");
    let nothing = exchange(addr, &Case::new("GET /nothing HTTP/1.1\r\n\r\n")).await;
    assert!(nothing.closed && nothing.text.is_empty(), "{nothing:?}");
    let i = exchange(addr, &Case::new("GET /implicit HTTP/1.1\r\n\r\n")).await;
    assert_eq!(
        i.text,
        "HTTP/1.1 200 OK\r\nDate: X\r\nConnection: keep-alive\r\nKeep-Alive: timeout=65\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"
    );
    let e = exchange(addr, &Case::new("GET /implicit-end HTTP/1.1\r\n\r\nGET /implicit-end HTTP/1.0\r\n\r\n")).await;
    assert_eq!(
        e.text,
        "HTTP/1.1 200 OK\r\nDate: X\r\nConnection: keep-alive\r\nKeep-Alive: timeout=65\r\nContent-Length: 0\r\n\r\n\
         HTTP/1.1 200 OK\r\nDate: X\r\nConnection: close\r\n\r\n"
    );
}

#[tokio::test]
async fn a_write_after_the_end_goes_nowhere() {
    // (it went out after the answer, where the client reads the next answer's head)
    let (addr, _) = engine(short(), Duration::from_secs(65)).await;
    let got = exchange(addr, &Case::new("GET /after-end HTTP/1.1\r\n\r\nGET /x HTTP/1.1\r\n\r\n")).await;
    assert!(got.text.contains("\r\n\r\nokHTTP/1.1 200 OK\r\n"), "{got:?}");
    assert!(!got.text.contains("XX"), "{got:?}");
}

#[tokio::test]
async fn bodies_up_to_a_limit() {
    let (addr, _) = engine(short(), Duration::from_secs(65)).await;
    let ok = exchange(addr, &Case::new("POST /small HTTP/1.1\r\nContent-Length: 4\r\n\r\nabcd")).await;
    assert!(ok.text.ends_with("\r\n\r\nabcd\n"), "{ok:?}");
    let big = exchange(addr, &Case::new("POST /small HTTP/1.1\r\nContent-Length: 5\r\n\r\nabcde")).await;
    assert!(big.text.starts_with("HTTP/1.1 413 Payload Too Large\r\n") && big.closed, "{big:?}");
    let chunked =
        exchange(addr, &Case::new("POST /small HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n")).await;
    assert!(chunked.text.starts_with("HTTP/1.1 413 ") && chunked.closed, "{chunked:?}");
    let in_bits = exchange(
        addr,
        &Case {
            parts: vec![b"POST /small HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nab\r\n".to_vec(), b"1\r\nc\r\n0\r\n\r\n".to_vec()],
            gap: Duration::from_millis(100),
            end: false,
            wait: Duration::from_millis(600),
        },
    )
    .await;
    assert!(in_bits.text.ends_with("\r\n\r\nabc\n"), "{in_bits:?}");
}

// ---- where the engine differs from Node on purpose ----

#[tokio::test]
async fn a_client_that_ends_its_side_still_gets_the_answers_under_way() {
    // Node ended the socket at the client's FIN and lost an answer not written yet; the engine sends it, then closes
    let (addr, _) = engine(short(), Duration::from_secs(65)).await;
    let got = exchange(
        addr,
        &Case {
            parts: vec![b"GET /slow HTTP/1.1\r\n\r\nGET /chunked HTTP/1.0\r\n\r\n".to_vec()],
            gap: Duration::ZERO,
            end: true,
            wait: Duration::from_millis(1000),
        },
    )
    .await;
    assert!(got.closed);
    assert!(got.text.contains("\r\n\r\nslow\n") && got.text.ends_with("\r\n\r\nhello world"), "{got:?}");
}

#[tokio::test]
async fn an_upgrade_behind_an_answer_under_way_waits_its_turn() {
    // Node emitted 'upgrade' at once and the slow answer was lost; here it goes first
    let (addr, _) = engine(short(), Duration::from_secs(65)).await;
    let got =
        exchange(addr, &Case::new("GET /slow HTTP/1.1\r\n\r\nGET /lan HTTP/1.1\r\nUpgrade: x\r\nConnection: upgrade\r\n\r\nXX")).await;
    let (slow, up) = (got.text.find("slow\n").unwrap_or(usize::MAX), got.text.find("101 Switching").unwrap_or(0));
    assert!(slow < up, "{got:?}");
}
