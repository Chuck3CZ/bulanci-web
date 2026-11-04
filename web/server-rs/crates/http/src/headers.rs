//! A request's headers as Node's `IncomingMessage` builds `q.headers` (`_addHeaderLine`, `matchKnownFields`): names
//! in lower case; a second line of a header Node keeps once is dropped, `cookie` lines are joined with "; ", all
//! others with ", ".

use crate::Headers;
use crate::parse::latin1;

/// How Node joins a header's lines.
#[derive(Copy, Clone, PartialEq, Eq)]
enum Join {
    /// the first line wins (Node's "no duplicates" fields)
    First,
    /// "; "
    Cookie,
    /// ", "
    List,
}

fn join_of(name: &str) -> Join {
    match name {
        "age"
        | "authorization"
        | "content-length"
        | "content-type"
        | "etag"
        | "expires"
        | "from"
        | "host"
        | "if-modified-since"
        | "if-unmodified-since"
        | "last-modified"
        | "location"
        | "max-forwards"
        | "proxy-authorization"
        | "referer"
        | "retry-after"
        | "server"
        | "user-agent" => Join::First,
        "cookie" => Join::Cookie,
        // (set-cookie, an array in Node, is a list here)
        _ => Join::List,
    }
}

/// Lines with more names than this are joined through an index (a request may carry up to 1000).
const LINEAR: usize = 32;

/// `q.headers` from the request's (name, value) lines in order; values are bytes as they came (latin1, as Node
/// reads them).
pub(crate) fn join<'a>(lines: impl Iterator<Item = (&'a [u8], &'a [u8])>) -> Headers {
    let mut list: Vec<(String, String)> = Vec::new();
    let mut index: Option<std::collections::HashMap<String, usize>> = None;
    for (name, value) in lines {
        let name = lower(name);
        let at = match &index {
            Some(ix) => ix.get(&name).copied(),
            None => list.iter().position(|(k, _)| *k == name),
        };
        match at {
            None => {
                if let Some(ix) = &mut index {
                    ix.insert(name.clone(), list.len());
                }
                list.push((name, latin1(value)));
                if index.is_none() && list.len() > LINEAR {
                    index = Some(list.iter().enumerate().map(|(i, (k, _))| (k.clone(), i)).collect());
                }
            }
            Some(i) => match join_of(&list[i].0) {
                Join::First => {}
                Join::Cookie => {
                    list[i].1.push_str("; ");
                    list[i].1.push_str(&latin1(value));
                }
                Join::List => {
                    list[i].1.push_str(", ");
                    list[i].1.push_str(&latin1(value));
                }
            },
        }
    }
    Headers { list }
}

/// a header name (a token: ASCII) in lower case
fn lower(name: &[u8]) -> String {
    let mut s = String::with_capacity(name.len());
    for &b in name {
        s.push(b.to_ascii_lowercase() as char);
    }
    s
}

impl Headers {
    /// a header by its name (any case)
    pub fn get(&self, name: &str) -> Option<&str> {
        self.list.iter().find(|(k, _)| k.eq_ignore_ascii_case(name)).map(|(_, v)| v.as_str())
    }
    /// how many headers (after joining)
    pub fn len(&self) -> usize {
        self.list.len()
    }
    pub fn is_empty(&self) -> bool {
        self.list.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn h(lines: &[(&str, &str)]) -> Vec<(String, String)> {
        join(lines.iter().map(|(k, v)| (k.as_bytes(), v.as_bytes()))).list
    }

    #[test]
    fn node_joins_as_it_does() {
        let got = h(&[
            ("Host", "a"),
            ("HOST", "b"),
            ("X-Foo", "1"),
            ("x-foo", "2"),
            ("Cookie", "a=1"),
            ("cookie", "b=2"),
            ("Accept", "x"),
            ("ACCEPT", "y"),
            ("User-Agent", "u"),
            ("user-agent", "v"),
        ]);
        let want: Vec<(String, String)> =
            [("host", "a"), ("x-foo", "1, 2"), ("cookie", "a=1; b=2"), ("accept", "x, y"), ("user-agent", "u")]
                .iter()
                .map(|(k, v)| (k.to_string(), v.to_string()))
                .collect();
        assert_eq!(got, want);
    }

    #[test]
    fn many_lines_join_the_same_through_the_index() {
        let names: Vec<String> = (0..100).map(|i| format!("x-{i}")).collect();
        let mut lines: Vec<(&str, &str)> = names.iter().map(|n| (n.as_str(), "a")).collect();
        lines.push(("X-7", "b"));
        lines.push(("host", "h"));
        lines.push(("Host", "i"));
        let got = h(&lines);
        assert_eq!(got.len(), 101);
        assert_eq!(got[7].1, "a, b");
        assert_eq!(got[100], ("host".to_string(), "h".to_string()));
    }

    /// Node's own IncomingMessage joins random header lists (known fields in their usual and odd cases, others,
    /// repeats): q.headers must come out the same
    #[test]
    fn joined_as_nodes_incoming_message_joins_them() {
        let names = [
            "Host",
            "host",
            "HOST",
            "Cookie",
            "cookie",
            "Content-Type",
            "content-type",
            "CONTENT-TYPE",
            "User-Agent",
            "Accept",
            "accept",
            "X-Forwarded-For",
            "x-forwarded-for",
            "Authorization",
            "Referer",
            "Age",
            "ETag",
            "Etag",
            "Date",
            "Vary",
            "Origin",
            "Expect",
            "Upgrade",
            "Connection",
            "Content-Length",
            "If-None-Match",
            "Cache-Control",
            "Accept-Encoding",
            "Accept-Language",
            "X-Forwarded-Host",
            "X-Forwarded-Proto",
            "Content-Encoding",
            "Transfer-Encoding",
            "If-Match",
            "Location",
            "Server",
            "From",
            "Expires",
            "Last-Modified",
            "Retry-After",
            "Max-Forwards",
            "Proxy-Authorization",
            "If-Modified-Since",
            "If-Unmodified-Since",
            "X-A",
            "x-a",
            "Sec-WebSocket-Key",
            "CF-Connecting-IP",
            "cf-connecting-ip",
            "Range",
            "TE",
        ];
        let values = ["a", "b", "1, 2", "x=y", "", "c; d", "\u{e9}"];
        let mut rng = crate::node::Rng(42);
        let mut cases = Vec::new();
        for _ in 0..2000 {
            let n = 1 + rng.below(8);
            let lines: Vec<(String, String)> = (0..n).map(|_| (rng.pick(&names).to_string(), rng.pick(&values).to_string())).collect();
            cases.push(lines);
        }
        let input = serde_json::to_string(&cases).unwrap_or_default();
        let script = r#"
            import { IncomingMessage } from 'node:http';
            let s = ''; process.stdin.on('data', d => { s += d; });
            process.stdin.on('end', () => {
              const out = JSON.parse(s).map(lines => {
                const m = new IncomingMessage(null);
                m._addHeaderLines(lines.flat(), lines.length * 2);
                return Object.entries(m.headers).map(([k, v]) => [k, Array.isArray(v) ? v.join(', ') : v]);
              });
              process.stdout.write(JSON.stringify(out));
            });"#;
        let Some(out) = crate::node::run(script, &input) else {
            eprintln!("skipped: no node to compare with");
            return;
        };
        let want: Vec<Vec<(String, String)>> = serde_json::from_str(&out).unwrap_or_default();
        assert_eq!(want.len(), cases.len());
        for (lines, want) in cases.iter().zip(want) {
            // the bytes on the wire are latin1 (é is one byte), as Node reads them
            let raw: Vec<(Vec<u8>, Vec<u8>)> =
                lines.iter().map(|(k, v)| (k.as_bytes().to_vec(), v.chars().map(|c| c as u32 as u8).collect())).collect();
            let got = join(raw.iter().map(|(k, v)| (k.as_slice(), v.as_slice()))).list;
            assert_eq!(got, want, "{lines:?}");
        }
    }

    #[test]
    fn values_are_latin1() {
        let got = join([(&b"x"[..], &b"a\xffb"[..])].into_iter()).list;
        assert_eq!(got[0].1, "a\u{ff}b");
    }
}
