//! Cross-checks against the ws library itself (web/node_modules/ws, run by node): its Receiver over random byte
//! streams of client frames, and its WebSocketServer's handshake over random Sec-WebSocket-Protocol headers.
//! Skipped (with a message) where node or ws is missing.

mod common;

use std::io::Write;
use std::path::PathBuf;
use std::process::{Command, Stdio};

use bytes::Bytes;
use common::*;
use nfsu2_ws::{Message, ReadError, WsReader, handshake_response, pick_protocol};

fn ws_dir() -> Option<PathBuf> {
    let d = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../node_modules/ws");
    if !d.join("lib/receiver.js").exists() {
        eprintln!("skipped: no ws library at {}", d.display());
        return None;
    }
    if Command::new("node").arg("--version").output().is_err() {
        eprintln!("skipped: no node");
        return None;
    }
    Some(d)
}

/// node running `script` (CommonJS, the ws folder as argv[2]) over these lines on stdin; its stdout's lines
fn node(ws: &PathBuf, script: &str, input: String) -> Vec<String> {
    let mut child = Command::new("node")
        .args(["-e", script, "--"])
        .arg(ws)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .spawn()
        .expect("node");
    let mut stdin = child.stdin.take().unwrap();
    let feeder = std::thread::spawn(move || stdin.write_all(input.as_bytes()).unwrap());
    let out = child.wait_with_output().unwrap();
    feeder.join().unwrap();
    assert!(out.status.success(), "node failed");
    String::from_utf8(out.stdout).unwrap().lines().map(str::to_string).collect()
}

// Each line: maxPayload, a space, the stream's bytes in hex.  Out: the events, '|' between them.  The error for a
// bad close code loses its code, as ours has none.
const RECEIVER: &str = r#"
const ws = process.argv[process.argv.length - 1];
const Receiver = require(ws + '/lib/receiver.js');
const { kStatusCode } = require(ws + '/lib/constants.js');
const rl = require('readline').createInterface({ input: process.stdin });
const out = [];
(async () => {
  for await (const line of rl) {
    const [max, hex] = line.split(' ');
    const r = new Receiver({ isServer: true, maxPayload: +max });
    const ev = [];
    const h = d => Buffer.from(d).toString('hex');
    r.on('message', (d, binary) => ev.push((binary ? 'B ' : 'T ') + h(d)));
    r.on('ping', d => ev.push('P ' + h(d)));
    r.on('pong', d => ev.push('O ' + h(d)));
    r.on('conclude', (code, reason) => ev.push('C ' + code + ' ' + h(reason)));
    r.on('error', e => ev.push('E ' + e[kStatusCode] + ' ' + e.message.replace(/^(Invalid WebSocket frame: invalid status code) \d+$/, '$1')));
    r.write(Buffer.from(hex || '', 'hex'));
    // the error comes on the next tick
    await new Promise(res => setImmediate(res));
    out.push(ev.join('|'));
  }
  process.stdout.write(out.map(s => s + '\n').join(''));
})();
"#;

fn events(got: &(Vec<Message>, ReadError)) -> String {
    let mut ev: Vec<String> = got
        .0
        .iter()
        .map(|m| match m {
            Message::Text(d) => format!("T {}", hex(d)),
            Message::Binary(d) => format!("B {}", hex(d)),
            Message::Ping(d) => format!("P {}", hex(d)),
            Message::Pong(d) => format!("O {}", hex(d)),
            Message::Close(code, d) => format!("C {code} {}", hex(d)),
        })
        .collect();
    if let ReadError::Protocol { code, reason } = &got.1 {
        ev.push(format!("E {code} {reason}"));
    }
    ev.join("|")
}

/// a payload of UTF-8 pieces: whole characters mostly, sometimes a byte of one, or one no UTF-8 has
fn utf8ish(r: &mut Rng, n: usize) -> Vec<u8> {
    let atoms: [&[u8]; 12] = [
        b"a",
        b"Z",
        b" ",
        "é".as_bytes(),
        "€".as_bytes(),
        "😀".as_bytes(),
        b"\xc3",
        b"\xa9",
        b"\xe2\x82",
        b"\xf0\x9f\x98",
        b"\xff",
        b"\xed\xa0\x80",
    ];
    let mut p = Vec::new();
    while p.len() < n {
        let a = if r.chance(95) { *r.pick(&atoms[..6]) } else { *r.pick(&atoms) };
        p.extend_from_slice(a);
    }
    p
}

/// A random stream of client frames: mostly well formed, with every way of breaking one ws knew sprinkled in.
fn random_stream(r: &mut Rng) -> (usize, Vec<u8>) {
    let max = *r.pick(&[65536, 300, 100, 0]);
    let mut out = Vec::new();
    // a message in fragments is open (as the frames are meant: the reader decides what they are)
    let mut open = false;
    for _ in 0..1 + r.below(6) {
        let kind = r.below(100);
        let (mut op, fin) = match kind {
            0..=19 => (1, true),
            20..=34 => (2, true),
            35..=59 if open || r.chance(10) => (0, r.chance(60)),
            35..=59 => (*r.pick(&[1, 2]), false),
            60..=71 => (9, true),
            72..=77 => (10, true),
            78..=94 => (8, true),
            _ => (r.below(16) as u8, r.chance(70)),
        };
        if r.chance(2) {
            op = r.below(16) as u8;
        }
        let fin = if r.chance(2) { !fin } else { fin };
        if op < 3 {
            open = !fin;
        }
        let control = (8..=10).contains(&op);
        let n = match (control, r.chance(85)) {
            (_, true) => r.below(24) as usize,
            (true, false) => 120 + r.below(10) as usize,
            (false, false) => 90 + r.below(320) as usize,
        };
        let payload = match op {
            8 => match r.below(10) {
                0 => vec![],
                1 => vec![r.next() as u8],
                _ => {
                    let code: u16 = match r.below(4) {
                        0 => r.next() as u16,
                        _ => *r.pick(&[0, 999, 1000, 1001, 1004, 1005, 1006, 1011, 1014, 1015, 2999, 3000, 4999, 5000]),
                    };
                    let mut p = code.to_be_bytes().to_vec();
                    let n = r.below(12) as usize;
                    p.extend(utf8ish(r, n));
                    p
                }
            },
            1 | 0 => utf8ish(r, n),
            _ => r.bytes(n),
        };
        let mut b0 = (op & 0x0f) | if fin { 0x80 } else { 0 };
        if r.chance(2) {
            b0 |= ((1 + r.below(7)) as u8) << 4;
        }
        let mask = if r.chance(2) {
            None
        } else if r.chance(10) {
            Some([0; 4])
        } else {
            let k = r.next().to_le_bytes();
            Some([k[0], k[1], k[2], k[3]])
        };
        if r.chance(2) {
            // a length no payload follows: over 2^53, or past every limit
            out.push(b0);
            let n = if r.chance(50) { r.next() | 1 << 60 } else { (1 << 53) - 1 - r.below(1000) };
            out.extend(length(n, mask.is_some(), Len::L64));
            out.extend(mask.unwrap_or([1, 2, 3, 4]));
            continue;
        }
        // a control frame in a long form is refused: rarely
        let len = if control && r.chance(90) { Len::Short } else { *r.pick(&[Len::Short, Len::Short, Len::Short, Len::L16, Len::L64]) };
        out.extend(raw_with(b0, &payload, mask, len));
    }
    if r.chance(8) {
        let cut = r.below(out.len() as u64 + 1) as usize;
        out.truncate(cut);
    }
    (max, out)
}

#[tokio::test]
async fn reader_against_ws_receiver() {
    let Some(ws) = ws_dir() else { return };
    let mut rng = Rng(0x6455_0001);
    let cases: Vec<(usize, Vec<u8>)> = (0..50000).map(|_| random_stream(&mut rng)).collect();
    let input: String = cases.iter().map(|(max, s)| format!("{max} {}\n", hex(s))).collect();
    let want = node(&ws, RECEIVER, input);
    assert_eq!(want.len(), cases.len());
    let (mut errors, mut messages) = (std::collections::BTreeMap::new(), 0);
    for (i, ((max, s), want)) in cases.iter().zip(&want).enumerate() {
        let whole = read_all(WsReader::new(tokio::io::empty(), Bytes::copy_from_slice(s), *max)).await;
        assert_eq!(events(&whole), *want, "case {i}: max {max}, stream {}", hex(s));
        // and in pieces of random sizes
        let sizes: Vec<usize> = (0..7).map(|_| 1 + rng.below(40) as usize).collect();
        let pieces = read_all(WsReader::new(Drip::sizes(s, sizes), Bytes::new(), *max)).await;
        assert_eq!(pieces, whole, "case {i} in pieces");
        if let ReadError::Protocol { code, reason } = whole.1 {
            *errors.entry((code, reason)).or_insert(0) += 1;
        }
        messages += whole.0.len();
    }
    eprintln!("{} streams, {messages} messages, the same as ws's; its errors:", cases.len());
    for ((code, reason), n) in &errors {
        eprintln!("  {n:5} {code} {reason}");
    }
    // the corpus reaches every rule
    assert!(messages > 30000, "{messages} messages");
    assert!(errors.len() >= 24, "{} kinds of errors", errors.len());
}

// Each line: a Sec-WebSocket-Key, a tab, the Sec-WebSocket-Protocol header in hex ("N": none).  Out: the 101 ws
// wrote, in hex, or "ERR" when it refused the handshake.
const HANDSHAKE: &str = r#"
const ws = process.argv[process.argv.length - 1];
const WebSocketServer = require(ws + '/lib/websocket-server.js');
const { Duplex } = require('stream');
const wss = new WebSocketServer({ noServer: true, perMessageDeflate: false, clientTracking: false, maxPayload: 65536, autoPong: false });
let refused = false;
wss.on('wsClientError', () => { refused = true; });
const rl = require('readline').createInterface({ input: process.stdin });
const out = [];
rl.on('line', line => {
  const [key, proto] = line.split('\t');
  const chunks = [];
  const sock = new Duplex({ read() {}, write(c, e, cb) { chunks.push(Buffer.from(c)); cb(); } });
  const headers = { upgrade: 'websocket', 'sec-websocket-key': key, 'sec-websocket-version': '13' };
  if (proto !== 'N') headers['sec-websocket-protocol'] = Buffer.from(proto, 'hex').toString('latin1');
  refused = false;
  wss.handleUpgrade({ method: 'GET', headers, url: '/lan' }, sock, Buffer.alloc(0), () => {});
  out.push(refused ? 'ERR' : Buffer.concat(chunks).toString('hex'));
  sock.destroy();
});
rl.on('close', () => process.stdout.write(out.map(s => s + '\n').join('')));
"#;

#[test]
fn handshake_against_ws_server() {
    let Some(ws) = ws_dir() else { return };
    let mut rng = Rng(0x6455_0002);
    const B64: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let pieces = ["a", "b", "chat", "x-1", "!", "~", " ", "\t", ",", ", ", ";", "\"", "é", "=", "/", "A", "a,b", "\u{1}"];
    let mut cases = Vec::new();
    for i in 0..20000 {
        let key: String = (0..22).map(|_| *rng.pick(B64) as char).chain("==".chars()).collect();
        let proto = if i % 10 == 0 {
            None
        } else {
            let n = rng.below(7);
            Some((0..n).map(|_| *rng.pick(&pieces)).collect::<String>())
        };
        cases.push((key, proto));
    }
    // headers as Node gives them: latin1 strings, so only ASCII ones are the same text on both sides (any other
    // character is refused on both)
    let input: String = cases
        .iter()
        .map(|(k, p)| {
            let h = match p {
                None => "N".to_string(),
                Some(p) if p.is_ascii() => hex(p.as_bytes()),
                // é as latin1 (one byte): no token character either way
                Some(p) => hex(&p.chars().map(|c| if c.is_ascii() { c as u8 } else { 0xe9 }).collect::<Vec<u8>>()),
            };
            format!("{k}\t{h}\n")
        })
        .collect();
    let want = node(&ws, HANDSHAKE, input);
    assert_eq!(want.len(), cases.len());
    let (mut picked, mut refused) = (0, 0);
    for ((key, proto), want) in cases.iter().zip(&want) {
        let got = match pick_protocol(proto.as_deref()) {
            Err(()) => {
                refused += 1;
                "ERR".to_string()
            }
            Ok(p) => {
                picked += p.is_some() as usize;
                hex(&handshake_response(key, p.as_deref()))
            }
        };
        assert_eq!(got, *want, "key {key}, Sec-WebSocket-Protocol {proto:?}");
    }
    eprintln!("{} handshakes: {picked} with a protocol, {refused} refused, the same as ws's", cases.len());
    assert!(picked > 2000 && refused > 2000);
}
