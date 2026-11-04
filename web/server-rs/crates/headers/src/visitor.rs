//! Cloudflare's `cf-visitor` header as the Node server this replaced read it: `JSON.parse(h).scheme === 'https'`,
//! false where the parse throws.  This answers the same without building a value: the text is checked against
//! JSON.parse's grammar (ECMA-404, any depth, lone surrogates in \u escapes allowed, as V8 takes them; serde_json
//! refuses those and more than 128 levels), and only the top-level object's last "scheme" member (JSON.parse keeps
//! the last of duplicate keys) is decoded.

/// `JSON.parse(text).scheme === 'https'`, false where JSON.parse throws (or the value has no properties: null)
pub(crate) fn scheme_is_https(text: &str) -> bool {
    Parser { b: text.as_bytes(), i: 0 }.top().unwrap_or(false)
}

struct Parser<'a> {
    b: &'a [u8],
    i: usize,
}

const SCHEME: [u16; 6] = [b's' as u16, b'c' as u16, b'h' as u16, b'e' as u16, b'm' as u16, b'e' as u16];
const HTTPS: [u16; 5] = [b'h' as u16, b't' as u16, b't' as u16, b'p' as u16, b's' as u16];

impl Parser<'_> {
    fn peek(&self) -> Option<u8> {
        self.b.get(self.i).copied()
    }

    fn next(&mut self) -> Option<u8> {
        let c = self.peek()?;
        self.i += 1;
        Some(c)
    }

    /// JSON's whitespace: tab, line feed, carriage return, space (not JS's \s)
    fn ws(&mut self) {
        while matches!(self.peek(), Some(b' ' | b'\t' | b'\n' | b'\r')) {
            self.i += 1;
        }
    }

    /// The whole text: one value, nothing but whitespace around it.
    fn top(&mut self) -> Option<bool> {
        // the open containers (true: object); the top-level object's "scheme" value so far
        let mut stack: Vec<bool> = Vec::new();
        let mut top_object = false;
        let mut https = false;
        let mut member_is_scheme = false;
        'value: loop {
            self.ws();
            // the value of the top-level object's "scheme" member replaces any before it
            let is_scheme_value = stack.len() == 1 && stack[0] && member_is_scheme;
            if is_scheme_value {
                https = false;
            }
            match self.peek()? {
                b'{' => {
                    self.i += 1;
                    top_object |= stack.is_empty();
                    stack.push(true);
                    self.ws();
                    if self.peek()? == b'}' {
                        self.i += 1;
                        stack.pop();
                    } else {
                        member_is_scheme = self.key(stack.len() == 1)?;
                        continue 'value;
                    }
                }
                b'[' => {
                    self.i += 1;
                    stack.push(false);
                    self.ws();
                    if self.peek()? == b']' {
                        self.i += 1;
                        stack.pop();
                    } else {
                        continue 'value;
                    }
                }
                b'"' => {
                    if is_scheme_value {
                        https = self.string(true)?.as_deref() == Some(&HTTPS[..]);
                    } else {
                        self.string(false)?;
                    }
                }
                b't' => self.word(b"true")?,
                b'f' => self.word(b"false")?,
                b'n' => self.word(b"null")?,
                _ => self.number()?,
            }
            // after a value: close what ends here, or go on to the next element or member
            loop {
                self.ws();
                let Some(&object) = stack.last() else { break 'value };
                match self.next()? {
                    b',' => {
                        if object {
                            self.ws();
                            member_is_scheme = self.key(stack.len() == 1)?;
                        }
                        continue 'value;
                    }
                    b'}' if object => {
                        stack.pop();
                    }
                    b']' if !object => {
                        stack.pop();
                    }
                    _ => return None,
                }
            }
        }
        self.ws();
        (self.i == self.b.len()).then_some(top_object && https)
    }

    /// A member's name and its ':'; whether the name is "scheme" (decoded only where `decode`).
    fn key(&mut self, decode: bool) -> Option<bool> {
        if self.peek()? != b'"' {
            return None;
        }
        let name = self.string(decode)?;
        self.ws();
        if self.next()? != b':' {
            return None;
        }
        Some(name.as_deref() == Some(&SCHEME[..]))
    }

    /// A string at its opening quote; its UTF-16 code units where `decode` (a character beyond ASCII stands as
    /// U+FFFF there: only "scheme" and "https" are ever compared with).
    fn string(&mut self, decode: bool) -> Option<Option<Vec<u16>>> {
        self.i += 1;
        let mut out = decode.then(Vec::new);
        loop {
            let c = self.next()?;
            let unit = match c {
                b'"' => return Some(out),
                b'\\' => match self.next()? {
                    b'"' => b'"' as u16,
                    b'\\' => b'\\' as u16,
                    b'/' => b'/' as u16,
                    b'b' => 8,
                    b'f' => 12,
                    b'n' => 10,
                    b'r' => 13,
                    b't' => 9,
                    b'u' => {
                        let hex = self.b.get(self.i..self.i + 4)?;
                        self.i += 4;
                        hex.iter().try_fold(0u16, |a, &h| Some((a << 4) | (h as char).to_digit(16)? as u16))?
                    }
                    _ => return None,
                },
                // a control character must be escaped
                0..=0x1f => return None,
                0x20..=0x7f => c as u16,
                // the text is UTF-8 already (a &str): its continuation bytes need no check
                _ => 0xffff,
            };
            if let Some(o) = out.as_mut() {
                o.push(unit);
            }
        }
    }

    fn word(&mut self, w: &[u8]) -> Option<()> {
        if self.b.get(self.i..self.i + w.len())? != w {
            return None;
        }
        self.i += w.len();
        Some(())
    }

    /// `-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?`
    fn number(&mut self) -> Option<()> {
        if self.peek() == Some(b'-') {
            self.i += 1;
        }
        match self.next()? {
            b'0' => {}
            b'1'..=b'9' => self.digits(),
            _ => return None,
        }
        if self.peek() == Some(b'.') {
            self.i += 1;
            self.digit()?;
            self.digits();
        }
        if matches!(self.peek(), Some(b'e' | b'E')) {
            self.i += 1;
            if matches!(self.peek(), Some(b'+' | b'-')) {
                self.i += 1;
            }
            self.digit()?;
            self.digits();
        }
        Some(())
    }

    fn digit(&mut self) -> Option<()> {
        self.next().filter(u8::is_ascii_digit).map(|_| ())
    }

    fn digits(&mut self) {
        while self.peek().is_some_and(|c| c.is_ascii_digit()) {
            self.i += 1;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::scheme_is_https as https;

    #[test]
    fn as_json_parse_reads_it() {
        assert!(https(r#"{"scheme":"https"}"#));
        assert!(https(" {\"scheme\" :\t\"https\"}\r\n"));
        assert!(!https(r#"{"scheme":"http"}"#));
        assert!(!https("{}"));
        assert!(!https(""));
        assert!(!https("null"));
        assert!(!https(r#""https""#));
        assert!(!https(r#"["https"]"#));
        // the last of duplicate keys counts
        assert!(https(r#"{"scheme":"http","scheme":"https"}"#));
        assert!(!https(r#"{"scheme":"https","scheme":{"a":1}}"#));
        assert!(!https(r#"{"scheme":"https","scheme":null}"#));
        // escapes are decoded, lone surrogates taken
        assert!(https(r#"{"scheme":"https"}"#));
        assert!(https(r#"{"scheme":"https","x":"\ud800"}"#));
        // only the top-level object's member
        assert!(!https(r#"{"a":{"scheme":"https"}}"#));
        assert!(https(r#"{"a":{"scheme":"http"},"scheme":"https"}"#));
        assert!(https(r#"{"a":[{"scheme":"http"}],"scheme":"https","b":[]}"#));
        assert!(https(&format!(r#"{{"scheme":"https","d":{}1{}}}"#, "[".repeat(1000), "]".repeat(1000))));
        // what JSON.parse refuses
        for bad in [
            r#"{"scheme":"https",}"#,
            r#"{"scheme":"https"}x"#,
            "{'scheme':'https'}",
            "\u{a0}{\"scheme\":\"https\"}",
            "\u{feff}{\"scheme\":\"https\"}",
            "{\"scheme\":\"https\",\"a\":\"\t\"}",
            r#"{"scheme":"https","a":01}"#,
            r#"{"scheme":"https","a":1.}"#,
            r#"{"scheme":"https","a":.5}"#,
            r#"{"scheme":"https","a":+1}"#,
            r#"{"scheme":"https","a":"\x"}"#,
            r#"{"scheme":"https","a":"\u12"}"#,
            r#"{"scheme":"https""#,
            r#"{"scheme":"https","a":[1,]}"#,
            r#"{"scheme":"https","a":tru}"#,
            r#"{"scheme":"https"}}"#,
        ] {
            assert!(!https(bad), "{bad}");
        }
        assert!(https(r#"{"scheme":"https","a":-0.5e-3,"b":1E+2,"c":[true,false,null]}"#));
    }
}
