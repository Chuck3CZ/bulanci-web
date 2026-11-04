//! An upgraded connection (the /lan WebSocket) still counts while whoever took it keeps it: in the listener's count
//! (max_connections) and in the per-client connection budget (Service::admit's share), as Node counted an upgraded
//! socket until its 'close'.  (Before, both were given back as soon as the engine handed the stream on, so a crowd of
//! WebSockets left the port's limits free for as many more connections.)

use std::net::SocketAddr;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Duration;

use nfsu2_http::{ClientError, Limits, Request, Response, Service, Upgraded};
use std::sync::Mutex;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

/// a service that keeps every upgraded connection, and counts its admit shares
#[derive(Default)]
struct Keeper {
    kept: Mutex<Vec<Upgraded>>,
    shares: Arc<AtomicUsize>,
}

struct Share(Arc<AtomicUsize>);
impl Drop for Share {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::SeqCst);
    }
}

impl Service for Keeper {
    fn admit(&self, _: &SocketAddr) -> Option<Box<dyn Send + Sync>> {
        self.shares.fetch_add(1, Ordering::SeqCst);
        Some(Box::new(Share(self.shares.clone())))
    }
    fn keep_alive(&self, _: &SocketAddr) -> Duration {
        Duration::from_secs(5)
    }
    async fn request<'a>(&'a self, _req: Request, res: &'a mut Response) {
        res.head(200, vec![("content-length".into(), "2".into())]);
        let _ = res.write(b"ok").await;
        let _ = res.end().await;
    }
    async fn expectation<'a>(&'a self, _req: Request, _res: &'a mut Response) {}
    async fn upgrade(&self, _req: Request, mut up: Upgraded) {
        let _ = up.stream.write_all(b"HTTP/1.1 101 Switching Protocols\r\n\r\n").await;
        self.kept.lock().unwrap().push(up);
    }
    fn client_error(&self, _: ClientError) -> Option<Vec<u8>> {
        None
    }
}

async fn upgrade(addr: SocketAddr) -> TcpStream {
    let mut c = TcpStream::connect(addr).await.expect("connect");
    c.write_all(b"GET /lan HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n").await.expect("write");
    let mut b = [0u8; 64];
    let n = c.read(&mut b).await.expect("the 101");
    assert!(b[..n].starts_with(b"HTTP/1.1 101"), "{:?}", String::from_utf8_lossy(&b[..n]));
    c
}

/// whether a new connection is let in: an answer to a request, or closed without a byte
async fn let_in(addr: SocketAddr) -> bool {
    let mut c = TcpStream::connect(addr).await.expect("connect");
    let _ = c.write_all(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n").await;
    let mut b = [0u8; 256];
    matches!(tokio::time::timeout(Duration::from_secs(2), c.read(&mut b)).await, Ok(Ok(n)) if n > 0)
}

#[tokio::test]
async fn an_upgraded_connection_counts_while_it_is_kept() {
    let (listener, _) = nfsu2_http::bind("127.0.0.1", 0, None).await.expect("bind");
    let addr = listener.local_addr().expect("addr");
    let svc = Arc::new(Keeper::default());
    let limits = Limits { max_connections: 2, ..Limits::default() };
    nfsu2_http::serve(listener, None, Arc::new(limits), svc.clone());
    let a = upgrade(addr).await;
    let b = upgrade(addr).await;
    tokio::time::sleep(Duration::from_millis(100)).await;
    assert_eq!(svc.kept.lock().unwrap().len(), 2);
    assert_eq!(svc.shares.load(Ordering::SeqCst), 2, "both upgraded connections keep their admit share");
    assert!(!let_in(addr).await, "two upgraded connections fill a port of max_connections 2");
    // the service lets them go: the port has room again, the shares are back
    svc.kept.lock().unwrap().clear();
    drop((a, b));
    tokio::time::sleep(Duration::from_millis(100)).await;
    assert!(let_in(addr).await, "room again once they are gone");
}

/// a service that lets every upgraded connection go at once (a refusal), and panics on an upgrade to /panic
#[derive(Default)]
struct Dropper {
    shares: Arc<AtomicUsize>,
}

impl Service for Dropper {
    fn admit(&self, _: &SocketAddr) -> Option<Box<dyn Send + Sync>> {
        self.shares.fetch_add(1, Ordering::SeqCst);
        Some(Box::new(Share(self.shares.clone())))
    }
    fn keep_alive(&self, _: &SocketAddr) -> Duration {
        Duration::from_secs(5)
    }
    async fn request<'a>(&'a self, _req: Request, res: &'a mut Response) {
        res.head(200, vec![("content-length".into(), "2".into())]);
        let _ = res.write(b"ok").await;
        let _ = res.end().await;
    }
    async fn expectation<'a>(&'a self, _req: Request, _res: &'a mut Response) {}
    async fn upgrade(&self, req: Request, up: Upgraded) {
        if req.url == "/panic" {
            panic!("an upgrade that fails");
        }
        drop(up);
    }
    fn client_error(&self, _: ClientError) -> Option<Vec<u8>> {
        None
    }
}

async fn shares_back(shares: &AtomicUsize, what: &str) {
    let t0 = std::time::Instant::now();
    while shares.load(Ordering::SeqCst) != 0 {
        assert!(t0.elapsed() < Duration::from_secs(2), "{what}: shares still held");
        tokio::time::sleep(Duration::from_millis(5)).await;
    }
}

// Every connection that no upgrade keeps gives its shares back as it goes: one that was answered, an upgrade the
// service let go (its refusals), an upgrade whose service panicked.  (A port of max_connections 1: each in turn.)
#[tokio::test]
async fn any_other_connection_gives_its_shares_back_as_it_goes() {
    let (listener, _) = nfsu2_http::bind("127.0.0.1", 0, None).await.expect("bind");
    let addr = listener.local_addr().expect("addr");
    let svc = Arc::new(Dropper::default());
    nfsu2_http::serve(listener, None, Arc::new(Limits { max_connections: 1, ..Limits::default() }), svc.clone());
    assert!(let_in(addr).await, "a request answered");
    shares_back(&svc.shares, "an answered connection that the client closed").await;
    for path in ["/lan", "/panic"] {
        let mut c = TcpStream::connect(addr).await.expect("connect");
        let req = format!("GET {path} HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n");
        c.write_all(req.as_bytes()).await.expect("write");
        let mut b = [0u8; 64];
        let n = tokio::time::timeout(Duration::from_secs(2), c.read(&mut b)).await.expect("closed in time").unwrap_or(0);
        assert_eq!(n, 0, "{path}: closed without an answer");
        shares_back(&svc.shares, path).await;
        assert!(let_in(addr).await, "{path}: the port has room again");
        shares_back(&svc.shares, path).await;
    }
}
