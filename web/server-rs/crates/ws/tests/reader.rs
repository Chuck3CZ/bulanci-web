//! WsReader against hand-built frames: every rule of ws's receiver.js, each read whole and in pieces.

mod common;

use std::time::Duration;

use bytes::Bytes;
use common::*;
use nfsu2_ws::{Message, ReadError, WsReader};
use tokio::io::AsyncWriteExt;

const MAX: usize = 65536;

const MASK_SET: &str = "Invalid WebSocket frame: MASK must be set";
const RSV1: &str = "Invalid WebSocket frame: RSV1 must be clear";
const RSV23: &str = "Invalid WebSocket frame: RSV2 and RSV3 must be clear";
const FIN: &str = "Invalid WebSocket frame: FIN must be set";
const UTF8: &str = "Invalid WebSocket frame: invalid UTF-8 sequence";
const MAX_PAYLOAD: &str = "Max payload size exceeded";
const OVER_2_53: &str = "Unsupported WebSocket frame: payload length > 2^53 - 1";
const STATUS: &str = "Invalid WebSocket frame: invalid status code";

#[tokio::test]
async fn rfc_6455_examples() {
    // 5.7: a single-frame masked text message, and a masked ping, both "Hello"
    let hello = [0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58];
    assert_eq!(read_bytes(&hello, MAX).await, (vec![t("Hello")], ReadError::Closed));
    let ping = [0x89, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58];
    assert_eq!(read_bytes(&ping, MAX).await, (vec![Message::Ping(Bytes::from_static(b"Hello"))], ReadError::Closed));
    // its unmasked examples are a server's frames: from a client they break the protocol
    let unmasked = [0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f];
    assert_eq!(read_bytes(&unmasked, MAX).await, (vec![], err(1002, MASK_SET)));
    let fragmented = [0x01, 0x03, 0x48, 0x65, 0x6c, 0x80, 0x02, 0x6c, 0x6f];
    assert_eq!(read_bytes(&fragmented, MAX).await, (vec![], err(1002, MASK_SET)));
}

#[tokio::test]
async fn single_frames() {
    let data = cat(&[text(b"Hello"), binary(&[0, 1, 2, 255]), text(b""), binary(b""), pong(b"x"), ping(b"")]);
    let want = vec![t("Hello"), b(&[0, 1, 2, 255]), t(""), b(b""), Message::Pong(Bytes::from_static(b"x")), Message::Ping(Bytes::new())];
    assert_eq!(read_bytes(&data, MAX).await, (want, ReadError::Closed));
}

#[tokio::test]
async fn a_mask_of_zeros() {
    let f = raw_with(0x81, b"plain", Some([0; 4]), Len::Short);
    assert_eq!(read_bytes(&f, MAX).await, (vec![t("plain")], ReadError::Closed));
}

#[tokio::test]
async fn lengths_in_every_form() {
    for n in [0, 1, 125, 126, 127, 1000, 65535, 65536] {
        let p: Vec<u8> = (0..n).map(|i| (i * 7) as u8).collect();
        let (m, e) = read_bytes(&binary(&p), MAX).await;
        assert_eq!((m, e), (vec![b(&p)], ReadError::Closed), "length {n}");
    }
    // a length in a longer form than it needs: ws took it
    for len in [Len::L16, Len::L64] {
        let f = raw_with(0x81, b"short", Some(MASK), len);
        assert_eq!(read_bytes(&f, MAX).await, (vec![t("short")], ReadError::Closed));
    }
    let f = raw_with(0x82, &[9; 300], Some(MASK), Len::L64);
    assert_eq!(read_bytes(&f, MAX).await, (vec![b(&[9; 300])], ReadError::Closed));
}

#[tokio::test]
async fn max_payload_over_a_frame() {
    // exactly max_payload is fine, one byte more a 1009, refused on its header before the payload comes
    let p = vec![1u8; MAX + 1];
    assert_eq!(read_bytes(&binary(&p[..MAX]), MAX).await.0, vec![b(&p[..MAX])]);
    let f = binary(&p);
    assert_eq!(read_bytes(&f, MAX).await, (vec![], err(1009, MAX_PAYLOAD)));
    assert_eq!(read_bytes(&f[..14], MAX).await, (vec![], err(1009, MAX_PAYLOAD)));
    // text the same, and a small limit
    assert_eq!(read_bytes(&text(&[b'a'; 11]), 10).await, (vec![], err(1009, MAX_PAYLOAD)));
    assert_eq!(read_bytes(&text(&[b'a'; 10]), 10).await.0, vec![t("aaaaaaaaaa")]);
}

#[tokio::test]
async fn max_payload_over_a_message() {
    // counted over all of a message's fragments
    let a = vec![2u8; 40000];
    let ok = cat(&[raw(0x02, &a), cont(true, &[3; MAX - 40000])]);
    let mut whole = a.clone();
    whole.extend([3; MAX - 40000]);
    assert_eq!(read_bytes(&ok, MAX).await, (vec![b(&whole)], ReadError::Closed));
    let over = cat(&[raw(0x02, &a), cont(true, &[3; MAX - 40000 + 1])]);
    assert_eq!(read_bytes(&over, MAX).await, (vec![], err(1009, MAX_PAYLOAD)));
    // the second fragment's header alone is enough
    let header_only = &over[..6 + 40000 + 8];
    assert_eq!(read_bytes(header_only, MAX).await, (vec![], err(1009, MAX_PAYLOAD)));
    // control frames between the fragments do not count; the next message starts from 0
    let pings = cat(&[raw(0x01, &[b'a'; 6]), ping(&[0; 125]), cont(true, &[b'b'; 4]), text(&[b'c'; 10])]);
    let (m, e) = read_bytes(&pings, 10).await;
    assert_eq!(e, ReadError::Closed);
    assert_eq!(m, vec![Message::Ping(Bytes::from(vec![0; 125])), t("aaaaaabbbb"), t("cccccccccc")]);
}

#[tokio::test]
async fn no_max_payload() {
    // ws: maxPayload 0 is no limit
    let p = vec![5u8; 200_000];
    assert_eq!(read_bytes(&binary(&p), 0).await, (vec![b(&p)], ReadError::Closed));
    // a length announced and never sent reserves nothing: the reader waits for it (here: the end)
    let mut f = vec![0x82];
    f.extend(length((1u64 << 53) - 1, true, Len::L64));
    f.extend(MASK);
    f.extend([0; 100]);
    assert_eq!(read_all(WsReader::new(Drip::new(&f, 16), Bytes::new(), 0)).await, (vec![], ReadError::Closed));
}

#[tokio::test]
async fn lengths_over_2_53() {
    // ws counted in JS numbers: a length over 2^53 - 1 is a 1009 of its own, before max_payload
    let mut f = vec![0x82];
    f.extend(length(1u64 << 53, true, Len::L64));
    assert_eq!(read_bytes(&f, MAX).await, (vec![], err(1009, OVER_2_53)));
    assert_eq!(read_bytes(&f, 0).await, (vec![], err(1009, OVER_2_53)));
    let mut f = vec![0x82];
    f.extend(length(u64::MAX, true, Len::L64));
    assert_eq!(read_bytes(&f, 0).await, (vec![], err(1009, OVER_2_53)));
    let mut f = vec![0x82];
    f.extend(length((1u64 << 53) - 1, true, Len::L64));
    assert_eq!(read_bytes(&f, MAX).await, (vec![], err(1009, MAX_PAYLOAD)));
}

#[tokio::test]
async fn rsv_bits() {
    // no extension was ever accepted (perMessageDeflate: false): RSV1 is a 1002 too; RSV2/3 are checked first
    for (b0, why) in [(0xc1, RSV1), (0xa1, RSV23), (0x91, RSV23), (0xb1, RSV23), (0xe1, RSV23), (0xf1, RSV23)] {
        assert_eq!(read_bytes(&raw(b0, b"x"), MAX).await, (vec![], err(1002, why)), "{b0:#x}");
    }
    // on a continuation and on a control frame
    let f = cat(&[raw(0x01, b"a"), raw(0xc0, b"b")]);
    assert_eq!(read_bytes(&f, MAX).await, (vec![], err(1002, RSV1)));
    assert_eq!(read_bytes(&raw(0xc9, b""), MAX).await, (vec![], err(1002, RSV1)));
    // an unmasked frame with RSV1: the RSV is seen first
    assert_eq!(read_bytes(&raw_with(0xc1, b"x", None, Len::Short), MAX).await, (vec![], err(1002, RSV1)));
}

#[tokio::test]
async fn every_opcode() {
    const NAMES: [&str; 16] = [
        "Invalid WebSocket frame: invalid opcode 0",
        "Invalid WebSocket frame: invalid opcode 1",
        "Invalid WebSocket frame: invalid opcode 2",
        "Invalid WebSocket frame: invalid opcode 3",
        "Invalid WebSocket frame: invalid opcode 4",
        "Invalid WebSocket frame: invalid opcode 5",
        "Invalid WebSocket frame: invalid opcode 6",
        "Invalid WebSocket frame: invalid opcode 7",
        "",
        "",
        "",
        "Invalid WebSocket frame: invalid opcode 11",
        "Invalid WebSocket frame: invalid opcode 12",
        "Invalid WebSocket frame: invalid opcode 13",
        "Invalid WebSocket frame: invalid opcode 14",
        "Invalid WebSocket frame: invalid opcode 15",
    ];
    for op in 0u8..16 {
        for fin in [true, false] {
            let b0 = op | if fin { 0x80 } else { 0 };
            let got = read_bytes(&raw(b0, b"ab"), MAX).await;
            let want = match (op, fin) {
                (1, true) => (vec![t("ab")], ReadError::Closed),
                (2, true) => (vec![b(b"ab")], ReadError::Closed),
                // the first fragment of a message that never ends
                (1 | 2, false) => (vec![], ReadError::Closed),
                // "ab" as a close code is 0x6162: not one a close frame may carry
                (8, true) => (vec![], err(1002, STATUS)),
                (9, true) => (vec![Message::Ping(Bytes::from_static(b"ab"))], ReadError::Closed),
                (10, true) => (vec![Message::Pong(Bytes::from_static(b"ab"))], ReadError::Closed),
                (8..=10, false) => (vec![], err(1002, FIN)),
                // 0: a continuation with no message to continue
                _ => (vec![], err(1002, NAMES[op as usize])),
            };
            assert_eq!(got, want, "opcode {op} fin {fin}");
        }
    }
}

#[tokio::test]
async fn continuation_rules() {
    // a continuation with nothing to continue
    assert_eq!(read_bytes(&cont(true, b"x"), MAX).await, (vec![], err(1002, "Invalid WebSocket frame: invalid opcode 0")));
    // after a whole message there is nothing to continue either
    let f = cat(&[text(b"a"), cont(true, b"b")]);
    assert_eq!(read_bytes(&f, MAX).await, (vec![t("a")], err(1002, "Invalid WebSocket frame: invalid opcode 0")));
    // a new text or binary message while one is in fragments
    let f = cat(&[raw(0x01, b"a"), text(b"b")]);
    assert_eq!(read_bytes(&f, MAX).await, (vec![], err(1002, "Invalid WebSocket frame: invalid opcode 1")));
    let f = cat(&[raw(0x01, b"a"), raw(0x02, b"b")]);
    assert_eq!(read_bytes(&f, MAX).await, (vec![], err(1002, "Invalid WebSocket frame: invalid opcode 2")));
}

#[tokio::test]
async fn fragments_joined() {
    let f = cat(&[raw(0x01, b"Hel"), cont(true, b"lo")]);
    assert_eq!(read_bytes(&f, MAX).await, (vec![t("Hello")], ReadError::Closed));
    // empty fragments anywhere, many fragments, binary, and a message after
    let f = cat(&[
        raw(0x02, b""),
        cont(false, &[1, 2]),
        cont(false, b""),
        cont(false, &[3]),
        cont(true, &[4, 5, 6]),
        raw(0x01, b""),
        cont(true, b""),
        text(b"next"),
    ]);
    assert_eq!(read_bytes(&f, MAX).await, (vec![b(&[1, 2, 3, 4, 5, 6]), t(""), t("next")], ReadError::Closed));
}

#[tokio::test]
async fn control_frames_between_fragments() {
    let f = cat(&[raw(0x01, b"one "), ping(b"p"), cont(false, b"two "), pong(b"q"), cont(true, b"three")]);
    let want = vec![Message::Ping(Bytes::from_static(b"p")), Message::Pong(Bytes::from_static(b"q")), t("one two three")];
    assert_eq!(read_bytes(&f, MAX).await, (want, ReadError::Closed));
    // a close in the middle ends it all
    let f = cat(&[raw(0x02, b"half"), close(1000, b""), cont(true, b"never")]);
    assert_eq!(read_bytes(&f, MAX).await, (vec![closed(1000, "")], ReadError::Closed));
}

#[tokio::test]
async fn control_frame_lengths() {
    let p125 = vec![7u8; 125];
    assert_eq!(read_bytes(&ping(&p125), MAX).await.0, vec![Message::Ping(Bytes::from(p125.clone()))]);
    // over 125 bytes: refused on the 7-bit length (126 and 127 are what it can say)
    let f = raw_with(0x89, &[7; 126], Some(MASK), Len::Short);
    assert_eq!(read_bytes(&f, MAX).await, (vec![], err(1002, "Invalid WebSocket frame: invalid payload length 126")));
    let f = raw_with(0x8a, &[7; 5], Some(MASK), Len::L64);
    assert_eq!(read_bytes(&f, MAX).await, (vec![], err(1002, "Invalid WebSocket frame: invalid payload length 127")));
    let f = raw_with(0x88, &[7; 5], Some(MASK), Len::L16);
    assert_eq!(read_bytes(&f, MAX).await, (vec![], err(1002, "Invalid WebSocket frame: invalid payload length 126")));
    // a close frame of 1 byte (half a code)
    assert_eq!(read_bytes(&raw(0x88, &[3]), MAX).await, (vec![], err(1002, "Invalid WebSocket frame: invalid payload length 1")));
    // FIN is checked before the length; the length before the mask
    assert_eq!(read_bytes(&raw_with(0x09, &[0; 126], Some(MASK), Len::Short), MAX).await, (vec![], err(1002, FIN)));
    let f = raw_with(0x89, &[0; 126], None, Len::Short);
    assert_eq!(read_bytes(&f, MAX).await, (vec![], err(1002, "Invalid WebSocket frame: invalid payload length 126")));
}

#[tokio::test]
async fn masks_required() {
    for b0 in [0x81, 0x82, 0x89, 0x8a, 0x88, 0x01] {
        let f = raw_with(b0, b"", None, Len::Short);
        assert_eq!(read_bytes(&f, MAX).await, (vec![], err(1002, MASK_SET)), "{b0:#x}");
    }
    // a frame after good ones
    let f = cat(&[text(b"a"), raw_with(0x82, b"b", None, Len::Short)]);
    assert_eq!(read_bytes(&f, MAX).await, (vec![t("a")], err(1002, MASK_SET)));
}

#[tokio::test]
async fn text_must_be_utf8() {
    for bad in [
        &[0xff][..],
        &[0x80],
        &[0xc3],                   // cut short
        &[0xc0, 0x80],             // overlong
        &[0xc1, 0xbf],             // overlong
        &[0xe0, 0x80, 0x80],       // overlong
        &[0xed, 0xa0, 0x80],       // a surrogate
        &[0xf0, 0x80, 0x80, 0x80], // overlong
        &[0xf4, 0x90, 0x80, 0x80], // over U+10FFFF
        &[0xf5, 0x80, 0x80, 0x80],
        &[0xe2, 0x82],
        b"ok then \xe2\x82",
    ] {
        assert_eq!(read_bytes(&text(bad), MAX).await, (vec![], err(1007, UTF8)), "{bad:x?}");
        // binary is anything
        assert_eq!(read_bytes(&binary(bad), MAX).await.0, vec![b(bad)]);
    }
    let good = "a\u{7f}\u{80}\u{7ff}\u{800}\u{d7ff}\u{e000}\u{fffd}\u{10000}\u{10ffff} é € 😀";
    assert_eq!(read_bytes(&text(good.as_bytes()), MAX).await.0, vec![t(good)]);
}

#[tokio::test]
async fn utf8_across_fragments() {
    // a character split across fragments is fine: the whole message is checked
    let f = cat(&[raw(0x01, b"caf\xc3"), cont(true, b"\xa9")]);
    assert_eq!(read_bytes(&f, MAX).await.0, vec![t("café")]);
    let f = cat(&[raw(0x01, b"\xe2"), ping(b""), cont(false, b"\x82"), cont(true, b"\xac!")]);
    assert_eq!(read_bytes(&f, MAX).await.0, vec![Message::Ping(Bytes::new()), t("€!")]);
    let f = cat(&[raw(0x01, b"\xf0\x9f"), cont(false, b"\x98"), cont(true, b"\x80")]);
    assert_eq!(read_bytes(&f, MAX).await.0, vec![t("😀")]);
    // a sequence broken across fragments
    let f = cat(&[raw(0x01, b"\xc3"), cont(true, b"(")]);
    assert_eq!(read_bytes(&f, MAX).await, (vec![], err(1007, UTF8)));
    // a bad first fragment is refused when the message ends, not before (ws checked the whole message)
    let f = cat(&[raw(0x01, b"\xff"), ping(b"still")]);
    assert_eq!(read_bytes(&f, MAX).await, (vec![Message::Ping(Bytes::from_static(b"still"))], ReadError::Closed));
    let f = cat(&[raw(0x01, b"\xff"), ping(b"still"), cont(true, b"end")]);
    assert_eq!(read_bytes(&f, MAX).await, (vec![Message::Ping(Bytes::from_static(b"still"))], err(1007, UTF8)));
}

#[tokio::test]
async fn close_frames() {
    assert_eq!(read_bytes(&raw(0x88, b""), MAX).await, (vec![closed(1005, "")], ReadError::Closed));
    assert_eq!(read_bytes(&close(1000, b"bye"), MAX).await, (vec![closed(1000, "bye")], ReadError::Closed));
    let reason = "x".repeat(123);
    assert_eq!(read_bytes(&close(4999, reason.as_bytes()), MAX).await.0, vec![closed(4999, &reason)]);
    for code in [1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 3000, 3999, 4000, 4999] {
        assert_eq!(read_bytes(&close(code, b"r"), MAX).await, (vec![closed(code, "r")], ReadError::Closed), "{code}");
    }
    for code in [0, 999, 1004, 1005, 1006, 1015, 1016, 2000, 2999, 5000, 65535] {
        assert_eq!(read_bytes(&close(code, b""), MAX).await, (vec![], err(1002, STATUS)), "{code}");
    }
    // the reason must be UTF-8
    assert_eq!(read_bytes(&close(1000, b"\xc3"), MAX).await, (vec![], err(1007, UTF8)));
    assert_eq!(read_bytes(&close(1000, "é".as_bytes()), MAX).await.0, vec![closed(1000, "é")]);
}

#[tokio::test]
async fn nothing_counts_after_a_close() {
    // ws stopped reading at a close frame: what follows, good or broken, is never seen
    let f = cat(&[text(b"a"), close(1001, b""), text(b"b"), raw_with(0x81, b"c", None, Len::Short), ping(b"d")]);
    assert_eq!(read_bytes(&f, MAX).await, (vec![t("a"), closed(1001, "")], ReadError::Closed));
    // and next() goes on saying Closed
    let mut r = WsReader::new(tokio::io::empty(), Bytes::from(close(1000, b"")), MAX);
    assert_eq!(r.next().await, Ok(closed(1000, "")));
    assert_eq!(r.next().await, Err(ReadError::Closed));
    assert_eq!(r.next().await, Err(ReadError::Closed));
}

#[tokio::test]
async fn nothing_counts_after_a_protocol_error() {
    let f = cat(&[raw(0x83, b""), text(b"later")]);
    let mut r = WsReader::new(Drip::new(&f, 3), Bytes::new(), MAX);
    assert_eq!(r.next().await, Err(err(1002, "Invalid WebSocket frame: invalid opcode 3")));
    assert_eq!(r.next().await, Err(ReadError::Closed));
    assert_eq!(r.next().await, Err(ReadError::Closed));
}

#[tokio::test]
async fn the_end_of_the_connection() {
    let f = text(b"Hello");
    for cut in 0..f.len() {
        assert_eq!(read_bytes(&f[..cut], MAX).await, (vec![], ReadError::Closed), "cut at {cut}");
    }
    // an I/O error is the same as the end
    let mut r = WsReader::new(Failing(cat(&[text(b"a"), text(b"b")])), Bytes::new(), MAX);
    assert_eq!(r.next().await, Ok(t("a")));
    assert_eq!(r.next().await, Ok(t("b")));
    assert_eq!(r.next().await, Err(ReadError::Closed));
    let mut r = WsReader::new(Failing(text(b"a")[..4].to_vec()), Bytes::new(), MAX);
    assert_eq!(r.next().await, Err(ReadError::Closed));
}

#[tokio::test]
async fn pre_and_the_connection_together() {
    // what the HTTP server read past the head comes first
    let f = cat(&[text(b"first"), text(b"second"), text(b"third")]);
    for split in 0..=f.len() {
        let r = WsReader::new(Drip::new(&f[split..], 5), Bytes::copy_from_slice(&f[..split]), MAX);
        assert_eq!(read_all(r).await, (vec![t("first"), t("second"), t("third")], ReadError::Closed), "split {split}");
    }
}

#[tokio::test]
async fn a_single_frame_message_is_not_copied() {
    // payloads are views of the read block: two frames read together sit side by side in it, 6 header bytes apart
    let f = cat(&[binary(&[1; 100]), binary(&[2; 50]), raw(0x02, &[3; 10]), cont(true, &[4; 10])]);
    let n = f.len();
    let mut r = WsReader::new(tokio::io::empty(), Bytes::from(f), MAX);
    let (Ok(Message::Binary(a)), Ok(Message::Binary(b))) = (r.next().await, r.next().await) else { panic!() };
    assert_eq!(a.as_ptr() as usize + a.len() + 6, b.as_ptr() as usize);
    // a message in fragments is joined into a buffer of its own
    let Ok(Message::Binary(c)) = r.next().await else { panic!() };
    assert_eq!(&c[..], &[[3; 10], [4; 10]].concat()[..]);
    let block = a.as_ptr() as usize - 6..a.as_ptr() as usize + n;
    assert!(!block.contains(&(c.as_ptr() as usize)));
}

#[tokio::test]
async fn many_frames_in_one_read() {
    let frames: Vec<Vec<u8>> = (0..5000u32).map(|i| binary(&i.to_be_bytes())).collect();
    let (m, e) = read_bytes(&frames.concat(), MAX).await;
    assert_eq!(e, ReadError::Closed);
    assert_eq!(m, (0..5000u32).map(|i| b(&i.to_be_bytes())).collect::<Vec<_>>());
}

#[tokio::test]
async fn large_frames_in_random_pieces() {
    let mut rng = Rng(0x5eed);
    let mut frames = Vec::new();
    let mut want = Vec::new();
    for _ in 0..40 {
        let n = *rng.pick(&[0, 1, 125, 126, 4000, 16 << 10, (16 << 10) + 1, 40000, MAX]);
        let p = rng.bytes(n);
        frames.push(binary(&p));
        want.push(b(&p));
    }
    let data = frames.concat();
    for seed in 1..6 {
        let mut rng = Rng(seed);
        let sizes: Vec<usize> = (0..97).map(|_| 1 + rng.below(20000) as usize).collect();
        let got = read_all(WsReader::new(Drip::sizes(&data, sizes), Bytes::new(), MAX)).await;
        assert_eq!(got, (want.clone(), ReadError::Closed), "seed {seed}");
    }
}

#[tokio::test]
async fn cancel_safe() {
    // next() dropped by a timeout while a frame is half there (a select! with a timer): nothing is lost
    let f = cat(&[raw(0x01, b"frag"), ping(b"p"), cont(true, b"ment"), binary(&[9; 300])]);
    let (mut client, server) = tokio::io::duplex(1 << 16);
    let mut r = WsReader::new(server, Bytes::new(), MAX);
    let writer = tokio::spawn(async move {
        for piece in f.chunks(7) {
            client.write_all(piece).await.unwrap();
            tokio::time::sleep(Duration::from_millis(2)).await;
        }
        client
    });
    let (mut got, mut timeouts) = (Vec::new(), 0);
    while got.len() < 3 {
        match tokio::time::timeout(Duration::from_micros(500), r.next()).await {
            Ok(m) => got.push(m.unwrap()),
            Err(_) => timeouts += 1,
        }
    }
    drop(writer.await.unwrap());
    assert_eq!(got, vec![Message::Ping(Bytes::from_static(b"p")), t("fragment"), b(&[9; 300])]);
    assert!(timeouts > 0, "the timeout never fired");
    assert_eq!(r.next().await, Err(ReadError::Closed));
}
