//! The handshake (accept key, subprotocol, the 101) and the server's own frames.

use nfsu2_ws::{accept_key, frame, handshake_response, pick_protocol};

#[test]
fn accept_key_of_rfc_6455() {
    // RFC 6455 1.3's example
    assert_eq!(accept_key("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

#[test]
fn the_101() {
    let want = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\
                Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    assert_eq!(String::from_utf8(handshake_response("dGhlIHNhbXBsZSBub25jZQ==", None)).unwrap(), want);
    let want = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\
                Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nSec-WebSocket-Protocol: chat\r\n\r\n";
    assert_eq!(String::from_utf8(handshake_response("dGhlIHNhbXBsZSBub25jZQ==", Some("chat"))).unwrap(), want);
    // ws: `if (protocol)`: an empty one is no header
    assert_eq!(handshake_response("dGhlIHNhbXBsZSBub25jZQ==", Some("")), handshake_response("dGhlIHNhbXBsZSBub25jZQ==", None));
}

fn pick(h: &str) -> Result<Option<String>, ()> {
    pick_protocol(Some(h))
}
fn ok(p: &str) -> Result<Option<String>, ()> {
    Ok(Some(p.to_string()))
}

#[test]
fn subprotocols_as_ws_reads_them() {
    // none: none picked
    assert_eq!(pick_protocol(None), Ok(None));
    // subprotocol.js's own cases (ws's test/subprotocol.test.js), and the first is the one picked
    assert_eq!(pick("foo"), ok("foo"));
    assert_eq!(pick("foo,bar,baz"), ok("foo"));
    assert_eq!(pick("foo , bar,baz"), ok("foo"));
    assert_eq!(pick("foo\t,\tbar , baz"), ok("foo"));
    assert_eq!(pick("bar, foo"), ok("bar"));
    // spaces and tabs between tokens, never first, never last
    assert_eq!(pick("a, \t b"), ok("a"));
    assert_eq!(pick(" foo"), Err(()));
    assert_eq!(pick("\tfoo"), Err(()));
    assert_eq!(pick("foo "), Err(()));
    assert_eq!(pick("foo\t"), Err(()));
    assert_eq!(pick("foo bar"), Err(()));
    assert_eq!(pick("foo\tbar"), Err(()));
    // empty: the header, an item, the end after a comma
    assert_eq!(pick(""), Err(()));
    assert_eq!(pick(","), Err(()));
    assert_eq!(pick(",foo"), Err(()));
    assert_eq!(pick("foo,"), Err(()));
    assert_eq!(pick("foo,,bar"), Err(()));
    assert_eq!(pick("foo, ,bar"), Err(()));
    assert_eq!(pick("foo, "), Err(()));
    // duplicates, wherever they are
    assert_eq!(pick("foo,foo"), Err(()));
    assert_eq!(pick("foo, bar, foo"), Err(()));
    assert_eq!(pick("bar,foo , foo"), Err(()));
    assert_eq!(pick("foo,Foo"), ok("foo"));
    // characters that are no token's
    for c in ['"', '(', ')', '/', ':', ';', '<', '=', '>', '?', '@', '[', '\\', ']', '{', '}', '\0', '\r', '\n', '\x7f', 'é', '😀'] {
        assert_eq!(pick(&format!("fo{c}o")), Err(()), "{c:?}");
    }
    // every token character
    assert_eq!(pick("!#$%&'*+-.^_`|~09AZaz"), ok("!#$%&'*+-.^_`|~09AZaz"));
}

#[test]
fn many_subprotocols() {
    // a header of thousands of tokens is read in one pass
    let h: Vec<String> = (0..20000).map(|i| format!("p{i}")).collect();
    assert_eq!(pick(&h.join(", ")), ok("p0"));
    let mut d = h.clone();
    d.push("p0".into());
    assert_eq!(pick(&d.join(",")), Err(()));
}

#[test]
fn frame_headers() {
    let header = |op, len| {
        let mut v = Vec::new();
        frame::header(op, len, &mut v);
        v
    };
    assert_eq!(header(frame::TEXT, 0), [0x81, 0]);
    assert_eq!(header(frame::BINARY, 125), [0x82, 125]);
    assert_eq!(header(frame::BINARY, 126), [0x82, 126, 0, 126]);
    assert_eq!(header(frame::BINARY, 65535), [0x82, 126, 0xff, 0xff]);
    assert_eq!(header(frame::BINARY, 65536), [0x82, 127, 0, 0, 0, 0, 0, 1, 0, 0]);
    assert_eq!(header(frame::BINARY, 0x0012_3456_789a), [0x82, 127, 0, 0, 0, 0x12, 0x34, 0x56, 0x78, 0x9a]);
    assert_eq!(header(frame::CLOSE, 2), [0x88, 2]);
    assert_eq!(header(frame::PING, 1), [0x89, 1]);
    assert_eq!(header(frame::PONG, 125), [0x8a, 125]);
    // encode appends: many frames in one buffer
    let mut v = vec![0xee];
    frame::encode(frame::TEXT, b"Hello", &mut v);
    frame::encode(frame::PONG, b"", &mut v);
    assert_eq!(v, [0xee, 0x81, 5, b'H', b'e', b'l', b'l', b'o', 0x8a, 0]);
    let mut v = Vec::new();
    frame::encode(frame::BINARY, &[7; 300], &mut v);
    assert_eq!((v.len(), &v[..4]), (304, &[0x82, 126, 1, 44][..]));
}

#[test]
fn close_payloads() {
    assert_eq!(frame::close_payload(1000, ""), [0x03, 0xe8]);
    assert_eq!(frame::close_payload(1013, "LAN full"), b"\x03\xf5LAN full");
    assert_eq!(frame::close_payload(4999, "é"), [0x13, 0x87, 0xc3, 0xa9]);
    // 1005 is no code: the empty payload of ws's close() without one
    assert_eq!(frame::close_payload(1005, "ignored"), Vec::<u8>::new());
    // 123 bytes of reason at most, cut at a character
    let p = frame::close_payload(1000, &"x".repeat(200));
    assert_eq!(p.len(), 125);
    let p = frame::close_payload(1000, &format!("{}é", "x".repeat(122)));
    assert_eq!(p.len(), 124);
    assert!(std::str::from_utf8(&p[2..]).is_ok());
}
