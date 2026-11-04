//! What the integration tests share: a service that answers as tests/node/ref_server.mjs does (route for route, byte
//! for byte), the engine started on an ephemeral port, Node's reference server, and a raw TCP client.
#![allow(dead_code)]

use std::future::Future;
use std::net::SocketAddr;
use std::process::Stdio;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering::Relaxed};
use std::time::Duration;

use nfsu2_http::{ClientError, Limits, Request, Response, Service, Upgraded};
use serde_json::{Value, json};
use tokio::io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader};
use tokio::net::{TcpListener, TcpStream};
use tokio::process::{Child, Command};
use tokio::time::{Instant, sleep, timeout_at};

pub fn reason(code: u16) -> &'static str {
    match code {
        400 => "Bad Request",
        408 => "Request Timeout",
        431 => "Request Header Fields Too Large",
        _ => "",
    }
}

/// ref_server.mjs rawResponse
pub fn raw_response(code: u16) -> Vec<u8> {
    format!("HTTP/1.1 {code} {}\r\ncontent-length: 0\r\nconnection: close\r\n\r\n", reason(code)).into_bytes()
}

pub struct TestService {
    pub keep_alive: Duration,
    /// admit() says no
    pub refuse: AtomicBool,
    /// connections admitted and not gone yet
    pub held: Arc<AtomicUsize>,
    /// a request to /panic panics (the connection goes, the server stays)
    pub panics: AtomicUsize,
    /// the next admit() panics
    pub panic_admit: AtomicBool,
    /// every peer must show a client certificate (client_certificate())
    pub prove: AtomicBool,
}

impl TestService {
    pub fn new(keep_alive: Duration) -> Arc<TestService> {
        Arc::new(TestService {
            keep_alive,
            refuse: AtomicBool::new(false),
            held: Arc::new(AtomicUsize::new(0)),
            panics: AtomicUsize::new(0),
            panic_admit: AtomicBool::new(false),
            prove: AtomicBool::new(false),
        })
    }
}

struct Held(Arc<AtomicUsize>);
impl Drop for Held {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Relaxed);
    }
}

/// JS Object.entries order: keys that are array indexes first (ascending), then the others as they came
fn js_order(entries: Vec<(String, String)>) -> Vec<(String, String)> {
    let index = |k: &str| -> Option<u64> {
        if k.is_empty() || (k.len() > 1 && k.starts_with('0')) || !k.bytes().all(|b| b.is_ascii_digit()) {
            return None;
        }
        k.parse::<u64>().ok().filter(|&n| n < u64::from(u32::MAX))
    };
    let (mut ints, rest): (Vec<_>, Vec<_>) = entries.into_iter().partition(|(k, _)| index(k).is_some());
    ints.sort_by_key(|(k, _)| index(k));
    ints.extend(rest);
    ints
}

/// ref_server.mjs echo: JSON.stringify({ m, u, v, h: Object.entries(q.headers) })
pub fn echo(q: &Request) -> String {
    let h: Vec<Value> =
        js_order(q.headers.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect()).into_iter().map(|(k, v)| json!([k, v])).collect();
    let mut o = serde_json::Map::new();
    o.insert("m".into(), json!(q.method));
    o.insert("u".into(), json!(q.url));
    o.insert("v".into(), json!(q.http_version()));
    o.insert("h".into(), Value::Array(h));
    nfsu2_util::json::stringify(&Value::Object(o))
}

fn hs(list: &[(&str, &str)]) -> Vec<(String, String)> {
    list.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect()
}

/// ref_server.mjs plain
pub async fn plain(res: &mut Response, code: u16, text: &str, extra: &[(&str, &str)]) {
    let body = format!("{text}\n");
    let mut h = hs(&[("content-type", "text/plain; charset=utf-8")]);
    h.push(("content-length".into(), body.len().to_string()));
    h.extend(hs(extra));
    res.head(code, h);
    if !res.is_head() {
        let _ = res.write(body.as_bytes()).await;
    }
    let _ = res.end().await;
}

/// ref_server.mjs /body: the whole body, as its length and hex
async fn body_route(q: &mut Request, s: &mut Response) {
    match q.read_body(1 << 20).await {
        Ok(b) => {
            let hex: String = b.iter().map(|x| format!("{x:02x}")).collect();
            let body = format!("{{\"n\":{},\"hex\":\"{hex}\"}}", b.len());
            s.head(200, vec![("content-type".into(), "application/json".into()), ("content-length".into(), body.len().to_string())]);
            let _ = s.write(body.as_bytes()).await;
            let _ = s.end().await;
        }
        Err(_) => plain(s, 500, "Server error", &[]).await,
    }
}

async fn handler(svc: &TestService, mut q: Request, s: &mut Response) {
    let p = q.url.split('?').next().unwrap_or("").to_string();
    let simple = |code: u16, list: &[(&str, &str)]| (code, hs(list));
    match p.as_str() {
        "/chunked" => {
            s.head(200, hs(&[("content-type", "text/plain")]));
            let _ = s.write(b"hello").await;
            sleep(Duration::from_millis(20)).await;
            let _ = s.write(b" world").await;
            let _ = s.end().await;
        }
        "/empty" | "/204" | "/304" | "/204te" => {
            let (code, h) = match p.as_str() {
                "/empty" => simple(200, &[("x-a", "1")]),
                "/204" => simple(204, &[("x-a", "1")]),
                "/304" => simple(304, &[("etag", "\"e\"")]),
                _ => simple(204, &[("transfer-encoding", "chunked")]),
            };
            s.head(code, h);
            let _ = s.end().await;
        }
        "/te" => {
            s.head(200, hs(&[("transfer-encoding", "chunked")]));
            let _ = s.write(b"ab").await;
            let _ = s.end().await;
        }
        "/close" | "/keep" | "/date" => {
            let h = match p.as_str() {
                "/close" => hs(&[("content-length", "2"), ("connection", "close")]),
                "/keep" => hs(&[("content-length", "2"), ("connection", "keep-alive")]),
                _ => hs(&[("date", "yesterday"), ("content-length", "2")]),
            };
            s.head(200, h);
            let _ = s.write(b"ok").await;
            let _ = s.end().await;
        }
        "/ka" => {
            s.set_header("Keep-Alive", "timeout=7");
            s.head(200, hs(&[("content-length", "2"), ("x-b", "2"), ("keep-alive", "timeout=8")]));
            let _ = s.write(b"ok").await;
            let _ = s.end().await;
        }
        "/ka2" => {
            s.set_header("Keep-Alive", "timeout=7");
            s.head(200, hs(&[("content-length", "2")]));
            let _ = s.write(b"ok").await;
            let _ = s.end().await;
        }
        "/big" => {
            let b = vec![b'a'; 200_000];
            s.head(200, vec![("content-length".into(), b.len().to_string())]);
            let _ = s.write(&b).await;
            let _ = s.end().await;
        }
        "/bigchunked" => {
            s.head(200, vec![]);
            let _ = s.write(&vec![b'b'; 70_000]).await;
            let _ = s.write(&[b'c'; 10]).await;
            let _ = s.end().await;
        }
        "/slow" => {
            sleep(Duration::from_millis(150)).await;
            plain(s, 200, "slow", &[]).await;
        }
        // (not in ref_server.mjs) an answer that takes its time before its first byte
        "/wait" => {
            sleep(Duration::from_millis(2000)).await;
            plain(s, 200, "waited", &[]).await;
        }
        "/body" => {
            // (`for await` over the request is never synchronous in Node: the handler continues in a later tick)
            tokio::task::yield_now().await;
            body_route(&mut q, s).await;
        }
        // (not in ref_server.mjs) engine tests
        "/panic" => {
            svc.panics.fetch_add(1, Relaxed);
            panic!("a bug in one request");
        }
        "/destroy" => {
            s.head(200, hs(&[("content-length", "10")]));
            let _ = s.write(b"12345").await;
            s.destroy();
        }
        "/close-after" => {
            s.close_after();
            s.head(200, hs(&[("content-length", "2")]));
            let _ = s.write(b"ok").await;
            let _ = s.end().await;
        }
        "/noend" => {
            s.head(200, vec![]);
            let _ = s.write(b"abc").await;
        }
        "/after-end" => {
            s.head(200, hs(&[("content-length", "2")]));
            let _ = s.write(b"ok").await;
            let _ = s.end().await;
            // (a bug: Node's write after end() is an error, and nothing goes out)
            let w = s.write(b"XX").await;
            let _ = s.write(&[]).await;
            assert!(w.is_err(), "a write after end() went through");
        }
        "/nothing" => {}
        "/huge" | "/huge-late" => {
            // 16 MiB: more than a client that stops reading takes in its buffers; "late": its first byte after a
            // wait, as a file's answer starts once the file is open
            if p == "/huge-late" {
                sleep(Duration::from_millis(50)).await;
            }
            let n: usize = q.url.split('?').nth(1).and_then(|v| v.parse().ok()).unwrap_or(16 << 20);
            s.head(200, vec![("content-length".into(), n.to_string())]);
            let chunk = vec![b'z'; 64 << 10];
            let mut left = n;
            while left > 0 {
                let k = left.min(chunk.len());
                if s.write(&chunk[..k]).await.is_err() {
                    return;
                }
                left -= k;
            }
            let _ = s.end().await;
        }
        "/small" => match q.read_body(4).await {
            Ok(b) => plain(s, 200, &String::from_utf8_lossy(&b), &[]).await,
            Err(nfsu2_http::BodyError::TooLarge) => plain(s, 413, "Too large", &[("connection", "close")]).await,
            Err(nfsu2_http::BodyError::Broken) => plain(s, 500, "Broken", &[]).await,
        },
        "/implicit" => {
            let _ = s.write(b"abc").await;
            let _ = s.end().await;
        }
        "/implicit-end" => {
            let _ = s.end().await;
        }
        "/conninfo" => {
            let body = format!("{} {} {}", q.conn.peer, q.conn.encrypted, q.conn.peer_addr);
            s.head(200, vec![("content-length".into(), body.len().to_string())]);
            let _ = s.write(body.as_bytes()).await;
            let _ = s.end().await;
        }
        _ => {
            if let Some(c) = p.strip_prefix("/status/") {
                // JS `+c`, then writeHead's `statusCode |= 0`: outside 100-999 it throws, and the handler's catch
                // answers a plain 500
                let code = if c.is_empty() {
                    Some(0)
                } else if c.bytes().all(|b| b.is_ascii_digit()) {
                    c.parse::<u64>().ok()
                } else {
                    None
                };
                match code.filter(|n| (100..=999).contains(n)) {
                    Some(n) => {
                        s.head(n as u16, hs(&[("content-length", "0")]));
                        let _ = s.end().await;
                    }
                    None => plain(s, 500, "Server error", &[]).await,
                }
                return;
            }
            let body = echo(&q);
            s.head(200, vec![("content-type".into(), "application/json".into()), ("content-length".into(), body.len().to_string())]);
            if !s.is_head() {
                let _ = s.write(body.as_bytes()).await;
            }
            let _ = s.end().await;
        }
    }
}

impl Service for TestService {
    fn admit(&self, _peer: &SocketAddr) -> Option<Box<dyn Send + Sync>> {
        if self.panic_admit.swap(false, Relaxed) {
            panic!("a bug in admit");
        }
        if self.refuse.load(Relaxed) {
            return None;
        }
        self.held.fetch_add(1, Relaxed);
        Some(Box::new(Held(self.held.clone())))
    }
    fn client_certificate(&self, _peer: &SocketAddr) -> bool {
        self.prove.load(Relaxed)
    }
    fn keep_alive(&self, _peer: &SocketAddr) -> Duration {
        self.keep_alive
    }
    fn request<'a>(&'a self, req: Request, res: &'a mut Response) -> impl Future<Output = ()> + Send + 'a {
        handler(self, req, res)
    }
    async fn expectation(&self, _req: Request, res: &mut Response) {
        plain(res, 417, "Expectation failed", &[("connection", "close")]).await
    }
    async fn upgrade(&self, req: Request, up: Upgraded) {
        let hex: String = up.pre.iter().map(|x| format!("{x:02x}")).collect();
        let answer = format!("HTTP/1.1 101 Switching Protocols\r\nx-up: {}\r\nx-pre: {hex}\r\n\r\n", echo(&req));
        nfsu2_http::raw_refusal(up.stream, answer.into_bytes()).await;
    }
    fn client_error(&self, e: ClientError) -> Option<Vec<u8>> {
        Some(raw_response(match e {
            ClientError::Parse => 400,
            ClientError::HeaderOverflow => 431,
            ClientError::Timeout => 408,
        }))
    }
}

/// The engine with this service, on 127.0.0.1 (or `host`), port 0.
pub async fn start(limits: Limits, svc: Arc<TestService>) -> SocketAddr {
    start_on("127.0.0.1", limits, svc).await
}

pub async fn start_on(host: &str, limits: Limits, svc: Arc<TestService>) -> SocketAddr {
    let (l, _) = nfsu2_http::bind(host, 0, None).await.expect("bind");
    let addr = l.local_addr().expect("addr");
    nfsu2_http::serve(l, None, Arc::new(limits), svc);
    addr
}

/// Node's reference server (tests/node/ref_server.mjs) with these options, or None where there is no node.
pub struct NodeRef {
    pub addr: SocketAddr,
    _child: Child,
}

impl NodeRef {
    pub async fn start(options: Value) -> Option<NodeRef> {
        Self::script("ref_server.mjs", &[options.to_string()]).await
    }

    /// another of tests/node/'s scripts, with these arguments (it prints "listening <port>" too)
    pub async fn script(name: &str, args: &[String]) -> Option<NodeRef> {
        let script = format!("{}/tests/node/{name}", env!("CARGO_MANIFEST_DIR"));
        let mut child =
            Command::new("node").arg(script).args(args).stdout(Stdio::piped()).stderr(Stdio::null()).kill_on_drop(true).spawn().ok()?;
        let mut lines = BufReader::new(child.stdout.take()?).lines();
        let line = tokio::time::timeout(Duration::from_secs(10), lines.next_line()).await.ok()?.ok()??;
        let port: u16 = line.strip_prefix("listening ")?.trim().parse().ok()?;
        // (the rest of its output is not read: it prints nothing more)
        Some(NodeRef { addr: SocketAddr::from(([127, 0, 0, 1], port)), _child: child })
    }
}

/// One exchange on a fresh connection.
#[derive(Clone, Debug)]
pub struct Case {
    pub parts: Vec<Vec<u8>>,
    pub gap: Duration,
    /// the client ends its side after sending
    pub end: bool,
    /// how long the answer is waited for (the connection may stay open)
    pub wait: Duration,
}

impl Case {
    pub fn new(data: impl AsRef<[u8]>) -> Case {
        Case { parts: vec![data.as_ref().to_vec()], gap: Duration::ZERO, end: false, wait: Duration::from_millis(700) }
    }
}

/// What came back: the bytes (Date values as "X"), and whether the server closed the connection within the wait.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Got {
    pub text: String,
    pub closed: bool,
}

pub async fn exchange(addr: SocketAddr, case: &Case) -> Got {
    let mut s = match TcpStream::connect(addr).await {
        Ok(s) => s,
        Err(e) => return Got { text: format!("<connect {e}>"), closed: true },
    };
    let _ = s.set_nodelay(true);
    let deadline = Instant::now() + case.wait;
    let mut out = Vec::new();
    let mut closed = false;
    let (mut r, mut w) = s.split();
    let send = async {
        for (i, p) in case.parts.iter().enumerate() {
            if i > 0 && !case.gap.is_zero() {
                sleep(case.gap).await;
            }
            if w.write_all(p).await.is_err() {
                return;
            }
        }
        if case.end {
            let _ = w.shutdown().await;
        }
    };
    let recv = async {
        let mut buf = vec![0u8; 64 << 10];
        loop {
            match timeout_at(deadline, r.read(&mut buf)).await {
                Ok(Ok(0)) | Ok(Err(_)) => {
                    closed = true;
                    break;
                }
                Ok(Ok(n)) => out.extend_from_slice(&buf[..n]),
                Err(_) => break,
            }
        }
    };
    tokio::join!(send, recv);
    Got { text: mask_dates(&out), closed }
}

/// the bytes as latin1, Date header values as "X"
pub fn mask_dates(b: &[u8]) -> String {
    let s: String = b.iter().map(|&c| c as char).collect();
    let mut out = String::with_capacity(s.len());
    let mut rest = s.as_str();
    while let Some(i) = rest.find("\r\nDate: ") {
        out.push_str(&rest[..i + 8]);
        out.push('X');
        let after = &rest[i + 8..];
        let end = after.find("\r\n").unwrap_or(after.len());
        rest = &after[end..];
    }
    out.push_str(rest);
    out
}

/// a listener that is not the engine (for bind tests)
pub async fn taken_port() -> (TcpListener, u16) {
    let l = TcpListener::bind("127.0.0.1:0").await.expect("bind");
    let p = l.local_addr().expect("addr").port();
    (l, p)
}

/// reads until the server closes or `ms` pass
pub async fn read_all(s: &mut TcpStream, ms: u64) -> (Vec<u8>, bool) {
    let deadline = Instant::now() + Duration::from_millis(ms);
    let mut out = Vec::new();
    let mut buf = vec![0u8; 64 << 10];
    loop {
        match timeout_at(deadline, s.read(&mut buf)).await {
            Ok(Ok(0)) | Ok(Err(_)) => return (out, true),
            Ok(Ok(n)) => out.extend_from_slice(&buf[..n]),
            Err(_) => return (out, false),
        }
    }
}
