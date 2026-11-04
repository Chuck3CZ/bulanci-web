//! Reading a client's frames: ws's Receiver (receiver.js) as an incremental parser over one read buffer.

use bytes::{Buf, Bytes, BytesMut};
use tokio::io::{AsyncRead, AsyncReadExt};

use crate::{Message, ReadError};

/// A read block: what one read takes at most when no larger frame is announced.
const BLOCK: usize = 16 << 10;
/// A read with less room than this left in the block starts another (or reclaims the old one once its payloads are
/// all dropped): reads of a few bytes would cost a system call each.
const MIN_READ: usize = 1 << 10;
/// Without a max_payload (0) a frame's buffer grows by this much a read at most (or by what it holds, past it), so
/// that what a client only announces is never reserved before it comes.
const STEP: usize = 1 << 20;

// ws's error messages (receiver.js createError: "Invalid WebSocket frame: " before the ones made with a prefix)
macro_rules! invalid {
    ($s:expr) => {
        concat!("Invalid WebSocket frame: ", $s)
    };
}
const RSV_2_3: &str = invalid!("RSV2 and RSV3 must be clear");
const RSV_1: &str = invalid!("RSV1 must be clear");
const EXPECTED_FIN: &str = invalid!("FIN must be set");
const EXPECTED_MASK: &str = invalid!("MASK must be set");
const INVALID_UTF8: &str = invalid!("invalid UTF-8 sequence");
const INVALID_CLOSE_CODE: &str = invalid!("invalid status code");
const MAX_PAYLOAD: &str = "Max payload size exceeded";
const OVER_2_53: &str = "Unsupported WebSocket frame: payload length > 2^53 - 1";
/// "invalid opcode N" by opcode (8, 9 and 10 are never refused for their opcode)
const INVALID_OPCODE: [&str; 16] = [
    invalid!("invalid opcode 0"),
    invalid!("invalid opcode 1"),
    invalid!("invalid opcode 2"),
    invalid!("invalid opcode 3"),
    invalid!("invalid opcode 4"),
    invalid!("invalid opcode 5"),
    invalid!("invalid opcode 6"),
    invalid!("invalid opcode 7"),
    invalid!("invalid opcode 8"),
    invalid!("invalid opcode 9"),
    invalid!("invalid opcode 10"),
    invalid!("invalid opcode 11"),
    invalid!("invalid opcode 12"),
    invalid!("invalid opcode 13"),
    invalid!("invalid opcode 14"),
    invalid!("invalid opcode 15"),
];

fn protocol(code: u16, reason: &'static str) -> ReadError {
    ReadError::Protocol { code, reason }
}

/// "invalid payload length N" for a control frame's 7-bit length: 1 (a close frame's), 126 or 127
fn invalid_control_length(len7: u8) -> ReadError {
    protocol(
        1002,
        match len7 {
            1 => invalid!("invalid payload length 1"),
            126 => invalid!("invalid payload length 126"),
            _ => invalid!("invalid payload length 127"),
        },
    )
}

/// validation.js isValidStatusCode: the codes a close frame may carry
fn valid_close_code(code: u16) -> bool {
    ((1000..=1014).contains(&code) && !(1004..=1006).contains(&code)) || (3000..=4999).contains(&code)
}

/// Messages from a client, read from the connection's read half; `pre`: what the HTTP server read past the
/// handshake's head (already this connection's bytes).
///
/// Reads go into blocks of 16 KiB, or one as large as a frame when a larger frame is announced (so never more than
/// `max_payload` + 14 bytes for one), and a single-frame message is its frame's own bytes in the block, unmasked in
/// place: no copy.  A payload keeps its block alive until it is dropped: copy what is kept for long.  A message in
/// fragments is joined (copied once, as ws's concat() did).
///
/// `next()` is cancel safe: a `select!` with a timer may drop it, nothing read is lost.  After a close frame or a
/// protocol error nothing the client sends counts (ws's receiver stopped): `next()` reads and drops it until the
/// connection ends, then says Closed.
pub struct WsReader<R> {
    r: R,
    buf: BytesMut,
    /// 0: no limit (ws's maxPayload 0)
    max_payload: u64,
    /// the opcode of the message whose fragments are coming (ws's `_fragmented`), 0 when none
    fragmented: u8,
    /// that message's payload bytes so far (ws's `_totalPayloadLength`): what max_payload counts
    total: u64,
    parts: Parts,
    /// a close frame came, the client broke the protocol, or the connection ended: no more messages
    done: bool,
    /// the connection ended
    ended: bool,
}

/// A frame's header, read and checked.
struct Header {
    fin: bool,
    /// the message's opcode for a continuation frame
    opcode: u8,
    /// the header's length (2 to 14 bytes)
    head: usize,
    len: usize,
    mask: [u8; 4],
}

enum Step {
    /// the buffer must hold this many bytes before the frame can be read further
    Need(usize),
    Frame(Header),
}

impl<R: AsyncRead + Unpin + Send> WsReader<R> {
    pub fn new(r: R, pre: Bytes, max_payload: usize) -> Self {
        let mut buf = BytesMut::with_capacity(BLOCK.max(pre.len()));
        buf.extend_from_slice(&pre);
        WsReader { r, buf, max_payload: max_payload as u64, fragmented: 0, total: 0, parts: Parts::None, done: false, ended: false }
    }

    /// the next message (a payload is the frame's own bytes, unmasked in place: no copy)
    pub async fn next(&mut self) -> Result<Message, ReadError> {
        if self.done {
            return self.drain().await;
        }
        loop {
            let want = match self.header() {
                Err(e) => return Err(self.stop(e)),
                Ok(Step::Need(n)) => n,
                Ok(Step::Frame(h)) if self.buf.len() >= h.head + h.len => match self.take(h) {
                    Ok(Some(m)) => return Ok(m),
                    Ok(None) => continue,
                    Err(e) => return Err(self.stop(e)),
                },
                Ok(Step::Frame(h)) => h.head + h.len,
            };
            self.fill(want).await?;
        }
    }

    /// The frame at the front of the buffer, as far as it is there: receiver.js getInfo, getPayloadLength16/64,
    /// haveLength and getMask, in their order (so that a frame broken twice is refused for what ws saw first, and
    /// as soon as ws would: a message over max_payload before its payload comes).
    fn header(&self) -> Result<Step, ReadError> {
        let b = &self.buf[..];
        let &[b0, b1, ..] = b else { return Ok(Step::Need(2)) };
        if b0 & 0x30 != 0 {
            return Err(protocol(1002, RSV_2_3));
        }
        // RSV1 is permessage-deflate's, which the server never accepts
        if b0 & 0x40 != 0 {
            return Err(protocol(1002, RSV_1));
        }
        let fin = b0 & 0x80 != 0;
        let mut opcode = b0 & 0x0f;
        let len7 = b1 & 0x7f;
        match opcode {
            0 => {
                if self.fragmented == 0 {
                    return Err(protocol(1002, INVALID_OPCODE[0]));
                }
                opcode = self.fragmented;
            }
            1 | 2 => {
                if self.fragmented != 0 {
                    return Err(protocol(1002, INVALID_OPCODE[opcode as usize]));
                }
            }
            8..=10 => {
                if !fin {
                    return Err(protocol(1002, EXPECTED_FIN));
                }
                if len7 > 125 || (opcode == 8 && len7 == 1) {
                    return Err(invalid_control_length(len7));
                }
            }
            _ => return Err(protocol(1002, INVALID_OPCODE[opcode as usize])),
        }
        // a client's frames are masked (RFC 6455 5.1)
        if b1 & 0x80 == 0 {
            return Err(protocol(1002, EXPECTED_MASK));
        }
        let (len, at) = match len7 {
            126 => {
                let Some(&[hi, lo]) = b.get(2..4) else { return Ok(Step::Need(4)) };
                (u16::from_be_bytes([hi, lo]) as u64, 4)
            }
            127 => {
                let Some(x) = b.get(2..10).and_then(|x| <[u8; 8]>::try_from(x).ok()) else { return Ok(Step::Need(10)) };
                let v = u64::from_be_bytes(x);
                // ws counted in JS numbers: more than 2^53 - 1 it could not
                if v >> 32 > (1 << 21) - 1 {
                    return Err(protocol(1009, OVER_2_53));
                }
                (v, 10)
            }
            n => (n as u64, 2),
        };
        if len > 0 && opcode < 8 && self.max_payload > 0 && self.total.saturating_add(len) > self.max_payload {
            return Err(protocol(1009, MAX_PAYLOAD));
        }
        let Some(&[m0, m1, m2, m3]) = b.get(at..at + 4) else { return Ok(Step::Need(at + 4)) };
        // within max_payload, a usize; only a 32-bit build without a limit could see more
        let len = usize::try_from(len).map_err(|_| protocol(1009, OVER_2_53))?;
        Ok(Step::Frame(Header { fin, opcode, head: at + 4, len, mask: [m0, m1, m2, m3] }))
    }

    /// The frame (all in the buffer) taken off it: a message, or None for a fragment that is not the last
    /// (receiver.js getData, controlMessage, dataMessage).
    fn take(&mut self, h: Header) -> Result<Option<Message>, ReadError> {
        self.buf.advance(h.head);
        let data = if h.len == 0 {
            Bytes::new()
        } else {
            let mut p = self.buf.split_to(h.len);
            unmask(&mut p, h.mask);
            p.freeze()
        };
        match h.opcode {
            8 => {
                self.end();
                close(data).map(Some)
            }
            9 => Ok(Some(Message::Ping(data))),
            10 => Ok(Some(Message::Pong(data))),
            op => {
                self.total = self.total.saturating_add(h.len as u64);
                self.parts.push(data);
                if !h.fin {
                    self.fragmented = op;
                    return Ok(None);
                }
                self.fragmented = 0;
                self.total = 0;
                let data = self.parts.take();
                if op == 2 {
                    return Ok(Some(Message::Binary(data)));
                }
                // the whole message is checked, as ws did: a character may be split across fragments
                if std::str::from_utf8(&data).is_err() {
                    return Err(protocol(1007, INVALID_UTF8));
                }
                Ok(Some(Message::Text(data)))
            }
        }
    }

    /// No more messages: what was put together is let go.
    fn end(&mut self) {
        self.done = true;
        self.parts = Parts::None;
    }

    fn stop(&mut self, e: ReadError) -> ReadError {
        self.end();
        e
    }

    /// More bytes for a step that needs `want` in the buffer.
    async fn fill(&mut self, want: usize) -> Result<(), ReadError> {
        // past STEP the buffer at most doubles a read: what is reserved follows what came
        let room = want.saturating_sub(self.buf.len()).clamp(MIN_READ, STEP.max(self.buf.len()));
        self.make_room(room);
        match self.r.read_buf(&mut self.buf).await {
            Ok(n) if n > 0 => Ok(()),
            _ => {
                self.ended = true;
                Err(self.stop(ReadError::Closed))
            }
        }
    }

    /// Room for `room` more bytes: the block's own, the block reclaimed once no payload holds it, or a new block
    /// with the bytes not yet read (at most a frame).
    fn make_room(&mut self, room: usize) {
        if self.buf.try_reclaim(room) {
            return;
        }
        let mut b = BytesMut::with_capacity((self.buf.len() + room).max(BLOCK));
        b.extend_from_slice(&self.buf);
        self.buf = b;
    }

    /// What comes after the end is dropped until the connection ends (ws resumed the socket and ignored it).
    async fn drain(&mut self) -> Result<Message, ReadError> {
        while !self.ended {
            self.buf.clear();
            self.make_room(MIN_READ);
            match self.r.read_buf(&mut self.buf).await {
                Ok(n) if n > 0 => {}
                _ => self.ended = true,
            }
        }
        self.buf = BytesMut::new();
        Err(ReadError::Closed)
    }
}

/// A close frame's payload: Close(code, reason), or what ws refused in it.
fn close(data: Bytes) -> Result<Message, ReadError> {
    if data.is_empty() {
        return Ok(Message::Close(1005, Bytes::new()));
    }
    // a payload of 1 byte was refused with the header
    let &[hi, lo, ..] = &data[..] else { return Err(invalid_control_length(1)) };
    let code = u16::from_be_bytes([hi, lo]);
    if !valid_close_code(code) {
        return Err(protocol(1002, INVALID_CLOSE_CODE));
    }
    let reason = data.slice(2..);
    if std::str::from_utf8(&reason).is_err() {
        return Err(protocol(1007, INVALID_UTF8));
    }
    Ok(Message::Close(code, reason))
}

/// The fragments of a message so far: the first one as it is (a message of one non-empty fragment is not copied),
/// the others joined to it.  Empty fragments are skipped, as ws did.
enum Parts {
    None,
    One(Bytes),
    Many(BytesMut),
}

impl Parts {
    fn push(&mut self, data: Bytes) {
        if data.is_empty() {
            return;
        }
        *self = match std::mem::replace(self, Parts::None) {
            Parts::None => Parts::One(data),
            Parts::One(first) => {
                let mut m = BytesMut::with_capacity(first.len() + data.len());
                m.extend_from_slice(&first);
                m.extend_from_slice(&data);
                Parts::Many(m)
            }
            Parts::Many(mut m) => {
                m.extend_from_slice(&data);
                Parts::Many(m)
            }
        };
    }

    fn take(&mut self) -> Bytes {
        match std::mem::replace(self, Parts::None) {
            Parts::None => Bytes::new(),
            Parts::One(b) => b,
            Parts::Many(m) => m.freeze(),
        }
    }
}

/// XOR with the mask, eight bytes at a time (a mask of zeros changes nothing: ws skipped it too).
fn unmask(data: &mut [u8], mask: [u8; 4]) {
    if mask == [0; 4] {
        return;
    }
    let [a, b, c, d] = mask;
    let m = u64::from_ne_bytes([a, b, c, d, a, b, c, d]);
    // chunks_exact is what makes it vectorize (twice as fast as chunks); not as_chunks_mut, which the workspace's
    // Rust 1.85 does not have
    #[allow(unknown_lints, clippy::chunks_exact_to_as_chunks)]
    let mut words = data.chunks_exact_mut(8);
    for w in &mut words {
        if let Ok(x) = <[u8; 8]>::try_from(&*w) {
            w.copy_from_slice(&(u64::from_ne_bytes(x) ^ m).to_ne_bytes());
        }
    }
    // the rest starts at a multiple of 8: the mask's first byte again
    for (i, x) in words.into_remainder().iter_mut().enumerate() {
        *x ^= mask[i & 3];
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unmask_matches_bytewise() {
        let mask = [0x37, 0xfa, 0x21, 0x3d];
        for n in 0..40 {
            let orig: Vec<u8> = (0..n as u8).map(|i| i.wrapping_mul(31)).collect();
            let mut fast = orig.clone();
            unmask(&mut fast, mask);
            let slow: Vec<u8> = orig.iter().enumerate().map(|(i, b)| b ^ mask[i % 4]).collect();
            assert_eq!(fast, slow, "length {n}");
        }
    }

    #[test]
    #[allow(clippy::manual_range_contains)]
    fn close_codes() {
        // validation.js isValidStatusCode, as it is written there
        for code in 0..=u16::MAX {
            let ws = (code >= 1000 && code <= 1014 && code != 1004 && code != 1005 && code != 1006) || (code >= 3000 && code <= 4999);
            assert_eq!(valid_close_code(code), ws, "{code}");
        }
    }
}
