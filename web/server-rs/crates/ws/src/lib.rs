//! The WebSocket protocol (RFC 6455) as the server speaks it on /lan, the way the `ws` library (8.x) that the Node
//! server used spoke it (`perMessageDeflate: false`, `autoPong: false`, `maxPayload` 64 KiB, `skipUTF8Validation`
//! false): the parts the switch (nfsu2-lan) builds its connections from.
//!
//! - the handshake: [`accept_key`], [`pick_protocol`] (ws's reading of Sec-WebSocket-Protocol: a malformed one is
//!   refused with 400), [`handshake_response`] (the 101 ws wrote)
//! - [`WsReader`]: messages from a client (masked frames; fragments joined; a message over `max_payload` is a
//!   1009, broken frames a 1002, a text message that is not UTF-8 a 1007, as ws closed them)
//! - [`frame`]: frames to a client (unmasked), into a buffer, so that a writer puts many into one write
//!
//! Every rule, code and reason is ws's (receiver.js, subprotocol.js, validation.js, websocket-server.js); the tests
//! compare the reader and the handshake with ws itself (tests/node.rs).

use std::collections::HashSet;
use std::fmt;

use base64::Engine;
use bytes::Bytes;
use sha1::{Digest, Sha1};

mod reader;

pub use reader::WsReader;

/// RFC 6455's GUID
pub const GUID: &str = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/// Sec-WebSocket-Accept for a Sec-WebSocket-Key
pub fn accept_key(key: &str) -> String {
    let mut h = Sha1::new();
    h.update(key.as_bytes());
    h.update(GUID.as_bytes());
    base64::engine::general_purpose::STANDARD.encode(h.finalize())
}

/// ws's reading of a Sec-WebSocket-Protocol header: Ok(None) when there is none, Ok(Some(first)) for the protocol
/// ws picks (the first: the server has no handleProtocols), Err(()) when it is malformed (ws: 400 "Invalid
/// Sec-WebSocket-Protocol header"; the server answers its own 400)
///
/// subprotocol.js's parse(), step for step: tokens separated by commas, spaces and tabs around them but not first
/// (nor last: Node trims a header's value, so a trailing one never came), no empty item, no token twice.  An empty
/// header is malformed too.
// (the unit error is the fixed API: there is one way to be malformed, and it is answered with a 400)
#[allow(clippy::result_unit_err)]
pub fn pick_protocol(header: Option<&str>) -> Result<Option<String>, ()> {
    let Some(h) = header else { return Ok(None) };
    // a set, not a list: a header of thousands of short tokens must not cost their square
    let mut seen: HashSet<&str> = HashSet::new();
    let mut first: Option<&str> = None;
    let (mut start, mut end): (Option<usize>, Option<usize>) = (None, None);
    for (i, &c) in h.as_bytes().iter().enumerate() {
        if end.is_none() && is_token(c) {
            start.get_or_insert(i);
        } else if i != 0 && (c == b' ' || c == b'\t') {
            if end.is_none() && start.is_some() {
                end = Some(i);
            }
        } else if c == b',' {
            let Some(s) = start else { return Err(()) };
            // start and end sit on ASCII bytes: always character boundaries
            let p = h.get(s..end.unwrap_or(i)).ok_or(())?;
            if !seen.insert(p) {
                return Err(());
            }
            first.get_or_insert(p);
            (start, end) = (None, None);
        } else {
            return Err(());
        }
    }
    let (Some(s), None) = (start, end) else { return Err(()) };
    let p = h.get(s..).ok_or(())?;
    if seen.contains(p) {
        return Err(());
    }
    Ok(Some(first.unwrap_or(p).to_string()))
}

/// validation.js's tokenChars: the characters of an HTTP token (RFC 7230)
fn is_token(c: u8) -> bool {
    matches!(c, b'!' | b'#'..=b'\'' | b'*' | b'+' | b'-' | b'.' | b'0'..=b'9' | b'A'..=b'Z' | b'^'..=b'z' | b'|' | b'~')
}

/// The 101 answer ws wrote: `HTTP/1.1 101 Switching Protocols`, Upgrade, Connection, Sec-WebSocket-Accept, and
/// Sec-WebSocket-Protocol when one was picked; nothing else
///
/// completeUpgrade's lines, in its order and spelling (the server had no 'headers' listener and no extensions).
pub fn handshake_response(key: &str, protocol: Option<&str>) -> Vec<u8> {
    let mut s = String::with_capacity(160);
    s.push_str("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ");
    s.push_str(&accept_key(key));
    s.push_str("\r\n");
    // ws: `if (protocol)`, so an empty one writes no header
    if let Some(p) = protocol.filter(|p| !p.is_empty()) {
        s.push_str("Sec-WebSocket-Protocol: ");
        s.push_str(p);
        s.push_str("\r\n");
    }
    s.push_str("\r\n");
    s.into_bytes()
}

/// A message from the client.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Message {
    /// UTF-8 (checked)
    Text(Bytes),
    Binary(Bytes),
    Ping(Bytes),
    Pong(Bytes),
    /// its close frame: the code (1005 when it had none) and reason (UTF-8, checked).  ws answered it with a close
    /// frame of the same code and reason (`frame::close_payload(code, reason)`, empty for 1005).
    Close(u16, Bytes),
}

/// Why no more messages come.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ReadError {
    /// the connection ended (or failed) without a close frame (ws's 'close' then says 1006)
    Closed,
    /// the client broke the protocol: the close frame to send it (ws's codes: 1002 a broken frame, 1007 a text
    /// message or close reason that is not UTF-8, 1009 a message over `max_payload`), then the connection goes.
    /// ws's close frame carried the code alone (`frame::close_payload(code, "")`); `reason` is the message of the
    /// error ws emitted (its 'error' event, which the server ignored), e.g. "Invalid WebSocket frame: MASK must be
    /// set" (the one for a bad close code is without the code: ws put it in, a static text cannot).
    Protocol { code: u16, reason: &'static str },
}

impl fmt::Display for ReadError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ReadError::Closed => f.write_str("the connection ended"),
            ReadError::Protocol { code, reason } => write!(f, "{reason} ({code})"),
        }
    }
}

impl std::error::Error for ReadError {}

/// Frames to a client (unmasked, FIN set, one frame a message).
pub mod frame {
    pub const CONTINUATION: u8 = 0x0;
    pub const TEXT: u8 = 0x1;
    pub const BINARY: u8 = 0x2;
    pub const CLOSE: u8 = 0x8;
    pub const PING: u8 = 0x9;
    pub const PONG: u8 = 0xA;

    /// the bytes of a frame's header for a payload of `len` (2, 4 or 10 bytes)
    ///
    /// As ws's Sender framed it: the shortest length form (7 bits up to 125, 16 up to 65535, 64 beyond), no mask (a
    /// server's frames are never masked).
    pub fn header(opcode: u8, len: usize, out: &mut Vec<u8>) {
        let b0 = 0x80 | opcode;
        if len < 126 {
            out.extend_from_slice(&[b0, len as u8]);
        } else if len < 65536 {
            let [hi, lo] = (len as u16).to_be_bytes();
            out.extend_from_slice(&[b0, 126, hi, lo]);
        } else {
            out.extend_from_slice(&[b0, 127]);
            out.extend_from_slice(&(len as u64).to_be_bytes());
        }
    }
    /// header and payload
    pub fn encode(opcode: u8, payload: &[u8], out: &mut Vec<u8>) {
        header(opcode, payload.len(), out);
        out.extend_from_slice(payload);
    }
    /// a close frame's payload: the code and the reason (at most 123 bytes)
    ///
    /// A longer reason is cut at a character (ws threw a RangeError: a close frame's payload is 125 bytes at most).
    /// 1005 is "no code": the empty payload ws sent for a close() without one (its answer to a client's close frame
    /// that had no code; 1005 itself may never be sent).
    pub fn close_payload(code: u16, reason: &str) -> Vec<u8> {
        if code == 1005 {
            return Vec::new();
        }
        let mut n = reason.len().min(123);
        while !reason.is_char_boundary(n) {
            n -= 1;
        }
        let mut p = Vec::with_capacity(2 + n);
        p.extend_from_slice(&code.to_be_bytes());
        p.extend_from_slice(&reason.as_bytes()[..n]);
        p
    }
}
