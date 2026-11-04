//! What the tests share: clients' frames built by hand, readers that hand their bytes out in pieces.
#![allow(dead_code)]

use std::pin::Pin;
use std::task::{Context, Poll};

use bytes::Bytes;
use nfsu2_ws::{Message, ReadError, WsReader};
use tokio::io::{AsyncRead, ReadBuf};

/// RFC 6455's example mask (5.7)
pub const MASK: [u8; 4] = [0x37, 0xfa, 0x21, 0x3d];

/// How a frame says its length.
#[derive(Clone, Copy)]
pub enum Len {
    /// the shortest form
    Short,
    /// 126 and 16 bits
    L16,
    /// 127 and 64 bits
    L64,
}

/// A client's frame: the first byte as given (FIN, RSV, opcode), masked with MASK, its length in the shortest form.
pub fn raw(b0: u8, payload: &[u8]) -> Vec<u8> {
    raw_with(b0, payload, Some(MASK), Len::Short)
}

pub fn raw_with(b0: u8, payload: &[u8], mask: Option<[u8; 4]>, len: Len) -> Vec<u8> {
    let mut v = vec![b0];
    v.extend(length(payload.len() as u64, mask.is_some(), len));
    match mask {
        Some(k) => {
            v.extend(k);
            v.extend(payload.iter().enumerate().map(|(i, b)| b ^ k[i % 4]));
        }
        None => v.extend(payload),
    }
    v
}

/// The second byte and the extended length for a payload of n bytes.
pub fn length(n: u64, masked: bool, len: Len) -> Vec<u8> {
    let m = if masked { 0x80 } else { 0 };
    match len {
        Len::Short if n < 126 => vec![m | n as u8],
        Len::Short | Len::L16 if n < 65536 => {
            let mut v = vec![m | 126];
            v.extend((n as u16).to_be_bytes());
            v
        }
        _ => {
            let mut v = vec![m | 127];
            v.extend(n.to_be_bytes());
            v
        }
    }
}

pub fn text(p: &[u8]) -> Vec<u8> {
    raw(0x81, p)
}
pub fn binary(p: &[u8]) -> Vec<u8> {
    raw(0x82, p)
}
pub fn ping(p: &[u8]) -> Vec<u8> {
    raw(0x89, p)
}
pub fn pong(p: &[u8]) -> Vec<u8> {
    raw(0x8a, p)
}
/// a continuation frame
pub fn cont(fin: bool, p: &[u8]) -> Vec<u8> {
    raw(if fin { 0x80 } else { 0x00 }, p)
}
/// a close frame with a code and a reason
pub fn close(code: u16, reason: &[u8]) -> Vec<u8> {
    let mut p = code.to_be_bytes().to_vec();
    p.extend(reason);
    raw(0x88, &p)
}

pub fn cat(frames: &[Vec<u8>]) -> Vec<u8> {
    frames.concat()
}

pub fn t(s: &str) -> Message {
    Message::Text(Bytes::copy_from_slice(s.as_bytes()))
}
pub fn b(p: &[u8]) -> Message {
    Message::Binary(Bytes::copy_from_slice(p))
}
pub fn closed(code: u16, reason: &str) -> Message {
    Message::Close(code, Bytes::copy_from_slice(reason.as_bytes()))
}
pub fn err(code: u16, reason: &'static str) -> ReadError {
    ReadError::Protocol { code, reason }
}

/// A reader that hands out its bytes `chunk` at a time (or in the sizes `sizes` gives, round and round).
pub struct Drip {
    data: Vec<u8>,
    at: usize,
    sizes: Vec<usize>,
    turn: usize,
}

impl Drip {
    pub fn new(data: &[u8], chunk: usize) -> Self {
        Drip { data: data.to_vec(), at: 0, sizes: vec![chunk.max(1)], turn: 0 }
    }
    pub fn sizes(data: &[u8], sizes: Vec<usize>) -> Self {
        Drip { data: data.to_vec(), at: 0, sizes, turn: 0 }
    }
}

impl AsyncRead for Drip {
    fn poll_read(mut self: Pin<&mut Self>, _: &mut Context<'_>, buf: &mut ReadBuf<'_>) -> Poll<std::io::Result<()>> {
        let size = self.sizes[self.turn % self.sizes.len()].max(1);
        self.turn += 1;
        let n = size.min(self.data.len() - self.at).min(buf.remaining());
        let at = self.at;
        buf.put_slice(&self.data[at..at + n]);
        self.at += n;
        Poll::Ready(Ok(()))
    }
}

/// A reader that fails after its bytes (a connection reset).
pub struct Failing(pub Vec<u8>);

impl AsyncRead for Failing {
    fn poll_read(mut self: Pin<&mut Self>, _: &mut Context<'_>, buf: &mut ReadBuf<'_>) -> Poll<std::io::Result<()>> {
        if self.0.is_empty() {
            return Poll::Ready(Err(std::io::Error::from(std::io::ErrorKind::ConnectionReset)));
        }
        let n = self.0.len().min(buf.remaining());
        buf.put_slice(&self.0[..n]);
        self.0.drain(..n);
        Poll::Ready(Ok(()))
    }
}

/// Every message a reader gives, and the error that ends them.
pub async fn read_all<R: AsyncRead + Unpin + Send>(mut r: WsReader<R>) -> (Vec<Message>, ReadError) {
    let mut out = Vec::new();
    loop {
        match r.next().await {
            Ok(m) => out.push(m),
            Err(e) => return (out, e),
        }
    }
}

/// The same bytes read four ways (all in `pre`, in one read, a byte a read, in pieces of 1 to 13 bytes), which must
/// agree.
pub async fn read_bytes(data: &[u8], max_payload: usize) -> (Vec<Message>, ReadError) {
    let whole = read_all(WsReader::new(tokio::io::empty(), Bytes::copy_from_slice(data), max_payload)).await;
    let one = read_all(WsReader::new(Drip::new(data, usize::MAX), Bytes::new(), max_payload)).await;
    assert_eq!(whole, one, "in pre vs in one read");
    let bytewise = read_all(WsReader::new(Drip::new(data, 1), Bytes::new(), max_payload)).await;
    assert_eq!(whole, bytewise, "in pre vs a byte a read");
    let pieces = read_all(WsReader::new(Drip::sizes(data, (1..=13).collect()), Bytes::new(), max_payload)).await;
    assert_eq!(whole, pieces, "in pre vs in pieces");
    whole
}

/// A small PRNG with a fixed seed (xorshift64*), so that random cases are the same every run.
pub struct Rng(pub u64);

impl Rng {
    pub fn next(&mut self) -> u64 {
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }
    /// 0..n
    pub fn below(&mut self, n: u64) -> u64 {
        self.next() % n.max(1)
    }
    /// true in pct out of 100
    pub fn chance(&mut self, pct: u64) -> bool {
        self.below(100) < pct
    }
    pub fn pick<'a, T>(&mut self, of: &'a [T]) -> &'a T {
        &of[self.below(of.len() as u64) as usize]
    }
    pub fn bytes(&mut self, n: usize) -> Vec<u8> {
        (0..n).map(|_| self.next() as u8).collect()
    }
}

pub fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}
