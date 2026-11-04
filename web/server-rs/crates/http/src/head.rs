//! What Node writes into a response's head besides the server's own headers: the status line with Node's reason
//! phrases (`http.STATUS_CODES`), and `Date` (`utcDate()`, the time of the second).

use std::cell::RefCell;

/// Node's `http.STATUS_CODES` (writeHead: "unknown" for a code it has no phrase for)
pub(crate) fn reason(code: u16) -> &'static str {
    match code {
        100 => "Continue",
        101 => "Switching Protocols",
        102 => "Processing",
        103 => "Early Hints",
        200 => "OK",
        201 => "Created",
        202 => "Accepted",
        203 => "Non-Authoritative Information",
        204 => "No Content",
        205 => "Reset Content",
        206 => "Partial Content",
        207 => "Multi-Status",
        208 => "Already Reported",
        226 => "IM Used",
        300 => "Multiple Choices",
        301 => "Moved Permanently",
        302 => "Found",
        303 => "See Other",
        304 => "Not Modified",
        305 => "Use Proxy",
        307 => "Temporary Redirect",
        308 => "Permanent Redirect",
        400 => "Bad Request",
        401 => "Unauthorized",
        402 => "Payment Required",
        403 => "Forbidden",
        404 => "Not Found",
        405 => "Method Not Allowed",
        406 => "Not Acceptable",
        407 => "Proxy Authentication Required",
        408 => "Request Timeout",
        409 => "Conflict",
        410 => "Gone",
        411 => "Length Required",
        412 => "Precondition Failed",
        413 => "Payload Too Large",
        414 => "URI Too Long",
        415 => "Unsupported Media Type",
        416 => "Range Not Satisfiable",
        417 => "Expectation Failed",
        418 => "I'm a Teapot",
        421 => "Misdirected Request",
        422 => "Unprocessable Entity",
        423 => "Locked",
        424 => "Failed Dependency",
        425 => "Too Early",
        426 => "Upgrade Required",
        428 => "Precondition Required",
        429 => "Too Many Requests",
        431 => "Request Header Fields Too Large",
        451 => "Unavailable For Legal Reasons",
        500 => "Internal Server Error",
        501 => "Not Implemented",
        502 => "Bad Gateway",
        503 => "Service Unavailable",
        504 => "Gateway Timeout",
        505 => "HTTP Version Not Supported",
        506 => "Variant Also Negotiates",
        507 => "Insufficient Storage",
        508 => "Loop Detected",
        509 => "Bandwidth Limit Exceeded",
        510 => "Not Extended",
        511 => "Network Authentication Required",
        _ => "unknown",
    }
}

const DAYS: [&str; 7] = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
const MONTHS: [&str; 12] = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];

/// JS `new Date(ms).toUTCString()`: "Tue, 21 Oct 2026 07:28:00 GMT"
pub(crate) fn utc_string(ms: i64) -> String {
    let (y, m, d, h, mi, s, _) = nfsu2_util::js::utc_parts(ms);
    let wd = (ms.div_euclid(86_400_000) + 4).rem_euclid(7) as usize;
    let year = if (0..=9999).contains(&y) { format!("{y:04}") } else { format!("{y}") };
    format!("{}, {d:02} {} {year} {h:02}:{mi:02}:{s:02} GMT", DAYS[wd], MONTHS[(m - 1) as usize])
}

thread_local! {
    /// the second and its Date (Node caches it the same way, for the rest of the second)
    static DATE: RefCell<(i64, String)> = const { RefCell::new((i64::MIN, String::new())) };
}

/// appends Node's `Date` value for now
pub(crate) fn push_date(out: &mut Vec<u8>) {
    let ms = nfsu2_util::clock::now_ms();
    let sec = ms.div_euclid(1000);
    DATE.with(|d| {
        let mut d = d.borrow_mut();
        if d.0 != sec {
            *d = (sec, utc_string(sec * 1000));
        }
        out.extend_from_slice(d.1.as_bytes());
    });
}

/// JS `/(?:^|\W)word(?:$|\W)/i.test(value)` (word in lower case): Node's tests for `close`, `chunked`, `100-continue`
pub(crate) fn has_token(value: &str, word: &str) -> bool {
    let v = value.as_bytes();
    let w = word.as_bytes();
    if v.len() < w.len() {
        return false;
    }
    let is_word = |b: u8| b.is_ascii_alphanumeric() || b == b'_';
    (0..=v.len() - w.len()).any(|i| {
        v[i..i + w.len()].eq_ignore_ascii_case(w) && (i == 0 || !is_word(v[i - 1])) && (i + w.len() == v.len() || !is_word(v[i + w.len()]))
    })
}

/// appends `s` as Node writes a head (latin1: one byte per character); false (and nothing appended) for a character
/// Node refuses in a header (outside tab, visible ASCII and \x80-\xff, as `checkInvalidHeaderChar`)
pub(crate) fn push_latin1(out: &mut Vec<u8>, s: &str) -> bool {
    if s.bytes().all(|b| b == b'\t' || (0x20..=0x7e).contains(&b)) {
        out.extend_from_slice(s.as_bytes());
        return true;
    }
    let start = out.len();
    for c in s.chars() {
        let c = c as u32;
        if c == 9 || (0x20..=0x7e).contains(&c) || (0x80..=0xff).contains(&c) {
            out.push(c as u8);
        } else {
            out.truncate(start);
            return false;
        }
    }
    true
}

/// a header name Node takes (`checkIsHttpToken`)
pub(crate) fn token(name: &str) -> bool {
    !name.is_empty()
        && name.bytes().all(|b| {
            b.is_ascii_alphanumeric()
                || matches!(b, b'!' | b'#' | b'$' | b'%' | b'&' | b'\'' | b'*' | b'+' | b'-' | b'.' | b'^' | b'_' | b'`' | b'|' | b'~')
        })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dates_as_js_writes_them() {
        assert_eq!(utc_string(0), "Thu, 01 Jan 1970 00:00:00 GMT");
        assert_eq!(utc_string(1_792_567_680_000), "Wed, 21 Oct 2026 07:28:00 GMT");
        assert_eq!(utc_string(951_782_400_000), "Tue, 29 Feb 2000 00:00:00 GMT");
        let mut d = Vec::new();
        push_date(&mut d);
        assert_eq!(d.len(), 29);
    }

    #[test]
    fn tokens_as_the_regexes_find_them() {
        assert!(has_token("close", "close"));
        assert!(has_token("keep-alive, Close", "close"));
        assert!(!has_token("closed", "close"));
        assert!(!has_token("xclose", "close"));
        assert!(has_token("x-close", "close"));
        assert!(has_token("trailers, chunked", "chunked"));
        assert!(!has_token("100-continuex", "100-continue"));
        assert!(has_token("x, 100-Continue", "100-continue"));
        assert!(!has_token("", "close"));
    }

    /// against JS itself: toUTCString over many times, STATUS_CODES for every code writeHead takes, and the three
    /// regexes over strings built to sit on their edges
    #[test]
    fn as_node_computes_them() {
        let mut rng = crate::node::Rng(7);
        let times: Vec<i64> =
            (0..3000).map(|i| if i < 10 { i * 86_399_999 } else { (rng.next() % 8_000_000_000_000) as i64 - 1_000_000_000_000 }).collect();
        let pieces = [
            "close",
            "Close",
            "CLOSE",
            "chunked",
            "100-continue",
            "100-Continue",
            "x",
            "_",
            "-",
            ",",
            " ",
            "\t",
            "9",
            "é",
            "keep-alive",
            "closed",
        ];
        let strings: Vec<String> = (0..3000).map(|_| (0..rng.below(5)).map(|_| rng.pick(&pieces)).collect()).collect();
        let input = serde_json::json!({ "times": times, "strings": strings }).to_string();
        let script = r#"
            import http from 'node:http';
            let s = ''; process.stdin.on('data', d => { s += d; });
            process.stdin.on('end', () => {
              const { times, strings } = JSON.parse(s);
              const codes = []; for (let c = 100; c <= 999; c++) codes.push(http.STATUS_CODES[c] || 'unknown');
              const re = w => new RegExp(`(?:^|\\W)${w}(?:$|\\W)`, 'i');
              process.stdout.write(JSON.stringify({
                dates: times.map(t => new Date(t).toUTCString()),
                codes,
                tokens: strings.map(x => [re('close').test(x), re('chunked').test(x), re('100-continue').test(x)]),
              }));
            });"#;
        let Some(out) = crate::node::run(script, &input) else {
            eprintln!("skipped: no node to compare with");
            return;
        };
        let v: serde_json::Value = serde_json::from_str(&out).unwrap_or_default();
        assert_eq!(v["dates"].as_array().map(Vec::len), Some(times.len()));
        assert_eq!(v["codes"].as_array().map(Vec::len), Some(900));
        assert_eq!(v["tokens"].as_array().map(Vec::len), Some(strings.len()));
        for (t, want) in times.iter().zip(v["dates"].as_array().into_iter().flatten()) {
            assert_eq!(utc_string(*t), want.as_str().unwrap_or_default(), "{t}");
        }
        for (c, want) in (100..=999u16).zip(v["codes"].as_array().into_iter().flatten()) {
            assert_eq!(reason(c), want.as_str().unwrap_or_default(), "{c}");
        }
        for (x, want) in strings.iter().zip(v["tokens"].as_array().into_iter().flatten()) {
            let got = [has_token(x, "close"), has_token(x, "chunked"), has_token(x, "100-continue")];
            let want: Vec<bool> = want.as_array().into_iter().flatten().map(|b| b.as_bool().unwrap_or_default()).collect();
            assert_eq!(got.to_vec(), want, "{x:?}");
        }
    }

    #[test]
    fn latin1_heads() {
        let mut o = Vec::new();
        assert!(push_latin1(&mut o, "a\u{e9}b"));
        assert_eq!(o, b"a\xe9b");
        assert!(!push_latin1(&mut o, "a\r\nb"));
        assert!(!push_latin1(&mut o, "\u{100}"));
        assert_eq!(o, b"a\xe9b");
    }
}
