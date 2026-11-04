//! A round trip with another implementation: tokio-tungstenite as the client, an echo server made of this crate's
//! parts (the handshake, WsReader, frame) on a real TCP connection.

use bytes::Bytes;
use futures_util::{SinkExt, StreamExt};
use nfsu2_ws::{Message, ReadError, WsReader, frame, handshake_response, pick_protocol};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;
use tokio_tungstenite::tungstenite::client::IntoClientRequest;
use tokio_tungstenite::tungstenite::http::HeaderValue;
use tokio_tungstenite::tungstenite::protocol::CloseFrame;
use tokio_tungstenite::tungstenite::protocol::frame::Frame;
use tokio_tungstenite::tungstenite::protocol::frame::coding::{Data, OpCode};
use tokio_tungstenite::tungstenite::{self as tung, Message as TMessage};

/// a header of the request's head
fn header(head: &str, name: &str) -> Option<String> {
    head.split("\r\n").skip(1).find_map(|l| {
        let (n, v) = l.split_once(':')?;
        n.trim().eq_ignore_ascii_case(name).then(|| v.trim().to_string())
    })
}

/// One connection's server: the handshake, then every message echoed, a ping answered with its pong, a close frame
/// with ws's answer (the same code and reason), a protocol error with its close frame.  What it saw comes back.
async fn echo_server() -> (String, tokio::task::JoinHandle<Vec<String>>) {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let url = format!("ws://{}/lan", listener.local_addr().unwrap());
    let task = tokio::spawn(async move {
        let (sock, _) = listener.accept().await.unwrap();
        sock.set_nodelay(true).unwrap();
        let (mut rd, mut wr) = sock.into_split();
        let mut head = Vec::new();
        let end = loop {
            let mut tmp = [0u8; 1024];
            let n = rd.read(&mut tmp).await.unwrap();
            assert!(n > 0, "no handshake");
            head.extend_from_slice(&tmp[..n]);
            if let Some(i) = head.windows(4).position(|w| w == b"\r\n\r\n") {
                break i + 4;
            }
        };
        let pre = Bytes::copy_from_slice(&head[end..]);
        let head = String::from_utf8(head[..end].to_vec()).unwrap();
        let key = header(&head, "sec-websocket-key").unwrap();
        let protocol = pick_protocol(header(&head, "sec-websocket-protocol").as_deref()).unwrap();
        wr.write_all(&handshake_response(&key, protocol.as_deref())).await.unwrap();
        let mut r = WsReader::new(rd, pre, 65536);
        let mut seen = Vec::new();
        loop {
            let mut out = Vec::new();
            let last = match r.next().await {
                Ok(Message::Text(t)) => {
                    seen.push(format!("text {}", t.len()));
                    frame::encode(frame::TEXT, &t, &mut out);
                    false
                }
                Ok(Message::Binary(b)) => {
                    seen.push(format!("binary {}", b.len()));
                    frame::encode(frame::BINARY, &b, &mut out);
                    false
                }
                Ok(Message::Ping(p)) => {
                    seen.push(format!("ping {}", String::from_utf8_lossy(&p)));
                    frame::encode(frame::PONG, &p, &mut out);
                    false
                }
                Ok(Message::Pong(p)) => {
                    seen.push(format!("pong {}", String::from_utf8_lossy(&p)));
                    false
                }
                Ok(Message::Close(code, reason)) => {
                    let reason = std::str::from_utf8(&reason).unwrap();
                    seen.push(format!("close {code} {reason}"));
                    frame::encode(frame::CLOSE, &frame::close_payload(code, reason), &mut out);
                    true
                }
                Err(ReadError::Protocol { code, reason }) => {
                    seen.push(format!("error {code} {reason}"));
                    frame::encode(frame::CLOSE, &frame::close_payload(code, ""), &mut out);
                    true
                }
                Err(ReadError::Closed) => {
                    seen.push("closed".into());
                    break;
                }
            };
            wr.write_all(&out).await.unwrap();
            if last {
                wr.shutdown().await.unwrap();
                // the client's end of the connection, whatever it sends before it
                assert_eq!(r.next().await, Err(ReadError::Closed));
                seen.push("closed".into());
                break;
            }
        }
        seen
    });
    (url, task)
}

fn text(s: &str) -> TMessage {
    TMessage::Text(s.into())
}

#[tokio::test]
async fn round_trip() {
    let (url, server) = echo_server().await;
    let mut req = url.into_client_request().unwrap();
    req.headers_mut().insert("Sec-WebSocket-Protocol", HeaderValue::from_static("chat, superchat"));
    let (mut ws, resp) = tokio_tungstenite::connect_async(req).await.unwrap();
    assert_eq!(resp.status(), 101);
    assert_eq!(resp.headers()["sec-websocket-protocol"], "chat");

    ws.send(text("hello")).await.unwrap();
    assert_eq!(ws.next().await.unwrap().unwrap(), text("hello"));
    for n in [0, 125, 126, 200, 65535, 65536] {
        let p = Bytes::from((0..n).map(|i| (i % 251) as u8).collect::<Vec<u8>>());
        ws.send(TMessage::Binary(p.clone())).await.unwrap();
        assert_eq!(ws.next().await.unwrap().unwrap(), TMessage::Binary(p), "{n} bytes");
    }
    // a message in fragments, a ping between them
    ws.send(TMessage::Frame(Frame::message(Bytes::from_static(b"Hel"), OpCode::Data(Data::Text), false))).await.unwrap();
    ws.send(TMessage::Ping(Bytes::from_static(b"p"))).await.unwrap();
    ws.send(TMessage::Frame(Frame::message(Bytes::from_static("lo é".as_bytes()), OpCode::Data(Data::Continue), true))).await.unwrap();
    assert_eq!(ws.next().await.unwrap().unwrap(), TMessage::Pong(Bytes::from_static(b"p")));
    assert_eq!(ws.next().await.unwrap().unwrap(), text("Hello é"));
    ws.send(TMessage::Pong(Bytes::from_static(b"unasked"))).await.unwrap();
    // the close handshake: ws answered with the same code and reason
    ws.send(TMessage::Close(Some(CloseFrame { code: 1000.into(), reason: "bye".into() }))).await.unwrap();
    match ws.next().await.unwrap().unwrap() {
        TMessage::Close(Some(f)) => assert_eq!((u16::from(f.code), f.reason.as_str()), (1000, "bye")),
        m => panic!("{m:?}"),
    }
    assert!(ws.next().await.is_none());
    // the server waits for the client's end of the connection
    drop(ws);
    let seen = server.await.unwrap();
    let want = [
        "text 5",
        "binary 0",
        "binary 125",
        "binary 126",
        "binary 200",
        "binary 65535",
        "binary 65536",
        "ping p",
        "text 8",
        "pong unasked",
        "close 1000 bye",
        "closed",
    ];
    assert_eq!(seen, want);
}

/// the close code (and reason) the client got from the server after sending `m`
async fn refused(m: TMessage) -> ((u16, String), Vec<String>) {
    let (url, server) = echo_server().await;
    let (mut ws, resp) = tokio_tungstenite::connect_async(url).await.unwrap();
    assert!(resp.headers().get("sec-websocket-protocol").is_none());
    ws.send(m).await.unwrap();
    let got = match ws.next().await {
        Some(Ok(TMessage::Close(Some(f)))) => (u16::from(f.code), f.reason.to_string()),
        Some(Ok(m)) => panic!("{m:?}"),
        Some(Err(e)) => panic!("{e}"),
        None => panic!("no close frame"),
    };
    // tungstenite answers the close frame; the server waits for the end of the connection
    while let Some(r) = ws.next().await {
        if let Err(tung::Error::ConnectionClosed | tung::Error::AlreadyClosed) = r {
            break;
        }
    }
    drop(ws);
    (got, server.await.unwrap())
}

#[tokio::test]
async fn a_message_over_max_payload() {
    let (got, seen) = refused(TMessage::Binary(Bytes::from(vec![0u8; 65537]))).await;
    // ws's close frame had the code alone
    assert_eq!(got, (1009, String::new()));
    assert_eq!(seen, ["error 1009 Max payload size exceeded", "closed"]);
}

#[tokio::test]
async fn text_that_is_not_utf8() {
    let f = Frame::message(Bytes::from_static(b"\xc3("), OpCode::Data(Data::Text), true);
    let (got, seen) = refused(TMessage::Frame(f)).await;
    assert_eq!(got, (1007, String::new()));
    assert_eq!(seen, ["error 1007 Invalid WebSocket frame: invalid UTF-8 sequence", "closed"]);
}

#[tokio::test]
async fn an_unknown_opcode() {
    let f = Frame::message(Bytes::from_static(b"?"), OpCode::Data(Data::Reserved(3)), true);
    let (got, seen) = refused(TMessage::Frame(f)).await;
    assert_eq!(got, (1002, String::new()));
    assert_eq!(seen, ["error 1002 Invalid WebSocket frame: invalid opcode 3", "closed"]);
}

#[tokio::test]
async fn the_end_without_a_close_frame() {
    let (url, server) = echo_server().await;
    let (mut ws, _) = tokio_tungstenite::connect_async(url).await.unwrap();
    ws.send(text("one")).await.unwrap();
    assert_eq!(ws.next().await.unwrap().unwrap(), text("one"));
    drop(ws);
    assert_eq!(server.await.unwrap(), ["text 3", "closed"]);
}
