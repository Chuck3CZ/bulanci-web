//! The engine against Node 24's own http server with the Node server's settings (tests/node/ref_server.mjs): the
//! same raw requests to both, the same bytes back (Date values aside) and the same connection state.  The cases are
//! the edge cases of llhttp's strictness, Node's header joining, its answer heads, pipelining and upgrades, and a few
//! thousand mutations of valid requests (a fixed seed).  Skipped (with a message) where `node` is missing.
//!
//! Left out on purpose (the engine differs, tests/engine.rs has them): a client that ends its side while an answer
//! is still being made (Node lost that answer, the engine sends it), and a CONNECT or upgrade read while an earlier
//! answer is under way (Node handled it at once and lost the earlier answer).

mod common;

use std::sync::Arc;
use std::time::Duration;

use common::{Case, Got, NodeRef, TestService, exchange};
use nfsu2_http::Limits;
use serde_json::json;

/// a string of latin1 characters as the bytes they stand for
fn lat(s: &str) -> Vec<u8> {
    s.chars().map(|c| c as u32 as u8).collect()
}

fn req_line_cases() -> Vec<String> {
    [
        "GET / HTTP/1.1\r\nHost: a\r\n\r\n",
        "get / HTTP/1.1\r\n\r\n",
        "FOO / HTTP/1.1\r\n\r\n",
        "DESCRIBE / HTTP/1.1\r\n\r\n",
        "QUERY / HTTP/1.1\r\n\r\n",
        "M-SEARCH * HTTP/1.1\r\n\r\n",
        "GETX / HTTP/1.1\r\n\r\n",
        "GET_PARAMETER / RTSP/1.0\r\n\r\n",
        "PROPPATCH / HTTP/1.1\r\n\r\n",
        "GET  / HTTP/1.1\r\n\r\n",
        "GET /  HTTP/1.1\r\n\r\n",
        "GET / HTTP/1.1 \r\n\r\n",
        "GET /\r\n\r\n",
        "GET /\r\nHost: a\r\n\r\n",
        "GET /\n\r\n",
        "GET /x?y\r\n\r\n",
        "GET / HTTP/1.0\r\n\r\n",
        "GET / HTTP/0.9\r\n\r\n",
        "GET / HTTP/2.0\r\n\r\n",
        "GET / HTTP/1.2\r\n\r\n",
        "GET / HTTP/3.0\r\n\r\n",
        "GET / HTTP/11.1\r\n\r\n",
        "GET / http/1.1\r\n\r\n",
        "GET / HTTP/1.1\n\n",
        "GET / HTTP/1.1\r\nHost: a\n\r\n",
        "GET / HTTP/1.1\nHost: a\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\n\n",
        "GET / HTTP/1.1\rHost: a\r\n\r\n",
        "GET / HTTP/1.1\r\n\n",
        "GET / HTTP/1.1\r\n\r\r\n",
        "\r\nGET / HTTP/1.1\r\n\r\n",
        "\nGET / HTTP/1.1\r\n\r\n",
        "\r\n\r\n\r\nGET / HTTP/1.1\r\n\r\n",
        " GET / HTTP/1.1\r\n\r\n",
        "GET /\u{7f} HTTP/1.1\r\n\r\n",
        "GET /\u{ff}\u{80} HTTP/1.1\r\n\r\n",
        "GET /a\tb HTTP/1.1\r\n\r\n",
        "GET /\u{1} HTTP/1.1\r\n\r\n",
        "GET /\u{0} HTTP/1.1\r\n\r\n",
        "GET http://evil.example/robots.txt HTTP/1.1\r\nHost: a\r\n\r\n",
        "GET * HTTP/1.1\r\n\r\n",
        "OPTIONS * HTTP/1.1\r\n\r\n",
        "GET /a#frag HTTP/1.1\r\n\r\n",
        "GET /a?b=c#d HTTP/1.1\r\n\r\n",
        "GET ?x HTTP/1.1\r\n\r\n",
        "GET //x HTTP/1.1\r\n\r\n",
        "GET a HTTP/1.1\r\n\r\n",
        "GET /\"<>\\ HTTP/1.1\r\n\r\n",
        "GET /{}|^`[] HTTP/1.1\r\n\r\n",
        "GET /%zz%%2F HTTP/1.1\r\n\r\n",
        "CONNECT host:443 HTTP/1.1\r\nHost: host:443\r\n\r\n",
        "CONNECT / HTTP/1.1\r\n\r\n",
        "CONNECT a b HTTP/1.1\r\n\r\n",
        "CONNECT a\u{1} HTTP/1.1\r\n\r\n",
        "CONNECT http://a/ HTTP/1.1\r\n\r\n",
        "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n",
        "PRI * HTTP/1.1\r\n\r\n",
        "PRI * HTTP/1.1\r\n\r\nXY",
        "PRI / HTTP/2.0\r\n\r\n",
        "HELLO THERE\r\n\r\n",
        "GET\r\n\r\n",
        "GET \r\n\r\n",
        "GET / \r\n\r\n",
        "GET / H\r\n\r\n",
        "GET / HTTP/1\r\n\r\n",
        "GET / HTTP/1.\r\n\r\n",
        "GET / HTTP/1.1x\r\n\r\n",
        "GET / RTSP/1.0\r\n\r\n",
        "DESCRIBE / RTSP/1.0\r\n\r\n",
        "SOURCE / ICE/1.0\r\n\r\n",
        "GET / ICE/1.0\r\n\r\n",
        "GET\t/ HTTP/1.1\r\n\r\n",
        "GET /\tHTTP/1.1\r\n\r\n",
        "GET http://a:80 HTTP/1.1\r\n\r\n",
        "GET http://a:80/x?y HTTP/1.1\r\n\r\n",
        "GET http://u:p@a/x HTTP/1.1\r\n\r\n",
        "GET https://[::1]:8/x HTTP/1.1\r\n\r\n",
        "GET ftp://a/ HTTP/1.1\r\n\r\n",
        "GET http:/a HTTP/1.1\r\n\r\n",
        "GET http//a HTTP/1.1\r\n\r\n",
        "GET 1http://a/ HTTP/1.1\r\n\r\n",
        "GET a:b HTTP/1.1\r\n\r\n",
        "GET /a b HTTP/1.1\r\n\r\n",
        "GET / HTTP/1.1\r\n\r",
        "GET *x HTTP/1.1\r\n\r\n",
        "GET http:// HTTP/1.1\r\n\r\n",
        "GET http://a#b HTTP/1.1\r\n\r\n",
        "GET http://a?b HTTP/1.1\r\n\r\n",
        "GET http://a@b@c/ HTTP/1.1\r\n\r\n",
        "GET http://[::1] HTTP/1.1\r\n\r\n",
        "GET HTTP://A/ HTTP/1.1\r\n\r\n",
        "GET h1ttp://a/ HTTP/1.1\r\n\r\n",
        "GET /a?b?c#d#e?f HTTP/1.1\r\n\r\n",
        "GET /a#b c HTTP/1.1\r\n\r\n",
        "GET /~!$&'()*+,;=:@ HTTP/1.1\r\n\r\n",
        "GET http://a\"b/ HTTP/1.1\r\n\r\n",
        "GET http://a\\b/ HTTP/1.1\r\n\r\n",
        "GET http://a~b/ HTTP/1.1\r\n\r\n",
        "GET http://a? HTTP/1.1\r\n\r\n",
        "GET /# HTTP/1.1\r\n\r\n",
        "GET *# HTTP/1.1\r\n\r\n",
        "GET http\r\n\r\n",
        "GET http:/\r\n\r\n",
        "PRI *\r\n\r\n",
        "PRI /x\r\nHost: a\r\n\r\nGET / HTTP/1.1\r\n\r\n",
        "PRI /x\r\nConnection: keep-alive\r\n\r\n",
        "PRI /x\r\nContent-Length: 2\r\n\r\nab",
        "CONNECT a:1\r\n\r\n",
    ]
    .iter()
    .map(|s| s.to_string())
    .collect()
}

fn header_cases() -> Vec<String> {
    let g = "GET / HTTP/1.1\r\n";
    let h = |lines: &[&str]| format!("{g}{}\r\n", lines.iter().map(|l| format!("{l}\r\n")).collect::<String>());
    let te = |v: &str| h(&[&format!("Transfer-Encoding: {v}")]) + "0\r\n\r\n";
    let body = |head: &str, b: &str| format!("POST /body HTTP/1.1\r\n{head}\r\n\r\n{b}");
    let chunked = |b: &str| body("Transfer-Encoding: chunked", b);
    let mut v = vec![
        h(&["Host : a"]),
        h(&["Host:a"]),
        h(&["Host:   a   "]),
        h(&["Host:\ta\t"]),
        h(&[" Host: a"]),
        h(&["\tHost: a"]),
        h(&["X: a", " b"]),
        h(&["X: a", "\tb"]),
        h(&[":x"]),
        h(&["X-A"]),
        h(&["X(A): b"]),
        h(&["X@: b"]),
        h(&["X!#$%&'*+-.^_`|~09: b"]),
        h(&["X: a\u{0}b"]),
        h(&["X: a\u{1}b"]),
        h(&["X: a\u{7f}b"]),
        h(&["X: a\u{ff}b\u{80}"]),
        h(&["X: a\rb"]),
        h(&["X: a\tb"]),
        h(&["X:"]),
        h(&["X: "]),
        h(&["X:\t"]),
        h(&["X: \t a \t "]),
        h(&["X\u{80}: a"]),
        h(&["Host: a", "Host: b"]),
        h(&["X-Foo: a", "X-Foo: b"]),
        h(&["Cookie: a=1", "Cookie: b=2"]),
        h(&["Content-Type: a", "content-type: b"]),
        h(&["HOST: A"]),
        h(&["User-Agent: x", "user-agent: y"]),
        h(&["Referer: a", "Referer: b"]),
        h(&["Authorization: a", "Authorization: b"]),
        h(&["Accept: a", "Accept: b"]),
        h(&["X-Forwarded-For: 1", "X-Forwarded-For: 2"]),
        h(&["Age: 1", "age: 2", "ETag: x", "etag: y", "From: f", "from: g", "Server: s", "server: t"]),
        h(&["Expires: 1", "expires: 2", "Location: l", "location: m", "Max-Forwards: 1", "max-forwards: 2"]),
        h(&["Retry-After: 1", "retry-after: 2", "If-Modified-Since: a", "if-modified-since: b", "Last-Modified: c", "last-modified: d"]),
        h(&["Proxy-Authorization: a", "proxy-authorization: b", "If-Unmodified-Since: x", "if-unmodified-since: y"]),
        h(&["Vary: a", "vary: b", "Date: c", "date: d", "Origin: o", "origin: p", "Upgrade-Insecure-Requests: 1"]),
        h(&["10: a", "2: b", "x: c", "01: d", "0: e"]),
        h(&["Content-Length: 5"]) + "hello",
        h(&["Content-Length: 5", "Content-Length: 5"]) + "hello",
        h(&["Content-Length: 5", "Content-Length: 6"]) + "hello",
        h(&["Content-Length: 5, 5"]) + "hello",
        h(&["Content-Length: -1"]),
        h(&["Content-Length: +5"]) + "hello",
        h(&["Content-Length:  5 "]) + "hello",
        h(&["Content-Length: 5x"]) + "hello",
        h(&["Content-Length: "]),
        h(&["Content-Length:"]),
        h(&["Content-Length: 0x5"]),
        h(&["Content-Length: 99999999999999999999"]),
        h(&["Content-Length: 18446744073709551615"]),
        h(&["Content-Length: 18446744073709551616"]),
        h(&["Content-Length: 0"]),
        h(&["Content-Length: 00005"]) + "hello",
        h(&["Content-Length: 5 5"]) + "hello",
        h(&["Content-Length: 5\t"]) + "hello",
        h(&["Content-Length:\t5"]) + "hello",
        h(&["CONTENT-LENGTH: 3"]) + "abc",
        h(&["Content-Length: 3", "content-length: 3"]) + "abc",
        h(&["Content-Length: 5", "Transfer-Encoding: chunked"]) + "0\r\n\r\n",
        h(&["Transfer-Encoding: chunked", "Content-Length: 5"]) + "0\r\n\r\n",
        h(&["Content-Length: 0", "Transfer-Encoding: chunked"]),
        h(&["Transfer-Encoding:", "Content-Length: 2"]) + "ab",
        h(&["Content-Length: 2", "Transfer-Encoding:"]) + "ab",
        te("chunked"),
        h(&["Transfer-Encoding: gzip"]),
        te("gzip, chunked"),
        h(&["Transfer-Encoding: chunked, gzip"]),
        h(&["Transfer-Encoding: chunked", "Transfer-Encoding: chunked"]) + "0\r\n\r\n",
        te("Chunked"),
        te("chunked "),
        te("chunked  "),
        te(" chunked"),
        te("chunked\t"),
        h(&["Transfer-Encoding: xchunked"]),
        h(&["Transfer-Encoding: identity"]),
        h(&["Transfer-Encoding: chunked;q=1"]),
        h(&["Transfer-Encoding: chunked", "Transfer-Encoding: gzip"]),
        h(&["Transfer-Encoding: gzip", "Transfer-Encoding: chunked"]) + "0\r\n\r\n",
        h(&["Transfer-Encoding:"]),
        te("chunked,"),
        te(",chunked"),
        te("chunked , chunked"),
        te("gzip ,chunked"),
        te("gzip,\tchunked"),
        te("gzip, chunked , "),
        te("chunkedx, chunked"),
        te("x;a=\"b,c\", chunked"),
        te("\u{80}, chunked"),
        te("chunk"),
        te("CHUNKED"),
        h(&["Transfer-Encoding: chunked"]) + "zz\r\n",
        h(&["Transfer-Encoding: chunked"]) + "3;foo=bar\r\nabc\r\n0\r\n\r\n",
        h(&["Transfer-Encoding: chunked"]) + "3 \r\nabc\r\n0\r\n\r\n",
        h(&["Transfer-Encoding: chunked"]) + "3\nabc\n0\n\n",
        h(&["Transfer-Encoding: chunked"]) + "3\r\nabcX0\r\n\r\n",
        h(&["Transfer-Encoding: chunked"]) + "0\r\nX-T: 1\r\n\r\n",
        h(&["Transfer-Encoding: chunked"]) + "ffffffffffffffffff\r\n",
        h(&["Transfer-Encoding: chunked"]) + "3;\r\nabc\r\n0\r\n\r\n",
        h(&["Transfer-Encoding: chunked"]) + "3 ;x\r\nabc\r\n0\r\n\r\n",
        h(&["Transfer-Encoding: chunked"]) + "3;x=\"a b\"\r\nabc\r\n0\r\n\r\n",
        h(&["Transfer-Encoding: chunked"]) + "3;x=a b\r\nabc\r\n0\r\n\r\n",
        h(&["Transfer-Encoding: chunked"]) + "3\tx\r\nabc\r\n0\r\n\r\n",
        h(&["Transfer-Encoding: chunked"]) + "-3\r\nabc\r\n0\r\n\r\n",
        h(&["Transfer-Encoding: chunked"]) + "\r\n3\r\nabc\r\n0\r\n\r\n",
        h(&["Transfer-Encoding: chunked"]) + "0\r\n\r\nGET /x HTTP/1.1\r\n\r\n",
        h(&["Transfer-Encoding: chunked"]) + "0\r\nX-T 1\r\n\r\n",
        h(&["Transfer-Encoding: chunked"]) + "00\r\n\r\n",
        h(&["Transfer-Encoding: chunked"]) + "3\r\nabc\r\r\n0\r\n\r\n",
        h(&["Transfer-Encoding: chunked"]) + "3\r\nabc0\r\n\r\n",
        chunked("3\r\nabc\r\n2;e=1\r\nde\r\n0\r\nT: x\r\n\r\n"),
        chunked("1;a;b=c;d=\"e;f\"\r\na\r\n0;x\r\n\r\n"),
        chunked("1;a=\r\na\r\n0\r\n\r\n"),
        chunked("1;=b\r\na\r\n0\r\n\r\n"),
        chunked("1;;a\r\na\r\n0\r\n\r\n"),
        chunked("1;a;\r\na\r\n0\r\n\r\n"),
        chunked("1;a=;b\r\na\r\n0\r\n\r\n"),
        chunked("1;a=\"\\\"x\"\r\na\r\n0\r\n\r\n"),
        chunked("1;a=\"x\r\na\r\n0\r\n\r\n"),
        chunked("1; a\r\na\r\n0\r\n\r\n"),
        chunked("1;a =b\r\na\r\n0\r\n\r\n"),
        chunked("1;a= b\r\na\r\n0\r\n\r\n"),
        chunked("1;a=\"b\"c\r\na\r\n0\r\n\r\n"),
        chunked("1;a=\"b\";c\r\na\r\n0\r\n\r\n"),
        chunked("1;a=\u{80}\r\na\r\n0\r\n\r\n"),
        chunked("1;a=\"\u{80}\"\r\na\r\n0\r\n\r\n"),
        chunked("1;a=\"\\\u{1}\"\r\na\r\n0\r\n\r\n"),
        chunked("1\r\na\r\n0\r\nT: 1\r\nU: 2\r\n\r\n"),
        chunked("1\r\na\r\n0\r\n T: 1\r\n\r\n"),
        chunked("1\r\na\r\n0\r\n\n"),
        chunked("1\r\na\r\n0\r\nT: \u{1}\r\n\r\n"),
        chunked("1\r\na\r\n0\r\nContent-Length: 5\r\n\r\n"),
        chunked("1\r\na\r\n0\r\nTransfer-Encoding: chunked\r\n\r\n"),
        chunked("1\r\na\r\n0\r\nConnection: close\r\n\r\nGET / HTTP/1.1\r\n\r\n"),
        chunked("00000000000000000001\r\na\r\n0\r\n\r\n"),
        chunked("10000000000000000\r\na\r\n0\r\n\r\n"),
        chunked("1  \r\na\r\n0\r\n\r\n"),
        chunked(&format!("1;{}\r\na\r\n0\r\n\r\n", "e".repeat(16384))),
        chunked(&format!("1;{}\r\na\r\n0\r\n\r\n", "e".repeat(16385))),
        chunked(&format!("1;e={}\r\na\r\n0\r\n\r\n", "v".repeat(16383))),
        chunked(&format!("1;e={}\r\na\r\n0\r\n\r\n", "v".repeat(16384))),
        chunked(&format!("1;e=\"{}\"\r\na\r\n0\r\n\r\n", "v".repeat(16382))),
        chunked(&format!("1;e=\"{}\"\r\na\r\n0\r\n\r\n", "v".repeat(16383))),
        body("Content-Length: 2\r\nTransfer-Encoding: chunked", "ab"),
        body("Content-Length: 0", ""),
        "HEAD /body HTTP/1.1\r\nContent-Length: 2\r\n\r\nab".into(),
        "POST /body HTTP/0.9\r\nContent-Length: 2\r\n\r\nab".into(),
        "POST /body HTTP/1.0\r\nTransfer-Encoding: gzip\r\n\r\nab".into(),
        "POST /body HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n0\r\n\r\n".into(),
        "POST /body HTTP/1.0\r\nContent-Length: 2\r\n\r\nab".into(),
        "POST /body HTTP/1.1\r\nContent-Length: 3\r\n\r\nabcGET /x HTTP/1.1\r\n\r\n".into(),
        "GET / HTTP/1.0\r\nTransfer-Encoding: gzip\r\n\r\n".into(),
        "GET / HTTP/1.0\r\nConnection: keep-alive\r\nTransfer-Encoding: gzip\r\n\r\n".into(),
    ];
    for c in [
        "close",
        "close, keep-alive",
        "keep-alive, close",
        "closed",
        "x, close",
        "x,close",
        "close,x",
        "close x",
        "CLOSE",
        "\tclose",
        "close\t",
        ",close",
        "close ,x",
        "close  ,x",
        "close\t,x",
        "x close",
        "x,, close",
        "",
        "upgrade",
    ] {
        v.push(h(&[&format!("Connection: {c}")]));
        v.push(format!("GET / HTTP/1.0\r\nConnection: {}\r\n\r\n", c.replace("close", "keep-alive")));
    }
    v.extend([
        h(&["Connection: keep-alive", "Connection: close"]),
        h(&["Proxy-Connection: close"]),
        "GET / HTTP/1.0\r\nProxy-Connection: keep-alive\r\n\r\n".into(),
        "GET / HTTP/1.0\r\nConnection: keep-alivex\r\n\r\n".into(),
        "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\nGET /x HTTP/1.0\r\n\r\n".into(),
        "GET / HTTP/1.0\r\n\r\n\r\n\r\n".into(),
        "GET / HTTP/1.0\r\n\r\n\n".into(),
        "GET / HTTP/1.0\r\n\r\n \r\n".into(),
        "GET / HTTP/1.0\r\n\r\nGET / HTTP/1.1\r\n\r\n".into(),
        "GET / HTTP/1.1\r\nConnection: close\r\n\r\nGET / HTTP/1.1\r\n\r\n".into(),
        "GET /close HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n\r\n".into(),
        "GET /keep HTTP/1.1\r\nConnection: close\r\n\r\nGET / HTTP/1.1\r\n\r\n".into(),
        h(&["Upgrade: websocket", "Connection: Upgrade"]),
        h(&["Upgrade: websocket"]),
        h(&["Connection: Upgrade"]),
        h(&["Upgrade: websocket", "Connection: Upgrade", "Content-Length: 5"]) + "helloXYZ",
        h(&["Upgrade: websocket", "Connection: Upgrade"]) + "EXTRA",
        h(&["Upgrade:", "Connection: upgrade"]),
        h(&["Upgrade:  ", "Connection: upgrade"]),
        h(&["Upgrade: \u{80}", "Connection: upgrade"]),
        h(&["Upgrade: h2c", "Connection: Upgrade, HTTP2-Settings", "HTTP2-Settings: AAMA"]),
        h(&["Upgrade: websocket", "Connection: keep-alive, Upgrade"]),
        h(&["Upgrade: websocket", "Connection: upgradex"]),
        h(&["Connection: upgrade", "Upgrade: x"]),
        h(&["Upgrade: x", "Connection: upgrade, close"]),
        h(&["Upgrade: x", "Proxy-Connection: upgrade"]),
        h(&["Upgrade: x", "Connection: upgrade ", "X: y"]),
        h(&["Upgrade: x", "Connection: upgrade", "Expect: teapot"]),
        "GET / HTTP/1.0\r\nUpgrade: websocket\r\nConnection: upgrade\r\n\r\n".into(),
        h(&["Upgrade: websocket", "Connection: Upgrade", "Transfer-Encoding: chunked"]) + "0\r\n\r\nZ",
        h(&["Upgrade: websocket", "Connection: Upgrade", "Transfer-Encoding: gzip"]),
        "HEAD / HTTP/1.1\r\nUpgrade: x\r\nConnection: upgrade\r\n\r\n".into(),
        "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\nGET / HTTP/1.1\r\n\r\n".into(),
        "GET / HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\nXX".into(),
        "GET / HTTP/1.1\r\n\r\nCONNECT a:1 HTTP/1.1\r\n\r\n".into(),
        "POST /body HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\nabc".into(),
        "POST /body HTTP/1.1\r\nExpect: 100-continue\r\n\r\n".into(),
        h(&["Expect: teapot"]),
        "GET / HTTP/1.0\r\nExpect: teapot\r\n\r\n".into(),
        "GET / HTTP/2.0\r\nExpect: teapot\r\n\r\n".into(),
        h(&["Expect:"]),
        h(&["Expect: 100-Continue"]),
        h(&["Expect: x, 100-continue"]),
        h(&["Expect: 100-continuex"]),
        h(&["Expect: teapot", "Expect: 100-continue"]),
        "HEAD / HTTP/1.1\r\nExpect: teapot\r\n\r\n".into(),
        "GET / HTTP/1.1\r\nExpect: teapot\r\n\r\nGET / HTTP/1.1\r\n\r\n".into(),
        "GET / HTTP/1.1\r\nExpect: 100-continue\r\n\r\n".into(),
        "GET / HTTP/1.1\r\nHost: a\r\n\r\nGET /x HTTP/1.1\r\n\r\n".into(),
        "GET / HTTP/1.1\r\nHost: a\r\n\r\nBAD\r\n\r\n".into(),
        "GET /slow HTTP/1.1\r\nHost: a\r\n\r\nBAD\r\n\r\n".into(),
        "GET / HTTP/1.1\r\nHost: a\r\n\r\n\r\nGET /x HTTP/1.1\r\n\r\n".into(),
    ]);
    for p in [
        "/",
        "/chunked",
        "/empty",
        "/204",
        "/304",
        "/204te",
        "/te",
        "/close",
        "/keep",
        "/ka",
        "/ka2",
        "/date",
        "/big",
        "/bigchunked",
        "/status/100",
        "/status/101",
        "/status/299",
        "/status/418",
        "/status/509",
        "/status/999",
        "/status/205",
        "/status/199",
        "/status/226",
    ] {
        for (m, ver, extra) in [
            ("GET", "1.1", ""),
            ("GET", "1.0", ""),
            ("HEAD", "1.1", ""),
            ("HEAD", "1.0", ""),
            ("GET", "1.0", "Connection: keep-alive\r\n"),
            ("GET", "1.0", "TE: chunked\r\n"),
            ("GET", "1.0", "TE: trailers, chunked\r\n"),
            ("GET", "1.1", "Connection: close\r\n"),
            ("GET", "2.0", ""),
            ("GET", "0.9", ""),
        ] {
            v.push(format!("{m} {p} HTTP/{ver}\r\n{extra}\r\n"));
        }
    }
    let r = "GET / HTTP/1.1\r\n\r\n";
    v.extend([
        r.repeat(4),
        r.repeat(5),
        r.repeat(6),
        "GET /slow HTTP/1.1\r\n\r\n".repeat(4),
        "GET /slow HTTP/1.1\r\n\r\n".repeat(5),
        format!("GET /slow HTTP/1.1\r\n\r\nGET /slow HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc{}", r.repeat(3)),
        format!("GET /slow HTTP/1.1\r\n\r\nGET /slow HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n0\r\n\r\n{}", r.repeat(3)),
        format!("GET /slow HTTP/1.1\r\n\r\nGET /slow HTTP/1.1\r\nExpect: teapot\r\n\r\n{}", r.repeat(3)),
        format!("{}GET /close HTTP/1.1\r\n\r\n", r.repeat(2)),
        format!("GET /x HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\nUpgrade: a\r\nConnection: upgrade\r\n\r\n{}", r.repeat(5)),
    ]);
    v
}

/// The header limit's edges, against a server whose limit is 200 bytes.
fn small_limit_cases() -> Vec<String> {
    let a = |n: usize| "a".repeat(n);
    let g = "GET / HTTP/1.1\r\n";
    vec![
        format!("GET /{} HTTP/1.1\r\n\r\n", a(198)),
        format!("GET /{} HTTP/1.1\r\n\r\n", a(199)),
        format!("GET /{} HTTP/1.1\r\nX:\r\n\r\n", a(198)),
        format!("{g}X: {}\r\n\r\n", a(197)),
        format!("{g}X: {}\r\n\r\n", a(198)),
        format!("{g}X: {} \r\n\r\n", a(197)),
        format!("{g}X: {}\t\r\n\r\n", a(197)),
        format!("{g}X:   {}\r\n\r\n", a(197)),
        format!("{g}X: {} b\r\n\r\n", a(195)),
        format!("{g}X: {} b\r\n\r\n", a(196)),
        format!("{g}X: {}\r\nY: {}\r\n\r\n", a(97), a(98)),
        format!("{g}X: {}\r\nY: {}\r\n\r\n", a(97), a(99)),
        format!("{g}X: {}\r\nY: {}\r\n\r\n", a(97), a(100)),
        format!("{g}X: {}", a(300)),
        format!("PROPPATCH /{} HTTP/1.1\r\n\r\n", a(198)),
        format!("POST /body HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nT: {}\r\n\r\n", a(250)),
        format!("POST /body HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nT: {}\r\n\r\n", a(150)),
        format!("POST /body HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n1;{}\r\na\r\n0\r\n\r\n", a(400)),
        format!("POST /body HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nT: {}\r\n\r\nGET /{} HTTP/1.1\r\n\r\n", a(150), a(40)),
        format!("POST /body HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nT: {}\r\n\r\nGET /{} HTTP/1.1\r\n\r\n", a(150), a(50)),
        format!("GET / HTTP/1.1\r\n{}\r\n", "a:\r\n".repeat(198)),
        format!("GET / HTTP/1.1\r\n{}\r\n", "a:\r\n".repeat(199)),
    ]
}

/// A seeded generator (xorshift64*): the same corpus on every run.
struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }
    fn below(&mut self, n: usize) -> usize {
        (self.next() % n as u64) as usize
    }
}

/// Mutations of valid requests: bytes replaced, inserted, dropped, lines doubled, letters flipped in case.
fn fuzz_cases(n: usize, seed: u64) -> Vec<Vec<u8>> {
    let templates: [&[u8]; 10] = [
        b"GET /a/b?c=d#e HTTP/1.1\r\nHost: x\r\nX-A: 1\r\nConnection: close\r\n\r\n",
        b"POST /body HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello",
        b"POST /body HTTP/1.1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n3;e=1\r\nabc\r\n2\r\nde\r\n0\r\nT: x\r\n\r\n",
        b"GET http://a:1/x HTTP/1.0\r\nConnection: keep-alive, x\r\nCookie: a\r\nCookie: b\r\n\r\n",
        b"GET /lan HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: abc\r\n\r\nEXTRA",
        b"GET /chunked HTTP/1.1\r\nTE: chunked\r\nConnection: close\r\n\r\n",
        b"HEAD /status/204 HTTP/1.1\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n",
        b"OPTIONS * HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
        b"PUT /body HTTP/1.1\r\nContent-Length: 3\r\nExpect: 100-continue\r\nConnection: close\r\n\r\nabc",
        b"GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /empty HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\nConnection: close\r\n\r\n0\r\n\r\n",
    ];
    const BYTES: &[u8] = b"\r\n \t:;,=\"\\/?#%@*-_.0123456789aAzZ\x00\x01\x7f\x80\xff{}<>|^`[]()!$&'+~";
    let mut rng = Rng(seed);
    let mut out = Vec::with_capacity(n);
    for i in 0..n {
        let mut b = templates[i % templates.len()].to_vec();
        // most with a few edits, a quarter with many
        let edits = 1 + rng.below(if i % 4 == 3 { 8 } else { 3 });
        for _ in 0..edits {
            if b.is_empty() {
                break;
            }
            let at = rng.below(b.len());
            match rng.below(6) {
                0 | 1 => b[at] = BYTES[rng.below(BYTES.len())],
                2 => b.insert(at, BYTES[rng.below(BYTES.len())]),
                3 => {
                    b.remove(at);
                }
                4 => {
                    // a line doubled
                    let start = b[..at].iter().rposition(|&c| c == b'\n').map_or(0, |p| p + 1);
                    let end = b[at..].iter().position(|&c| c == b'\n').map_or(b.len(), |p| at + p + 1);
                    let line = b[start..end].to_vec();
                    b.splice(end..end, line);
                }
                _ => {
                    if b[at].is_ascii_alphabetic() {
                        b[at] ^= 0x20;
                    }
                }
            }
        }
        out.push(b);
    }
    out
}

/// The two places the engine differs on purpose when a request breaks the connection while answers are under way
/// (the crate's doc): Node wrote its raw refusal into the middle of an answer still being written, where the engine
/// finishes the answer first; and Node had sent answers queued behind the current one when its parser had handed
/// body bytes to JavaScript in between (that call ran what was due), where the engine drops the queued requests.
fn known_deviation(rust: &Got, node: &Got) -> bool {
    if !rust.closed || !node.closed {
        return false;
    }
    [400u16, 408, 431].iter().any(|&code| {
        let refusal: String = common::raw_response(code).iter().map(|&b| b as char).collect();
        let Some(before) = rust.text.strip_suffix(&refusal) else { return false };
        // (the refusal inside the answer, or after a part of it, the rest of the answer lost)
        let interleaved = (node.text.contains(&refusal) && !node.text.ends_with(&refusal))
            || node.text.strip_suffix(&refusal).is_some_and(|n| n.len() < before.len() && before.starts_with(n));
        let more_answers = !node.text.contains(&refusal) && node.text.len() > before.len() && node.text.starts_with(before);
        interleaved || more_answers
    })
}

/// The cases sent to both, in parallel; the differences, described.
async fn compare(rust: std::net::SocketAddr, node: std::net::SocketAddr, cases: Vec<Case>) -> Vec<String> {
    let sem = Arc::new(tokio::sync::Semaphore::new(48));
    let mut set = tokio::task::JoinSet::new();
    for (i, case) in cases.into_iter().enumerate() {
        let sem = sem.clone();
        set.spawn(async move {
            let _permit = sem.acquire_owned().await;
            let (a, b) = tokio::join!(exchange(rust, &case), exchange(node, &case));
            (i, case, a, b)
        });
    }
    let mut diffs = Vec::new();
    let mut known = 0;
    while let Some(r) = set.join_next().await {
        let Ok((i, case, a, b)) = r else { continue };
        if a != b && known_deviation(&a, &b) {
            known += 1;
        } else if a != b {
            diffs.push(format!(
                "case {i} {:?}\n  rust: {:?} closed={}\n  node: {:?} closed={}",
                common::mask_dates(&case.parts.concat()),
                clip(&a),
                a.closed,
                clip(&b),
                b.closed
            ));
        }
    }
    if known > 0 {
        eprintln!("{known} cases differ the way the engine differs on purpose (known_deviation)");
    }
    diffs.sort();
    diffs
}

fn clip(g: &Got) -> String {
    if g.text.len() > 400 { format!("{}...({})", &g.text[..400], g.text.len()) } else { g.text.clone() }
}

async fn servers(max_header: usize) -> Option<(std::net::SocketAddr, std::net::SocketAddr, NodeRef)> {
    let node = NodeRef::start(json!({ "maxHeaderSize": max_header })).await?;
    let limits = Limits { max_header_size: max_header, ..Limits::default() };
    let rust = common::start(limits, TestService::new(Duration::from_secs(65))).await;
    Some((rust, node.addr, node))
}

fn report(what: &str, n: usize, diffs: &[String]) {
    eprintln!("{what}: {n} cases, {} differ", diffs.len());
    for d in diffs.iter().take(40) {
        eprintln!("{d}");
    }
    assert!(diffs.is_empty(), "{what}: {} of {n} cases differ from Node", diffs.len());
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn edge_cases_as_node_answers_them() {
    let Some((rust, node, _keep)) = servers(16 << 10).await else {
        eprintln!("skipped: no node to compare with");
        return;
    };
    let mut cases: Vec<Case> = req_line_cases().iter().chain(header_cases().iter()).map(|s| Case::new(lat(s))).collect();
    // pieces and pauses: the same answers however the bytes come
    let split = |parts: &[&str], gap: u64| Case {
        parts: parts.iter().map(|p| lat(p)).collect(),
        gap: Duration::from_millis(gap),
        end: false,
        wait: Duration::from_millis(1200),
    };
    cases.extend([
        split(&["GE", "T / HTTP/1.1\r\n", "\r\n"], 50),
        split(&["POST /body HTTP/1.1\r\nContent-Length: 5\r\n\r\nab", "cde"], 100),
        split(&["POST /body HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n", "abc"], 100),
        split(&["POST /body HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r", "\nab", "c\r\n0\r\n", "\r\n"], 50),
        split(
            &[
                "GET /slow HTTP/1.1\r\n\r\nGET /slow HTTP/1.1\r\n\r\nGET /slow HTTP/1.1\r\n\r\n",
                "GET /slow HTTP/1.1\r\n\r\nGET /slow HTTP/1.1\r\n\r\n",
            ],
            50,
        ),
        split(
            &[
                "GET /slow HTTP/1.1\r\n\r\nGET /slow HTTP/1.1\r\n\r\nGET /slow HTTP/1.1\r\n\r\n",
                "GET /slow HTTP/1.1\r\n\r\nGET /slow HTTP/1.1\r\n\r\n",
            ],
            200,
        ),
        split(&["GET / HTTP/1.1\r\n\r\n", "GET /x HTTP/1.1\r\n\r\n", "GET /y HTTP/1.1\r\nConnection: close\r\n\r\n"], 100),
        split(&["GET / HTTP/1.1\r\nConnection: close\r\n\r\n", "GET / HTTP/1.1\r\n\r\n"], 100),
    ]);
    // a client that ends its side: mid-head, after a complete request, mid-body
    let ended = |s: &str| Case { parts: vec![lat(s)], gap: Duration::ZERO, end: true, wait: Duration::from_millis(700) };
    cases.extend([
        ended("GET / HTTP/1.1\r\nHost: a\r\n"),
        ended("GET / HTTP/1.1\r\n\r\n"),
        ended("GET / HTTP/1.1\r\nContent-Length: 5\r\n\r\nab"),
        ended("\r\n"),
        ended(""),
        ended("GET /slow HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n"),
        ended("GET / HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\n\r\n"),
        ended("POST /body HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nab"),
    ]);
    let n = cases.len();
    let diffs = compare(rust, node, cases).await;
    report("edge cases", n, &diffs);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn the_header_limit_where_node_draws_it() {
    let Some((rust, node, _keep)) = servers(200).await else {
        eprintln!("skipped: no node to compare with");
        return;
    };
    let cases: Vec<Case> = small_limit_cases().iter().map(|s| Case::new(lat(s))).collect();
    let n = cases.len();
    let diffs = compare(rust, node, cases).await;
    report("header limit", n, &diffs);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn mutated_requests_as_node_answers_them() {
    let Some((rust, node, _keep)) = servers(16 << 10).await else {
        eprintln!("skipped: no node to compare with");
        return;
    };
    // NFSU2_HTTP_FUZZ=<count>[,<seed>]: a longer run
    let knob = std::env::var("NFSU2_HTTP_FUZZ").unwrap_or_default();
    let mut knob = knob.split(',').map(|v| v.trim().parse::<u64>().ok());
    let n = knob.next().flatten().unwrap_or(3000) as usize;
    let seed = knob.next().flatten().unwrap_or(0x6e66_7375_3268_7474);
    let cases: Vec<Case> = fuzz_cases(n, seed).into_iter().map(Case::new).collect();
    let n = cases.len();
    let diffs = compare(rust, node, cases).await;
    report("mutations", n, &diffs);
}
