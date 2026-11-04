//! An answer as Node's `ServerResponse` writes it (`writeHead`, `_storeHeader`, `write_`, `end`): the service's
//! headers in its order with their names as given, then what Node adds (Date, Connection, Keep-Alive,
//! Transfer-Encoding), the body chunked where Node chunks it, nothing for HEAD, 1xx, 204 and 304.

use std::io::{self, IoSlice};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering::Relaxed};
use std::time::{Duration, Instant};

use tokio::io::AsyncWriteExt;

use crate::head::{has_token, push_date, push_latin1, reason, token};
use crate::{Response, WriteHalf};

/// The head (or a chunk's framing) and a body up to this much are copied into one buffer and written at once; a
/// larger body goes with them in one vectored write (one syscall either way when the kernel takes it all), uncopied.
const COALESCE: usize = 16 << 10;
/// the head buffer a new connection starts with (the server's heads, with its security headers: about 1.6 KiB)
const OUT: usize = 1024;
/// what an idle connection keeps of a head buffer a body written with its head grew
const IDLE_OUT: usize = 4096;

/// What the connection reads of an answer while the service writes it (the service holds the `Response`): bytes
/// on the wire, when, whether it is complete.  One per connection; only its own task touches it.
#[derive(Debug)]
pub(crate) struct Flow {
    epoch: Instant,
    bytes: AtomicU64,
    /// ms since `epoch` + 1 of the answer's first byte on the wire; 0: none yet
    t0: AtomicU64,
    /// ms since `epoch` of the last write
    last_write: AtomicU64,
    done: AtomicBool,
    /// complete, and the connection goes after it
    closing: AtomicBool,
    destroyed: AtomicBool,
}

impl Flow {
    pub fn new() -> Arc<Flow> {
        Arc::new(Flow {
            epoch: Instant::now(),
            bytes: AtomicU64::new(0),
            t0: AtomicU64::new(0),
            last_write: AtomicU64::new(0),
            done: AtomicBool::new(false),
            closing: AtomicBool::new(false),
            destroyed: AtomicBool::new(false),
        })
    }
    fn ms(&self) -> u64 {
        self.epoch.elapsed().as_millis() as u64
    }
    fn reset(&self) {
        self.bytes.store(0, Relaxed);
        self.t0.store(0, Relaxed);
        self.done.store(false, Relaxed);
        self.closing.store(false, Relaxed);
    }
    /// the answer is complete
    pub fn done(&self) -> bool {
        self.done.load(Relaxed)
    }
    /// the answer is complete and the connection goes after it
    pub fn closing(&self) -> bool {
        self.closing.load(Relaxed)
    }
    fn wrote(&self, n: usize) {
        let now = self.ms();
        self.bytes.fetch_add(n as u64, Relaxed);
        self.last_write.store(now, Relaxed);
        if self.t0.load(Relaxed) == 0 {
            self.t0.store(now + 1, Relaxed);
        }
    }
    /// a byte of the answer is on the wire
    pub fn wire(&self) -> bool {
        self.t0.load(Relaxed) != 0
    }
    /// the answer is under way (on the wire, not complete): since when
    pub fn under_way(&self) -> Option<Instant> {
        let t0 = self.t0.load(Relaxed);
        (t0 != 0 && !self.done.load(Relaxed)).then(|| self.epoch + Duration::from_millis(t0 - 1))
    }
    pub fn bytes(&self) -> u64 {
        self.bytes.load(Relaxed)
    }
    pub fn last_write(&self) -> Instant {
        self.epoch + Duration::from_millis(self.last_write.load(Relaxed))
    }
    pub fn destroyed(&self) -> bool {
        self.destroyed.load(Relaxed)
    }
    pub fn destroy(&self) {
        self.destroyed.store(true, Relaxed);
    }
}

/// What the answer needs of its request.
#[derive(Copy, Clone, Debug)]
pub(crate) struct Meta {
    pub head: bool,
    /// llhttp's should_keep_alive for the request
    pub keep_alive: bool,
    /// Node's useChunkedEncodingByDefault: HTTP/1.1, or an earlier request that says `TE: chunked`
    pub chunked_default: bool,
    /// what `Keep-Alive: timeout=` says (whole seconds, as Node floors them); None: no Keep-Alive header (Node with
    /// keepAliveTimeout 0)
    pub keep_alive_secs: Option<u64>,
}

#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub(crate) enum Phase {
    /// nothing given yet (set_header may be)
    Fresh,
    /// `head` was called
    Given,
    /// the head is composed (it goes out with the first bytes)
    Composed,
    Ended,
}

impl Response {
    pub(crate) fn new(io: WriteHalf, flow: Arc<Flow>) -> Response {
        Response {
            io: Some(io),
            out: Vec::with_capacity(OUT),
            flow,
            meta: Meta { head: false, keep_alive: true, chunked_default: true, keep_alive_secs: None },
            status: 200,
            pre: Vec::new(),
            given: Vec::new(),
            phase: Phase::Fresh,
            has_body: true,
            chunked: false,
            last: false,
            keep: true,
            implicit_len: None,
        }
    }

    /// a new answer, for this request
    pub(crate) fn begin(&mut self, meta: Meta) {
        self.meta = meta;
        self.status = 200;
        self.pre.clear();
        self.given.clear();
        self.phase = Phase::Fresh;
        self.has_body = !meta.head;
        self.chunked = false;
        self.last = false;
        self.keep = meta.keep_alive;
        self.implicit_len = None;
        self.out.clear();
        // (a long head left it large: an idle connection keeps no more than it needs)
        if self.out.capacity() > 2 * COALESCE {
            self.out = Vec::with_capacity(OUT);
        }
        self.flow.reset();
    }

    /// Between answers, the connection waiting for its next request: a head buffer that a body written with its head
    /// grew (up to COALESCE, a small file's answer) is let go, so that an idle keep-alive connection holds what a
    /// head needs, not the last small file.
    pub(crate) fn idle(&mut self) {
        if self.out.is_empty() && self.out.capacity() > IDLE_OUT {
            self.out = Vec::with_capacity(OUT);
        }
    }

    pub(crate) fn take_io(&mut self) -> Option<WriteHalf> {
        self.io.take()
    }
    pub(crate) fn ended(&self) -> bool {
        self.phase == Phase::Ended
    }
    /// `head` was given (an answer was begun)
    pub(crate) fn given(&self) -> bool {
        self.phase != Phase::Fresh
    }
    /// the connection goes after this answer (Node's `_last`)
    pub(crate) fn last(&self) -> bool {
        self.last
    }

    /// bytes that are not part of the answer (`100 Continue`)
    pub(crate) async fn raw(&mut self, bytes: &[u8]) -> io::Result<()> {
        let io = self.io.as_mut().ok_or_else(gone)?;
        io.write_all(bytes).await?;
        io.flush().await
    }

    pub fn set_header(&mut self, name: &str, value: &str) {
        if self.phase != Phase::Fresh {
            // Node throws (ERR_HTTP_HEADERS_SENT): nothing changes
            return;
        }
        set(&mut self.pre, name, value);
    }

    pub fn head(&mut self, status: u16, headers: Vec<(String, String)>) {
        if self.phase != Phase::Fresh {
            return;
        }
        self.status = status;
        self.given = headers;
        self.phase = Phase::Given;
    }

    pub async fn write(&mut self, data: &[u8]) -> io::Result<()> {
        self.check()?;
        if self.phase == Phase::Ended {
            // Node's ERR_STREAM_WRITE_AFTER_END: nothing goes out (it would, after the answer, where the client reads
            // the next one's head)
            return Err(io::Error::other("write after end"));
        }
        if self.phase == Phase::Fresh {
            // Node's implicit header: the status and the headers set so far, chunked
            self.phase = Phase::Given;
        }
        if self.phase == Phase::Given {
            self.compose();
        }
        if !self.has_body {
            // Node ignores body writes here, and sends the head with end()
            return Ok(());
        }
        if data.is_empty() {
            return if self.out.is_empty() { Ok(()) } else { self.send(&[], &[]).await };
        }
        if self.chunked {
            let mut b = itoa_hex(data.len());
            b.extend_from_slice(b"\r\n");
            self.out.extend_from_slice(&b);
            self.send(data, b"\r\n").await
        } else {
            self.send(data, &[]).await
        }
    }

    pub async fn end(&mut self) -> io::Result<()> {
        if self.phase == Phase::Ended {
            return Ok(());
        }
        self.check()?;
        if self.phase == Phase::Fresh {
            // Node's end() without a head: Content-Length 0 (where it chunks by default)
            self.implicit_len = Some(0);
            self.phase = Phase::Given;
        }
        if self.phase == Phase::Given {
            self.compose();
        }
        if self.has_body && self.chunked {
            self.out.extend_from_slice(b"0\r\n\r\n");
        }
        if !self.out.is_empty() {
            self.send(&[], &[]).await?;
        }
        self.phase = Phase::Ended;
        self.flow.closing.store(self.last, Relaxed);
        self.flow.done.store(true, Relaxed);
        Ok(())
    }

    pub fn head_sent(&self) -> bool {
        self.phase != Phase::Fresh
    }
    pub fn status(&self) -> u16 {
        self.status
    }
    pub fn is_head(&self) -> bool {
        self.meta.head
    }
    pub fn destroy(&mut self) {
        self.flow.destroy();
    }
    pub fn close_after(&mut self) {
        if matches!(self.phase, Phase::Fresh | Phase::Given) {
            // as Node's `shouldKeepAlive = false`: the head says close
            self.keep = false;
        }
        self.last = true;
        if self.phase == Phase::Ended {
            self.flow.closing.store(true, Relaxed);
        }
    }

    fn check(&self) -> io::Result<()> {
        if self.flow.destroyed() || self.io.is_none() {
            return Err(gone());
        }
        Ok(())
    }

    /// Node's `_storeHeader`: the head, into `out`.
    fn compose(&mut self) {
        self.phase = Phase::Composed;
        // (Node throws for a code outside 100-999; the server's 500 path answers then)
        let status = if (100..=999).contains(&self.status) { self.status } else { 500 };
        // RFC 9110: 1xx, 204 and 304 have no body
        if status < 200 || status == 204 || status == 304 {
            self.has_body = false;
        }
        let out = &mut self.out;
        out.extend_from_slice(b"HTTP/1.1 ");
        let mut num = itoa::Buffer::new();
        out.extend_from_slice(num.format(status).as_bytes());
        out.push(b' ');
        out.extend_from_slice(reason(status).as_bytes());
        out.extend_from_slice(b"\r\n");
        let mut seen = Seen::default();
        // with set_header, writeHead merges into those (a name given again replaces the value where it was)
        let list: Vec<(String, String)> = if self.pre.is_empty() {
            std::mem::take(&mut self.given)
        } else {
            let mut l = std::mem::take(&mut self.pre);
            for (k, v) in std::mem::take(&mut self.given) {
                set(&mut l, &k, &v);
            }
            l
        };
        for (k, v) in &list {
            // Node throws for these; here they are left out (never a broken head)
            if !token(k) {
                continue;
            }
            let start = out.len();
            out.extend_from_slice(k.as_bytes());
            out.extend_from_slice(b": ");
            if !push_latin1(out, v) {
                out.truncate(start);
                continue;
            }
            out.extend_from_slice(b"\r\n");
            seen.see(k, v, &mut self.last, &mut self.keep, &mut self.chunked);
        }
        if !seen.date {
            out.extend_from_slice(b"Date: ");
            push_date(out);
            out.extend_from_slice(b"\r\n");
        }
        // a 204 or 304 with Transfer-Encoding: chunked: no zero chunk, and the connection goes (Node)
        if self.chunked && (status == 204 || status == 304) {
            self.chunked = false;
            self.keep = false;
        }
        if !seen.connection {
            let keep = self.keep && (seen.content_length || self.meta.chunked_default);
            if keep {
                out.extend_from_slice(b"Connection: keep-alive\r\n");
                if let (Some(secs), false) = (self.meta.keep_alive_secs, seen.keep_alive) {
                    out.extend_from_slice(b"Keep-Alive: timeout=");
                    out.extend_from_slice(num.format(secs).as_bytes());
                    out.extend_from_slice(b"\r\n");
                }
            } else {
                self.last = true;
                out.extend_from_slice(b"Connection: close\r\n");
            }
        }
        if !seen.content_length && !seen.te {
            if !self.has_body {
                self.chunked = false;
            } else if !self.meta.chunked_default {
                // HTTP/1.0: the body ends with the connection
                self.last = true;
            } else if let Some(n) = self.implicit_len {
                out.extend_from_slice(b"Content-Length: ");
                out.extend_from_slice(num.format(n).as_bytes());
                out.extend_from_slice(b"\r\n");
            } else {
                out.extend_from_slice(b"Transfer-Encoding: chunked\r\n");
                self.chunked = true;
            }
        }
        out.extend_from_slice(b"\r\n");
    }

    /// `out` (head or framing), then `data`, then `tail`: one write when small, else one vectored write
    async fn send(&mut self, data: &[u8], tail: &[u8]) -> io::Result<()> {
        let io = self.io.as_mut().ok_or_else(gone)?;
        let r = if self.out.is_empty() && tail.is_empty() {
            // body bytes alone (a file, a compressor's output): as they are
            let r = io.write_all(data).await;
            if r.is_ok() {
                self.flow.wrote(data.len());
            }
            r
        } else if self.out.len() + data.len() + tail.len() <= COALESCE {
            self.out.extend_from_slice(data);
            self.out.extend_from_slice(tail);
            let r = io.write_all(&self.out).await;
            if r.is_ok() {
                self.flow.wrote(self.out.len());
            }
            r
        } else {
            let mut bufs = [IoSlice::new(&self.out), IoSlice::new(data), IoSlice::new(tail)];
            write_all_vectored(io, &mut bufs, &self.flow).await
        };
        self.out.clear();
        if let Err(e) = r {
            self.flow.destroy();
            return Err(e);
        }
        // (TLS: the records rustls holds go out now)
        if let Err(e) = io.flush().await {
            self.flow.destroy();
            return Err(e);
        }
        Ok(())
    }
}

/// Node's matchHeader: what the service's own headers say to the rest of the head.
#[derive(Default)]
struct Seen {
    connection: bool,
    content_length: bool,
    te: bool,
    date: bool,
    keep_alive: bool,
}

impl Seen {
    fn see(&mut self, k: &str, v: &str, last: &mut bool, keep: &mut bool, chunked: &mut bool) {
        if k.len() < 4 || k.len() > 17 {
            return;
        }
        let is = |s: &str| k.eq_ignore_ascii_case(s);
        if is("connection") {
            self.connection = true;
            if has_token(v, "close") {
                *last = true;
            } else {
                *keep = true;
            }
        } else if is("transfer-encoding") {
            self.te = true;
            if has_token(v, "chunked") {
                *chunked = true;
            }
        } else if is("content-length") {
            self.content_length = true;
        } else if is("date") {
            self.date = true;
        } else if is("keep-alive") {
            self.keep_alive = true;
        }
    }
}

/// `setHeader`: replaces a header of that name (any case) where it is, else adds it
fn set(list: &mut Vec<(String, String)>, name: &str, value: &str) {
    match list.iter_mut().find(|(k, _)| k.eq_ignore_ascii_case(name)) {
        Some(e) => *e = (name.to_string(), value.to_string()),
        None => list.push((name.to_string(), value.to_string())),
    }
}

/// a chunk size as Node writes it (lower-case hex)
fn itoa_hex(n: usize) -> Vec<u8> {
    format!("{n:x}").into_bytes()
}

fn gone() -> io::Error {
    io::Error::new(io::ErrorKind::NotConnected, "the connection is gone")
}

async fn write_all_vectored(io: &mut WriteHalf, mut bufs: &mut [IoSlice<'_>], flow: &Flow) -> io::Result<()> {
    // drop empty slices first: a write of only empty slices returns 0
    IoSlice::advance_slices(&mut bufs, 0);
    while !bufs.is_empty() {
        let n = io.write_vectored(bufs).await?;
        if n == 0 {
            return Err(io::ErrorKind::WriteZero.into());
        }
        flow.wrote(n);
        IoSlice::advance_slices(&mut bufs, n);
    }
    Ok(())
}
