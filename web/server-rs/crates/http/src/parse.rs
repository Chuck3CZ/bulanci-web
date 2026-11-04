//! The request parser: what llhttp 9.3 (Node 24's parser, strict: no lenient flags, the Node server set none) accepts
//! and refuses, so that a request Node answered 400 is answered 400 here and one it served is read the same way.
//! It reads the bytes as they come (a state per byte, nothing read twice), keeps what Node keeps (the URL, the header
//! names and values) and counts toward `max_header_size` what Node counts: the URL, the names, and the values with
//! their trailing whitespace, but not the method, the version, separators or a value's leading whitespace.
//!
//! The rules, as Node 24 applies them (each was checked against it, tests/cross_node.rs):
//! - a method llhttp knows (RTSP ones only with RTSP/x.y), one or more spaces, a URL of llhttp's strict characters
//!   (origin form, `*`, or `scheme://authority...`), spaces, `HTTP/`, `RTSP/` or `ICE/`, a version of 0.9, 1.0, 1.1
//!   or 2.0, CRLF; a URL followed by the line end is an HTTP/0.9 request (headers still follow)
//! - `PRI` is only ever the HTTP/2 preface: a 400 either way
//! - header lines end in CRLF (a bare LF or CR is a 400), names are tokens followed at once by `:`, values are
//!   tab, space, visible ASCII or obs-text; a line that starts with whitespace (obsolete folding) is a 400
//! - Content-Length: digits and trailing spaces, once, never with Transfer-Encoding; Transfer-Encoding: chunked only
//!   as the last coding, once (a request whose last coding is not chunked is read, then answered 400)
//! - chunk sizes in hex, extensions as tokens or quoted strings, CRLF everywhere, trailers as header lines

use crate::ClientError;
use crate::Headers;

/// llhttp's methods by its numbers
pub(crate) const METHODS: [&str; 47] = [
    "DELETE",
    "GET",
    "HEAD",
    "POST",
    "PUT",
    "CONNECT",
    "OPTIONS",
    "TRACE",
    "COPY",
    "LOCK",
    "MKCOL",
    "MOVE",
    "PROPFIND",
    "PROPPATCH",
    "SEARCH",
    "UNLOCK",
    "BIND",
    "REBIND",
    "UNBIND",
    "ACL",
    "REPORT",
    "MKACTIVITY",
    "CHECKOUT",
    "MERGE",
    "M-SEARCH",
    "NOTIFY",
    "SUBSCRIBE",
    "UNSUBSCRIBE",
    "PATCH",
    "PURGE",
    "MKCALENDAR",
    "LINK",
    "UNLINK",
    "SOURCE",
    "PRI",
    "DESCRIBE",
    "ANNOUNCE",
    "SETUP",
    "PLAY",
    "PAUSE",
    "TEARDOWN",
    "GET_PARAMETER",
    "SET_PARAMETER",
    "REDIRECT",
    "RECORD",
    "FLUSH",
    "QUERY",
];
const M_GET: u8 = 1;
const M_POST: u8 = 3;
const M_CONNECT: u8 = 5;
const M_OPTIONS: u8 = 6;
const M_SOURCE: u8 = 33;
const M_PRI: u8 = 34;
const M_QUERY: u8 = 46;
/// the longest method name ("GET_PARAMETER")
const METHOD_MAX: usize = 13;

/// methods llhttp takes with HTTP/x.y
fn http_method(m: u8) -> bool {
    m <= M_PRI || m == M_QUERY
}
/// with RTSP/x.y (the RTSP ones, and GET and POST for AirPlay)
fn rtsp_method(m: u8) -> bool {
    (35..=45).contains(&m) || m == M_OPTIONS || m == M_GET || m == M_POST
}

/// Node keeps the first 1000 header lines of a request (MAX_HEADER_PAIRS: 2000 names and values); the others are
/// read and checked, and dropped.
pub(crate) const HEADER_LINES_KEPT: usize = 1000;
/// Node's limit on one chunk's extensions (kMaxChunkExtensionsSize): more is a 400
const CHUNK_EXTENSIONS_MAX: usize = 16 << 10;

// ---- character classes (llhttp's) ----
fn tchar(b: u8) -> bool {
    b.is_ascii_alphanumeric()
        || matches!(b, b'!' | b'#' | b'$' | b'%' | b'&' | b'\'' | b'*' | b'+' | b'-' | b'.' | b'^' | b'_' | b'`' | b'|' | b'~')
}
/// STRICT_URL_CHAR: visible ASCII but `#` and `?` (which start a fragment or a query)
fn url_char(b: u8) -> bool {
    (0x21..=0x7e).contains(&b) && b != b'#' && b != b'?'
}
/// USERINFO_CHARS, and the brackets and `@` an authority may hold
fn server_char(b: u8) -> bool {
    b.is_ascii_alphanumeric()
        || matches!(
            b,
            b'-' | b'_'
                | b'.'
                | b'!'
                | b'~'
                | b'*'
                | b'\''
                | b'('
                | b')'
                | b'%'
                | b';'
                | b':'
                | b'&'
                | b'='
                | b'+'
                | b'$'
                | b','
                | b'['
                | b']'
                | b'@'
        )
}
/// a header value's characters: tab, space, visible ASCII, obs-text
fn value_char(b: u8) -> bool {
    b == b'\t' || (0x20..=0x7e).contains(&b) || b >= 0x80
}
/// inside a quoted chunk extension (qdtext)
fn qdtext(b: u8) -> bool {
    b == b'\t' || b == b' ' || b == 0x21 || (0x23..=0x5b).contains(&b) || (0x5d..=0x7e).contains(&b) || b >= 0x80
}
fn hex(b: u8) -> Option<u64> {
    (b as char).to_digit(16).map(u64::from)
}

// llhttp's flags
const F_CONNECTION_KEEP_ALIVE: u16 = 1;
const F_CONNECTION_CLOSE: u16 = 2;
const F_CONNECTION_UPGRADE: u16 = 4;
const F_CHUNKED: u16 = 8;
const F_UPGRADE: u16 = 16;
const F_CONTENT_LENGTH: u16 = 32;
const F_TRANSFER_ENCODING: u16 = 64;

/// The headers llhttp reads itself.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
enum Special {
    None,
    Connection,
    ContentLength,
    TransferEncoding,
    Upgrade,
}

fn special(name: &[u8]) -> Special {
    let is = |s: &str| name.eq_ignore_ascii_case(s.as_bytes());
    match name.len() {
        7 if is("upgrade") => Special::Upgrade,
        10 if is("connection") => Special::Connection,
        14 if is("content-length") => Special::ContentLength,
        16 if is("proxy-connection") => Special::Connection,
        17 if is("transfer-encoding") => Special::TransferEncoding,
        _ => Special::None,
    }
}

#[derive(Copy, Clone, PartialEq, Eq, Debug)]
enum Url {
    Start,
    Schema,
    Slash1,
    Slash2,
    Server,
    Path,
    Query,
    Fragment,
}

/// Content-Length's value: digits, then spaces
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
enum Cl {
    First,
    Digits,
    Spaces,
}

/// Transfer-Encoding's value: is `chunked` the last coding
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
enum Te {
    /// a coding starts; the bytes of "chunked" matched so far
    Start(u8),
    /// "chunked", and spaces after it
    ChunkedLast,
    /// any other coding, up to its comma
    Token,
    /// whitespace after a comma
    Ows,
}

#[derive(Copy, Clone, PartialEq, Eq, Debug)]
enum Ext {
    /// a name (empty so far: `true`)
    Name(bool),
    ValueStart,
    Value,
    Quoted,
    QuotedPair,
    QuotedEnd,
}

#[derive(Copy, Clone, PartialEq, Eq, Debug)]
enum St {
    /// between requests (CR and LF are skipped)
    Start,
    /// after a request that does not keep the connection: only CR and LF may follow
    Closed,
    /// nothing more is read (an upgrade, an error)
    Stopped,
    Method,
    SpacesBeforeUrl,
    Url(Url),
    /// CR after an HTTP/0.9 request line
    Url09Lf,
    SpacesAfterUrl,
    /// "HTTP/", "RTSP/" or "ICE/" (which: `proto`, matched: `k`)
    Proto,
    Major,
    Dot,
    Minor,
    LineCr,
    LineLf,
    /// `PRI ... HTTP/2.0`: the rest of the HTTP/2 preface (matched: `k`)
    Pri,
    /// a header line starts (or a trailer line, `trailers`)
    LineStart,
    Name,
    ValueOws,
    Value,
    ValueLf,
    /// CR of the empty line
    HeadLf,
    /// the head is out; the message ends without a body
    EndNext,
    /// the head is out; then this error (a request whose last coding is not chunked)
    FailNext,
    Body,
    ChunkSize,
    ChunkSizeLf,
    ChunkExt(Ext),
    ChunkData,
    ChunkDataCr,
    ChunkDataLf,
}

/// How a request's body is framed.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub(crate) enum BodyKind {
    None,
    Length(u64),
    Chunked,
}

/// A request's head, as Node's parser hands it to the server.
#[derive(Debug)]
pub(crate) struct Head {
    pub method: u8,
    pub url: String,
    pub major: u8,
    pub minor: u8,
    pub headers: Headers,
    /// llhttp's `should_keep_alive` at the end of the head (the answer's Connection header)
    pub keep_alive: bool,
    /// Connection: upgrade with an Upgrade header: the rest of the connection is not HTTP
    pub upgrade: bool,
    /// CONNECT (Node has no listener for it: the connection goes at once)
    pub close: bool,
    /// PRI read as HTTP/0.9: Node handed it to the server, read on, then destroyed the connection
    pub pri: bool,
    pub body: BodyKind,
}

impl Head {
    pub fn method_str(&self) -> &'static str {
        METHODS[self.method as usize]
    }
}

/// What the parser found in the bytes it was given.
#[derive(Debug)]
pub(crate) enum Event {
    /// all of it read: more is needed
    More,
    /// a request begins (its first byte; Node's header and request times count from here)
    Begin,
    Head(Box<Head>),
    /// body bytes: the last `len` of the bytes consumed
    Body(usize),
    /// the request (and its body) is complete
    End,
    Error(ClientError),
}

pub(crate) struct Parser {
    max_header: usize,
    st: St,
    trailers: bool,
    method: u8,
    mlen: usize,
    mbuf: [u8; METHOD_MAX],
    proto: u8,
    k: u8,
    major: u8,
    minor: u8,
    /// the URL, then each header's name and value
    hbuf: Vec<u8>,
    url_end: usize,
    /// (name start, name end, value start, value end) in `hbuf`, for the lines Node keeps
    fields: Vec<[u32; 4]>,
    /// the line being read is one Node keeps
    keep_line: bool,
    lines: usize,
    name_start: usize,
    value_start: usize,
    /// bytes toward max_header_size (Node's header_nread_: from a request's start, and again from each chunk size
    /// for the trailers)
    counted: usize,
    flags: u16,
    special: Special,
    cl: Cl,
    te: Te,
    /// the value of the line has started (it is not empty)
    value_seen: bool,
    content_length: u64,
    remaining: u64,
    chunk_digits: bool,
    ext_bytes: usize,
}

/// a head's URL, names and values (a browser's: a few hundred bytes), and its lines, to start with
const HEAD_BUFFER: usize = 512;
const FIELDS: usize = 16;
/// what an idle connection's parser keeps of what a larger head grew (Cloudflare's heads, with its own headers,
/// are 1-2 KiB)
const IDLE_HEAD_BUFFER: usize = 4096;
const IDLE_FIELDS: usize = 64;

const PRI_PREFACE: &[u8] = b"\r\n\r\nSM\r\n\r\n";
const PROTOS: [&[u8]; 3] = [b"HTTP/", b"RTSP/", b"ICE/"];

impl Parser {
    pub fn new(max_header: usize) -> Self {
        Parser {
            max_header,
            st: St::Start,
            trailers: false,
            method: 0,
            mlen: 0,
            mbuf: [0; METHOD_MAX],
            proto: 0,
            k: 0,
            major: 0,
            minor: 0,
            hbuf: Vec::with_capacity(HEAD_BUFFER),
            url_end: 0,
            fields: Vec::with_capacity(FIELDS),
            keep_line: false,
            lines: 0,
            name_start: 0,
            value_start: 0,
            counted: 0,
            flags: 0,
            special: Special::None,
            cl: Cl::First,
            te: Te::Start(0),
            value_seen: false,
            content_length: 0,
            remaining: 0,
            chunk_digits: false,
            ext_bytes: 0,
        }
    }

    #[cfg(test)]
    fn held(&self) -> (usize, usize) {
        (self.hbuf.capacity(), self.fields.capacity())
    }

    /// the parser stopped for good (an upgrade, CONNECT, an error): what follows is not its to read
    pub fn stopped(&self) -> bool {
        self.st == St::Stopped
    }

    /// an event is due without another byte (the end of a request without a body, or its refusal)
    pub fn pending(&self) -> bool {
        matches!(self.st, St::EndNext | St::FailNext)
    }

    /// between requests: the last one was read to its end, the next has not begun
    pub fn between(&self) -> bool {
        matches!(self.st, St::Start | St::Closed)
    }

    /// The client ended its side: an error if a request was under way (llhttp's "Invalid EOF state").
    pub fn eof_error(&self) -> Option<ClientError> {
        match self.st {
            St::Start | St::Closed | St::Stopped => None,
            _ => Some(ClientError::Parse),
        }
    }

    /// The parser stops: nothing more is read (an upgrade's head, a timeout).
    pub fn stop(&mut self) {
        self.st = St::Stopped;
    }

    /// Between requests (the connection waits for the next): the buffers a large head grew are let go, so that an
    /// idle keep-alive connection holds what a usual head needs, not the 16 KiB of the largest it ever read.
    pub fn idle(&mut self) {
        if !self.between() {
            return;
        }
        if self.hbuf.capacity() > IDLE_HEAD_BUFFER {
            self.hbuf = Vec::with_capacity(HEAD_BUFFER);
        }
        if self.fields.capacity() > IDLE_FIELDS {
            self.fields = Vec::with_capacity(FIELDS);
        }
    }

    fn fail(&mut self, e: ClientError) -> Event {
        self.st = St::Stopped;
        Event::Error(e)
    }

    /// one more byte toward max_header_size: Node refuses a head at max_header_size counted bytes
    #[inline]
    fn count(&mut self) -> bool {
        self.counted += 1;
        self.counted >= self.max_header
    }

    /// Reads from `input`: (bytes consumed, what was found).  Call again with what is left until it says `More`.
    pub fn parse(&mut self, input: &[u8]) -> (usize, Event) {
        let mut i = 0;
        // states that hand out an event without a byte
        match self.st {
            St::EndNext => {
                self.st = self.after_message();
                return (0, Event::End);
            }
            St::FailNext => return (0, self.fail(ClientError::Parse)),
            // (never called then: what follows is not the parser's)
            St::Stopped => return (0, Event::More),
            St::Body => return self.body(input),
            St::ChunkData => return self.chunk_data(input),
            _ => {}
        }
        while i < input.len() {
            // the common runs at once: a header value, a path or query, a header name (each byte as the state's
            // own rule would take it, nothing else)
            let run = match self.st {
                St::Value if self.special == Special::None => run_of(&input[i..], value_char),
                St::Url(Url::Path) => run_of(&input[i..], url_char),
                St::Url(Url::Query) => run_of(&input[i..], |b| url_char(b) || b == b'?'),
                St::Name => run_of(&input[i..], tchar),
                _ => 0,
            };
            if run > 0 {
                self.hbuf.extend_from_slice(&input[i..i + run]);
                self.counted += run;
                i += run;
                if self.counted >= self.max_header {
                    return (i, self.fail(ClientError::HeaderOverflow));
                }
                continue;
            }
            let b = input[i];
            i += 1;
            match self.st {
                St::Start => {
                    if b == b'\r' || b == b'\n' {
                        continue;
                    }
                    self.begin();
                    self.st = St::Method;
                    i -= 1;
                    return (i, Event::Begin);
                }
                St::Closed => {
                    if b == b'\r' || b == b'\n' {
                        continue;
                    }
                    // llhttp: "Data after `Connection: close`"
                    return (i, self.fail(ClientError::Parse));
                }
                St::Method => {
                    if b == b' ' && self.mlen > 0 {
                        match METHODS.iter().position(|m| m.as_bytes() == &self.mbuf[..self.mlen]) {
                            Some(m) => {
                                self.method = m as u8;
                                self.st = St::SpacesBeforeUrl;
                            }
                            None => return (i, self.fail(ClientError::Parse)),
                        }
                        continue;
                    }
                    if self.mlen == METHOD_MAX {
                        return (i, self.fail(ClientError::Parse));
                    }
                    self.mbuf[self.mlen] = b;
                    self.mlen += 1;
                    let prefix = &self.mbuf[..self.mlen];
                    if !METHODS.iter().any(|m| m.as_bytes().starts_with(prefix)) {
                        return (i, self.fail(ClientError::Parse));
                    }
                }
                St::SpacesBeforeUrl => {
                    if b == b' ' {
                        continue;
                    }
                    // CONNECT's target is an authority (host:port)
                    self.st = St::Url(if self.method == M_CONNECT { Url::Server } else { Url::Start });
                    i -= 1;
                }
                St::Url(u) => {
                    let next = match (u, b) {
                        (_, b' ') if !matches!(u, Url::Start | Url::Schema | Url::Slash1 | Url::Slash2) => {
                            self.url_end = self.hbuf.len();
                            self.st = St::SpacesAfterUrl;
                            continue;
                        }
                        (_, b'\r' | b'\n') if !matches!(u, Url::Start | Url::Schema | Url::Slash1 | Url::Slash2) => {
                            // HTTP/0.9: the line ends after the URL; headers still follow
                            self.url_end = self.hbuf.len();
                            self.major = 0;
                            self.minor = 9;
                            self.st = if b == b'\r' { St::Url09Lf } else { St::LineStart };
                            continue;
                        }
                        (Url::Start, b'/' | b'*') => Url::Path,
                        (Url::Start, b) if b.is_ascii_alphabetic() => Url::Schema,
                        (Url::Schema, b) if b.is_ascii_alphabetic() => Url::Schema,
                        (Url::Schema, b':') => Url::Slash1,
                        (Url::Slash1, b'/') => Url::Slash2,
                        (Url::Slash2, b'/') => Url::Server,
                        (Url::Server, b'/') => Url::Path,
                        (Url::Server, b'?') => Url::Query,
                        (Url::Server, b) if server_char(b) => Url::Server,
                        (Url::Path, b'?') => Url::Query,
                        (Url::Path, b'#') => Url::Fragment,
                        (Url::Path, b) if url_char(b) => Url::Path,
                        (Url::Query, b'#') => Url::Fragment,
                        (Url::Query, b) if url_char(b) || b == b'?' => Url::Query,
                        (Url::Fragment, b) if url_char(b) || b == b'?' || b == b'#' => Url::Fragment,
                        _ => return (i, self.fail(ClientError::Parse)),
                    };
                    self.hbuf.push(b);
                    self.st = St::Url(next);
                    if self.count() {
                        return (i, self.fail(ClientError::HeaderOverflow));
                    }
                }
                St::Url09Lf => {
                    if b != b'\n' {
                        return (i, self.fail(ClientError::Parse));
                    }
                    self.st = St::LineStart;
                }
                St::SpacesAfterUrl => {
                    if b == b' ' {
                        continue;
                    }
                    match PROTOS.iter().position(|p| p[0] == b) {
                        Some(p) => {
                            self.proto = p as u8;
                            self.k = 1;
                            self.st = St::Proto;
                        }
                        None => return (i, self.fail(ClientError::Parse)),
                    }
                }
                St::Proto => {
                    let p = PROTOS[self.proto as usize];
                    if b != p[self.k as usize] {
                        return (i, self.fail(ClientError::Parse));
                    }
                    self.k += 1;
                    if self.k as usize == p.len() {
                        let ok = match self.proto {
                            0 => http_method(self.method),
                            1 => rtsp_method(self.method),
                            _ => self.method == M_SOURCE,
                        };
                        if !ok {
                            return (i, self.fail(ClientError::Parse));
                        }
                        self.st = St::Major;
                    }
                }
                St::Major => match b {
                    b'0'..=b'9' => {
                        self.major = b - b'0';
                        self.st = St::Dot;
                    }
                    _ => return (i, self.fail(ClientError::Parse)),
                },
                St::Dot => {
                    if b != b'.' {
                        return (i, self.fail(ClientError::Parse));
                    }
                    self.st = St::Minor;
                }
                St::Minor => {
                    if !b.is_ascii_digit() {
                        return (i, self.fail(ClientError::Parse));
                    }
                    self.minor = b - b'0';
                    if !matches!((self.major, self.minor), (0, 9) | (1, 0) | (1, 1) | (2, 0)) {
                        return (i, self.fail(ClientError::Parse));
                    }
                    if self.method == M_PRI {
                        self.k = 0;
                        self.st = St::Pri;
                    } else {
                        self.st = St::LineCr;
                    }
                }
                St::Pri => {
                    // the HTTP/2 preface, or not: Node answers 400 to both (HPE_PAUSED_H2_UPGRADE, HPE_INVALID_VERSION)
                    if b != PRI_PREFACE[self.k as usize] {
                        return (i, self.fail(ClientError::Parse));
                    }
                    self.k += 1;
                    if self.k as usize == PRI_PREFACE.len() {
                        return (i, self.fail(ClientError::Parse));
                    }
                }
                St::LineCr => {
                    if b != b'\r' {
                        return (i, self.fail(ClientError::Parse));
                    }
                    self.st = St::LineLf;
                }
                St::LineLf => {
                    if b != b'\n' {
                        return (i, self.fail(ClientError::Parse));
                    }
                    self.st = St::LineStart;
                }
                St::LineStart => {
                    if b == b'\r' {
                        self.st = St::HeadLf;
                    } else if tchar(b) {
                        // every line is kept for llhttp's own reading of it (what is kept is counted, so it stays
                        // under max_header_size); Node hands the server only the first ones
                        self.keep_line = !self.trailers && self.lines < HEADER_LINES_KEPT;
                        self.lines += 1;
                        self.name_start = self.hbuf.len();
                        self.hbuf.push(b);
                        self.st = St::Name;
                        if self.count() {
                            return (i, self.fail(ClientError::HeaderOverflow));
                        }
                    } else {
                        // a bare LF, obsolete line folding (a line that starts with whitespace), anything else
                        return (i, self.fail(ClientError::Parse));
                    }
                }
                St::Name => {
                    if tchar(b) {
                        self.hbuf.push(b);
                        if self.count() {
                            return (i, self.fail(ClientError::HeaderOverflow));
                        }
                    } else if b == b':' {
                        if let Some(e) = self.name_done() {
                            return (i, self.fail(e));
                        }
                        self.st = St::ValueOws;
                    } else {
                        return (i, self.fail(ClientError::Parse));
                    }
                }
                St::ValueOws => {
                    if b == b' ' || b == b'\t' {
                        continue;
                    }
                    if b == b'\r' {
                        if self.special == Special::ContentLength {
                            // llhttp: "Empty Content-Length"
                            return (i, self.fail(ClientError::Parse));
                        }
                        self.st = St::ValueLf;
                        continue;
                    }
                    if !value_char(b) {
                        return (i, self.fail(ClientError::Parse));
                    }
                    if let Some(e) = self.value_starts() {
                        return (i, self.fail(e));
                    }
                    self.st = St::Value;
                    i -= 1;
                }
                St::Value => {
                    if b == b'\r' {
                        if let Some(e) = self.value_ends() {
                            return (i, self.fail(e));
                        }
                        self.st = St::ValueLf;
                        continue;
                    }
                    if !value_char(b) {
                        return (i, self.fail(ClientError::Parse));
                    }
                    if let Some(e) = self.special_byte(b) {
                        return (i, self.fail(e));
                    }
                    self.hbuf.push(b);
                    if self.count() {
                        return (i, self.fail(ClientError::HeaderOverflow));
                    }
                }
                St::ValueLf => {
                    if b != b'\n' {
                        return (i, self.fail(ClientError::Parse));
                    }
                    self.line_done();
                    self.st = St::LineStart;
                }
                St::HeadLf => {
                    if b != b'\n' {
                        return (i, self.fail(ClientError::Parse));
                    }
                    if self.trailers {
                        self.st = self.after_message();
                        return (i, Event::End);
                    }
                    return (i, self.head_done());
                }
                St::ChunkSize => {
                    if let Some(d) = hex(b) {
                        if self.remaining > u64::MAX >> 4 {
                            return (i, self.fail(ClientError::Parse));
                        }
                        self.remaining = self.remaining << 4 | d;
                        self.chunk_digits = true;
                    } else if self.chunk_digits && b == b';' {
                        self.st = St::ChunkExt(Ext::Name(true));
                    } else if self.chunk_digits && b == b'\r' {
                        self.st = St::ChunkSizeLf;
                    } else {
                        return (i, self.fail(ClientError::Parse));
                    }
                }
                St::ChunkSizeLf => {
                    if b != b'\n' {
                        return (i, self.fail(ClientError::Parse));
                    }
                    // Node resets its header count at each chunk size (the trailers count from here)
                    self.counted = 0;
                    if self.remaining == 0 {
                        self.trailers = true;
                        self.st = St::LineStart;
                    } else {
                        // the data is handed out in runs, not bytes
                        self.st = St::ChunkData;
                        let (n, e) = self.chunk_data(&input[i..]);
                        return (i + n, e);
                    }
                }
                St::ChunkExt(x) => {
                    let next = match (x, b) {
                        (Ext::Name(true), b'\r') => return (i, self.fail(ClientError::Parse)),
                        (Ext::Name(_), b) if tchar(b) => Ext::Name(false),
                        (Ext::Name(_), b'=') => Ext::ValueStart,
                        (Ext::Name(_), b';') => Ext::Name(true),
                        (Ext::ValueStart, b'"') => Ext::Quoted,
                        (Ext::ValueStart | Ext::Value, b) if tchar(b) => Ext::Value,
                        (Ext::ValueStart | Ext::Value | Ext::QuotedEnd, b';') => Ext::Name(true),
                        (Ext::Quoted, b'"') => Ext::QuotedEnd,
                        (Ext::Quoted, b'\\') => Ext::QuotedPair,
                        (Ext::Quoted, b) if qdtext(b) => Ext::Quoted,
                        (Ext::QuotedPair, b) if b == b'\t' || b == b' ' || (0x21..=0x7e).contains(&b) || b >= 0x80 => Ext::Quoted,
                        (Ext::Name(false) | Ext::ValueStart | Ext::Value | Ext::QuotedEnd, b'\r') => {
                            self.st = St::ChunkSizeLf;
                            continue;
                        }
                        _ => return (i, self.fail(ClientError::Parse)),
                    };
                    if b != b'=' && b != b';' {
                        self.ext_bytes += 1;
                        if self.ext_bytes > CHUNK_EXTENSIONS_MAX {
                            return (i, self.fail(ClientError::Parse));
                        }
                    }
                    self.st = St::ChunkExt(next);
                }
                St::ChunkDataCr => {
                    if b != b'\r' {
                        return (i, self.fail(ClientError::Parse));
                    }
                    self.st = St::ChunkDataLf;
                }
                St::ChunkDataLf => {
                    if b != b'\n' {
                        return (i, self.fail(ClientError::Parse));
                    }
                    self.chunk_start();
                }
                St::EndNext | St::FailNext | St::Stopped | St::Body | St::ChunkData => unreachable!(),
            }
        }
        (i, Event::More)
    }

    /// a request starts: what the last one left is cleared
    fn begin(&mut self) {
        self.trailers = false;
        self.counted = 0;
        self.mlen = 0;
        self.hbuf.clear();
        self.fields.clear();
        self.url_end = 0;
        self.lines = 0;
        self.flags = 0;
        self.content_length = 0;
        self.major = 1;
        self.minor = 1;
    }

    fn chunk_start(&mut self) {
        self.remaining = 0;
        self.chunk_digits = false;
        self.ext_bytes = 0;
        self.st = St::ChunkSize;
    }

    /// llhttp's should_keep_alive: HTTP/1.1 unless Connection: close; earlier ones only with Connection: keep-alive
    fn keep_alive(&self) -> bool {
        if self.major > 0 && self.minor > 0 { self.flags & F_CONNECTION_CLOSE == 0 } else { self.flags & F_CONNECTION_KEEP_ALIVE != 0 }
    }

    /// where the parser goes once a request is complete (by the flags then, trailers included)
    fn after_message(&self) -> St {
        if self.keep_alive() { St::Start } else { St::Closed }
    }

    /// A header name ended (at its colon): what llhttp checks there.
    fn name_done(&mut self) -> Option<ClientError> {
        self.special = special(&self.hbuf[self.name_start..]);
        self.value_seen = false;
        self.value_start = self.hbuf.len();
        match self.special {
            Special::TransferEncoding if self.flags & F_CONTENT_LENGTH != 0 => return Some(ClientError::Parse),
            // a second Content-Length (even the same), or one after Transfer-Encoding
            Special::ContentLength if self.flags & (F_CONTENT_LENGTH | F_TRANSFER_ENCODING) != 0 => return Some(ClientError::Parse),
            _ => {}
        }
        self.cl = Cl::First;
        self.te = Te::Start(0);
        None
    }

    /// The first byte of a value (after the whitespace llhttp drops).
    fn value_starts(&mut self) -> Option<ClientError> {
        self.value_seen = true;
        match self.special {
            Special::TransferEncoding => {
                // after chunked, no other coding (llhttp strict)
                if self.flags & F_CHUNKED != 0 {
                    return Some(ClientError::Parse);
                }
                self.flags |= F_TRANSFER_ENCODING;
            }
            Special::Upgrade => self.flags |= F_UPGRADE,
            _ => {}
        }
        None
    }

    /// A byte of a value, for the headers llhttp reads itself.
    fn special_byte(&mut self, b: u8) -> Option<ClientError> {
        match self.special {
            Special::ContentLength => match (self.cl, b) {
                (Cl::First | Cl::Digits, b'0'..=b'9') => {
                    let d = u64::from(b - b'0');
                    match self.content_length.checked_mul(10).and_then(|v| v.checked_add(d)) {
                        Some(v) => self.content_length = v,
                        None => return Some(ClientError::Parse),
                    }
                    self.cl = Cl::Digits;
                }
                (Cl::Digits | Cl::Spaces, b' ') => self.cl = Cl::Spaces,
                _ => return Some(ClientError::Parse),
            },
            Special::TransferEncoding => loop {
                match self.te {
                    Te::Start(k) => {
                        if b.to_ascii_lowercase() == b"chunked"[k as usize] {
                            self.te = if k == 6 { Te::ChunkedLast } else { Te::Start(k + 1) };
                            break;
                        }
                        self.te = Te::Token;
                    }
                    Te::ChunkedLast => {
                        if b == b' ' {
                            break;
                        }
                        if b == b',' {
                            // chunked followed by another coding
                            return Some(ClientError::Parse);
                        }
                        self.te = Te::Token;
                    }
                    Te::Token => {
                        if b == b',' {
                            self.te = Te::Ows;
                        }
                        break;
                    }
                    Te::Ows => {
                        if b == b' ' || b == b'\t' {
                            break;
                        }
                        self.te = Te::Start(0);
                    }
                }
            },
            _ => {}
        }
        None
    }

    /// The value ended (CR).
    fn value_ends(&mut self) -> Option<ClientError> {
        match self.special {
            Special::ContentLength => {
                if self.cl == Cl::First {
                    return Some(ClientError::Parse);
                }
                self.flags |= F_CONTENT_LENGTH;
            }
            Special::TransferEncoding => {
                if self.te == Te::ChunkedLast {
                    self.flags |= F_CHUNKED;
                }
            }
            Special::Connection => {
                let v = &self.hbuf[self.value_start..];
                self.flags |= connection_flags(v);
            }
            _ => {}
        }
        None
    }

    /// A header line ended (CRLF).
    fn line_done(&mut self) {
        if self.keep_line {
            self.fields.push([self.name_start as u32, self.value_start as u32, self.value_start as u32, self.hbuf.len() as u32]);
        }
        self.special = Special::None;
    }

    /// The empty line: the head is complete.
    fn head_done(&mut self) -> Event {
        // Node resets its count at the end of each head
        self.counted = 0;
        let upgrade = (self.flags & F_UPGRADE != 0 && self.flags & F_CONNECTION_UPGRADE != 0) || self.method == M_CONNECT;
        let close = self.method == M_CONNECT;
        let body = if upgrade || close {
            BodyKind::None
        } else if self.flags & F_CHUNKED != 0 {
            BodyKind::Chunked
        } else if self.flags & F_CONTENT_LENGTH != 0 && self.content_length > 0 {
            BodyKind::Length(self.content_length)
        } else {
            BodyKind::None
        };
        let head = Box::new(Head {
            method: self.method,
            url: latin1(&self.hbuf[..self.url_end]),
            major: self.major,
            minor: self.minor,
            headers: crate::headers::join(self.fields.iter().map(|f| {
                let name = &self.hbuf[f[0] as usize..f[1] as usize];
                let mut value = &self.hbuf[f[2] as usize..f[3] as usize];
                // Node trims the value's trailing whitespace (llhttp already dropped the leading)
                while let [rest @ .., b' ' | b'\t'] = value {
                    value = rest;
                }
                (name, value)
            })),
            keep_alive: self.keep_alive(),
            upgrade: upgrade && !close,
            close,
            pri: self.method == M_PRI,
            body,
        });
        self.st = if upgrade || close {
            // what follows is another protocol (or nothing)
            St::Stopped
        } else {
            match body {
                BodyKind::Chunked => {
                    self.chunk_start();
                    St::ChunkSize
                }
                BodyKind::Length(n) => {
                    self.remaining = n;
                    St::Body
                }
                // a request with a Transfer-Encoding whose last coding is not chunked: llhttp hands the head to the
                // server, then refuses (RFC 9112 6.3: the length cannot be known)
                BodyKind::None if self.flags & F_TRANSFER_ENCODING != 0 => St::FailNext,
                BodyKind::None => St::EndNext,
            }
        };
        // the trailers (if any) are read for llhttp's flags, not kept
        self.hbuf.clear();
        self.fields.clear();
        Event::Head(head)
    }

    /// Content-Length body bytes.
    fn body(&mut self, input: &[u8]) -> (usize, Event) {
        if input.is_empty() {
            return (0, Event::More);
        }
        if self.remaining == 0 {
            self.st = self.after_message();
            return (0, Event::End);
        }
        let n = (self.remaining.min(input.len() as u64)) as usize;
        self.remaining -= n as u64;
        if self.remaining == 0 {
            self.st = St::EndNext;
        }
        (n, Event::Body(n))
    }

    /// Chunk data bytes.
    fn chunk_data(&mut self, input: &[u8]) -> (usize, Event) {
        if input.is_empty() {
            return (0, Event::More);
        }
        let n = (self.remaining.min(input.len() as u64)) as usize;
        self.remaining -= n as u64;
        if self.remaining == 0 {
            self.st = St::ChunkDataCr;
        }
        (n, Event::Body(n))
    }
}

/// how many bytes from the start `ok` takes
#[inline]
fn run_of(s: &[u8], ok: impl Fn(u8) -> bool) -> usize {
    s.iter().position(|&b| !ok(b)).unwrap_or(s.len())
}

/// The tokens of a Connection header as llhttp reads them: close, keep-alive or upgrade, each between commas, with
/// leading whitespace and trailing spaces (not tabs: llhttp's rule), case-insensitive.
pub(crate) fn connection_flags(v: &[u8]) -> u16 {
    let mut flags = 0;
    for token in v.split(|&b| b == b',') {
        let mut t = token;
        while let [b' ' | b'\t', rest @ ..] = t {
            t = rest;
        }
        while let [rest @ .., b' '] = t {
            t = rest;
        }
        if t.eq_ignore_ascii_case(b"close") {
            flags |= F_CONNECTION_CLOSE;
        } else if t.eq_ignore_ascii_case(b"keep-alive") {
            flags |= F_CONNECTION_KEEP_ALIVE;
        } else if t.eq_ignore_ascii_case(b"upgrade") {
            flags |= F_CONNECTION_UPGRADE;
        }
    }
    flags
}

/// bytes as Node turns them into a string (latin1: one character per byte)
pub(crate) fn latin1(b: &[u8]) -> String {
    if b.is_ascii() {
        // ASCII: the bytes are the string
        return String::from_utf8(b.to_vec()).unwrap_or_default();
    }
    b.iter().map(|&c| c as char).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// every event of a parse of `input` given in pieces of `step` bytes (body runs joined; what is left after the
    /// parser stopped as "rest")
    fn events(input: &[u8], step: usize, max: usize) -> Vec<String> {
        let mut p = Parser::new(max);
        let mut out: Vec<String> = Vec::new();
        let mut buf: Vec<u8> = Vec::new();
        for piece in input.chunks(step) {
            buf.extend_from_slice(piece);
            while !p.stopped() {
                let (n, e) = p.parse(&buf);
                let done = matches!(e, Event::More);
                match e {
                    Event::More => {}
                    Event::Begin => out.push("begin".into()),
                    Event::Head(h) => out.push(format!(
                        "head {} {} {}.{} {:?} ka={} up={} close={} {:?}",
                        h.method_str(),
                        h.url,
                        h.major,
                        h.minor,
                        h.headers.iter().collect::<Vec<_>>(),
                        h.keep_alive,
                        h.upgrade,
                        h.close,
                        h.body
                    )),
                    Event::Body(len) => {
                        let s = String::from_utf8_lossy(&buf[n - len..n]).to_string();
                        match out.last_mut() {
                            Some(last) if last.starts_with("body ") => last.push_str(&s),
                            _ => out.push(format!("body {s}")),
                        }
                    }
                    Event::End => out.push("end".into()),
                    Event::Error(e) => out.push(format!("error {e:?}")),
                }
                buf.drain(..n);
                if done {
                    break;
                }
            }
        }
        if p.stopped() && !buf.is_empty() {
            out.push(format!("rest {}", String::from_utf8_lossy(&buf)));
        }
        out
    }

    fn all_steps(input: &[u8]) -> Vec<String> {
        let whole = events(input, input.len().max(1), 16 << 10);
        for step in [1, 2, 3, 7] {
            assert_eq!(events(input, step, 16 << 10), whole, "in pieces of {step}: {:?}", String::from_utf8_lossy(input));
        }
        whole
    }

    #[test]
    fn a_simple_request() {
        let e = all_steps(b"GET /a?b HTTP/1.1\r\nHost: x\r\nX-A:  1 \r\n\r\n");
        assert_eq!(e, vec!["begin", "head GET /a?b 1.1 [(\"host\", \"x\"), (\"x-a\", \"1\")] ka=true up=false close=false None", "end"]);
    }

    #[test]
    fn bodies_with_a_length_and_chunked() {
        let e = all_steps(b"POST / HTTP/1.1\r\nContent-Length: 3\r\n\r\nabcGET / HTTP/1.0\r\n\r\n");
        assert_eq!(e[1], "head POST / 1.1 [(\"content-length\", \"3\")] ka=true up=false close=false Length(3)");
        assert_eq!(e[2], "body abc");
        assert!(e.contains(&"head GET / 1.0 [] ka=false up=false close=false None".to_string()));
        let e = all_steps(b"POST / HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n3;a=\"b;c\"\r\nabc\r\n2\r\nde\r\n0\r\nT: 1\r\n\r\n");
        assert_eq!(e[2], "body abcde");
        assert_eq!(e.last().map(String::as_str), Some("end"));
    }

    #[test]
    fn what_node_refuses() {
        for bad in [
            &b"get / HTTP/1.1\r\n\r\n"[..],
            b"FOO / HTTP/1.1\r\n\r\n",
            b"DESCRIBE / HTTP/1.1\r\n\r\n",
            b"GET / HTTP/1.1\n\n",
            b"GET / HTTP/1.1\r\nHost: a\n\r\n",
            b"GET / HTTP/1.1\r\nHost : a\r\n\r\n",
            b"GET / HTTP/1.1\r\nX: a\r\n b\r\n\r\n",
            b"GET / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n",
            b"GET / HTTP/1.1\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n",
            b"GET / HTTP/1.1\r\nTransfer-Encoding: chunked, gzip\r\n\r\n",
            b"GET /\x7f HTTP/1.1\r\n\r\n",
            b"GET / HTTP/1.2\r\n\r\n",
            b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n",
            b"POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3 \r\nabc\r\n0\r\n\r\n",
        ] {
            let e = all_steps(bad);
            assert!(e.iter().any(|x| x == "error Parse"), "{:?}: {e:?}", String::from_utf8_lossy(bad));
        }
    }

    #[test]
    fn a_coding_other_than_chunked_last_is_refused_after_the_head() {
        let e = all_steps(b"GET / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n");
        assert!(e[1].starts_with("head GET"));
        assert_eq!(e[2], "error Parse");
    }

    #[test]
    fn the_header_limit_counts_what_node_counts() {
        // URL 1 + name 1 + value 197 = 199 < 200; one more (even a trailing space) is 200
        let ok = format!("GET / HTTP/1.1\r\nX:   {}\r\n\r\n", "a".repeat(197));
        assert!(events(ok.as_bytes(), 5, 200).iter().any(|x| x.starts_with("head")));
        let over = format!("GET / HTTP/1.1\r\nX: {} \r\n\r\n", "a".repeat(197));
        assert!(events(over.as_bytes(), 5, 200).contains(&"error HeaderOverflow".to_string()));
    }

    #[test]
    fn an_idle_parser_lets_go_of_what_a_large_head_grew() {
        let pad: String = (0..400).map(|i| format!("X-{i}: {}\r\n", "v".repeat(30))).collect();
        let big = format!("GET / HTTP/1.1\r\n{pad}\r\n");
        let mut p = Parser::new(16 << 10);
        // half of it: in the middle of a head nothing is let go
        let half = big.len() / 2;
        assert!(matches!(p.parse(&big.as_bytes()[..half]), (0, Event::Begin)));
        assert!(matches!(p.parse(&big.as_bytes()[..half]), (n, Event::More) if n == half));
        let mid = p.held();
        p.idle();
        assert_eq!(p.held(), mid);
        let (_, e) = p.parse(&big.as_bytes()[half..]);
        assert!(matches!(e, Event::Head(_)), "{e:?}");
        assert!(matches!(p.parse(&[]), (0, Event::End)));
        assert!(p.held().0 > IDLE_HEAD_BUFFER && p.held().1 > IDLE_FIELDS, "{:?}", p.held());
        p.idle();
        assert!(p.held().0 <= IDLE_HEAD_BUFFER && p.held().1 <= IDLE_FIELDS, "{:?}", p.held());
        // and it reads the next one as ever
        let (_, e) = p.parse(b"GET /b HTTP/1.1\r\n\r\n");
        assert!(matches!(e, Event::Begin));
    }

    #[test]
    fn upgrade_stops_and_leaves_the_rest() {
        let e = all_steps(b"GET /lan HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nContent-Length: 2\r\n\r\nxyz");
        assert!(e[1].contains("up=true"));
        assert_eq!(e.last().map(String::as_str), Some("rest xyz"));
    }

    /// the methods Node lists (http.METHODS) are exactly those llhttp takes with HTTP/x.y, PRI aside
    #[test]
    fn methods_as_node_lists_them() {
        let Some(out) = crate::node::run("import http from 'node:http'; process.stdout.write(JSON.stringify(http.METHODS));", "") else {
            eprintln!("skipped: no node to compare with");
            return;
        };
        let want: Vec<String> = serde_json::from_str(&out).unwrap_or_default();
        let mut got: Vec<String> =
            (0..METHODS.len() as u8).filter(|&m| http_method(m) && m != M_PRI).map(|m| METHODS[m as usize].to_string()).collect();
        got.sort();
        assert_eq!(got, want);
    }

    #[test]
    fn connection_tokens() {
        assert_eq!(connection_flags(b"close"), F_CONNECTION_CLOSE);
        assert_eq!(connection_flags(b"x, Keep-Alive "), F_CONNECTION_KEEP_ALIVE);
        assert_eq!(connection_flags(b"close\t"), 0);
        assert_eq!(connection_flags(b"closed"), 0);
        assert_eq!(connection_flags(b"x,, close"), F_CONNECTION_CLOSE);
        assert_eq!(connection_flags(b"Upgrade, HTTP2-Settings"), F_CONNECTION_UPGRADE);
    }
}
