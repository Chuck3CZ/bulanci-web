//! IP addresses as Node reads them: `net.isIPv4` / `net.isIPv6` (the regexes of Node 24's lib/internal/net.js,
//! written out by hand), the value an address stands for, an address in its plain form (`bare`), and the WHATWG URL
//! serializer of an IPv6 host (what `new URL('http://[…]/').hostname` gives).

use std::fmt::Write;

use nfsu2_util::js;

/// An address that passed `net.isIP`, as its value.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum Ip {
    V4(u32),
    V6([u16; 8]),
}

impl Ip {
    /// The address on one line for both families, the way Node's BlockList compares across them: an IPv4 address
    /// is its IPv4-mapped IPv6 address (::ffff:a.b.c.d).
    pub(crate) fn as_v6(self) -> u128 {
        match self {
            Ip::V4(v) => MAPPED | v as u128,
            Ip::V6(g) => g.iter().fold(0u128, |a, &x| (a << 16) | x as u128),
        }
    }
}

/// ::ffff:0:0, the IPv4-mapped prefix (/96)
pub(crate) const MAPPED: u128 = 0xffff << 32;

/// One part of Node's v4Seg, `25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9][0-9]|[0-9]`: 0 to 255 without a leading zero.
fn v4_octet(s: &[u8]) -> Option<u32> {
    if s.is_empty() || s.len() > 3 || !s.iter().all(u8::is_ascii_digit) || (s.len() > 1 && s[0] == b'0') {
        return None;
    }
    let v = s.iter().fold(0u32, |a, &c| a * 10 + (c - b'0') as u32);
    (v <= 255).then_some(v)
}

/// Node's IPv4Reg (`^v4Seg(\.v4Seg){3}$`): the address, or None.
pub(crate) fn v4(s: &str) -> Option<u32> {
    v4_bytes(s.as_bytes())
}

fn v4_bytes(s: &[u8]) -> Option<u32> {
    let mut v = 0u32;
    let mut n = 0;
    for part in s.split(|&c| c == b'.') {
        if n == 4 {
            return None;
        }
        v = (v << 8) | v4_octet(part)?;
        n += 1;
    }
    (n == 4).then_some(v)
}

/// Node's v6Seg: one to four hex digits, either case.
fn hex_group(s: &[u8]) -> Option<u16> {
    if s.is_empty() || s.len() > 4 {
        return None;
    }
    s.iter().try_fold(0u16, |a, &c| Some((a << 4) | (c as char).to_digit(16)? as u16))
}

/// The address part (before a zone) of Node's IPv6Reg, as its eight groups.  The regex's alternatives come to:
/// eight groups, or six and a dotted IPv4 tail; or, around one "::", a head of k groups and a tail of m (the tail
/// may end in a dotted IPv4, which counts as two) with k + m at most 7.
pub(crate) fn v6_groups(s: &str) -> Option<[u16; 8]> {
    let b = s.as_bytes();
    let mut g = [0u16; 8];
    match b.windows(2).position(|w| w == b"::") {
        None => {
            let parts = b.split(|&c| c == b':');
            let mut n = 0;
            let mut dotted = false;
            for part in parts {
                if n == 8 || dotted {
                    return None;
                }
                if part.contains(&b'.') {
                    // the dotted tail stands for the last two groups
                    if n != 6 {
                        return None;
                    }
                    let v = v4_bytes(part)?;
                    g[6] = (v >> 16) as u16;
                    g[7] = v as u16;
                    n = 8;
                    dotted = true;
                } else {
                    g[n] = hex_group(part)?;
                    n += 1;
                }
            }
            (n == 8).then_some(g)
        }
        Some(i) => {
            let (head, tail) = (&b[..i], &b[i + 2..]);
            let mut k = 0;
            if !head.is_empty() {
                for part in head.split(|&c| c == b':') {
                    if k == 7 {
                        return None;
                    }
                    g[k] = hex_group(part)?;
                    k += 1;
                }
            }
            let mut ts = [0u16; 8];
            let mut m = 0;
            if !tail.is_empty() {
                let mut dotted = false;
                for part in tail.split(|&c| c == b':') {
                    // a dotted IPv4 ends the address; nothing may follow it
                    if dotted || k + m >= 7 {
                        return None;
                    }
                    if part.contains(&b'.') {
                        if k + m + 2 > 7 {
                            return None;
                        }
                        let v = v4_bytes(part)?;
                        ts[m] = (v >> 16) as u16;
                        ts[m + 1] = v as u16;
                        m += 2;
                        dotted = true;
                    } else {
                        ts[m] = hex_group(part)?;
                        m += 1;
                    }
                }
            }
            if k + m > 7 {
                return None;
            }
            g[8 - m..].copy_from_slice(&ts[..m]);
            Some(g)
        }
    }
}

/// Node's `net.isIPv6`: the address, then an optional zone (`%[0-9a-zA-Z-.:]{1,}`).
pub(crate) fn is_v6(s: &str) -> bool {
    let addr = match s.find('%') {
        Some(p) => {
            let zone = &s.as_bytes()[p + 1..];
            if zone.is_empty() || !zone.iter().all(|&c| c.is_ascii_alphanumeric() || matches!(c, b'-' | b'.' | b':')) {
                return false;
            }
            &s[..p]
        }
        None => s,
    };
    v6_groups(addr).is_some()
}

/// Node's `net.isIP`: 4, 6 or 0.
pub(crate) fn is_ip(s: &str) -> u8 {
    if v4(s).is_some() {
        4
    } else if is_v6(s) {
        6
    } else {
        0
    }
}

/// `/^::ffff:(\d+\.\d+\.\d+\.\d+)$/i`: an IPv4-mapped address in dotted form (the octets unchecked, as there)
fn mapped_dotted(s: &str) -> bool {
    let b = s.as_bytes();
    if b.len() <= 7 || !b[..7].eq_ignore_ascii_case(b"::ffff:") {
        return false;
    }
    let mut n = 0;
    for part in b[7..].split(|&c| c == b'.') {
        if part.is_empty() || !part.iter().all(u8::is_ascii_digit) {
            return false;
        }
        n += 1;
    }
    n == 4
}

/// `bare_ip` before its lower-casing: the address text (a slice of `a`, in its own case) and its value;
/// None when it is not an address.  A socket's "::ffff:1.2.3.4" (an IPv4 client of a dual-stack listener) is
/// "1.2.3.4", "[::1]" is "::1", and a zone ("fe80::1%en0") goes.
pub(crate) fn bare(a: &str) -> Option<(&str, Ip)> {
    let mut s = js::trim(a);
    if s.starts_with('[') && s.ends_with(']') {
        s = &s[1..s.len() - 1];
    }
    if let Some(p) = s.find('%').filter(|&p| p > 0) {
        s = &s[..p];
    }
    if mapped_dotted(s) {
        s = &s[7..];
    }
    if let Some(v) = v4(s) {
        return Some((s, Ip::V4(v)));
    }
    // no zone is left (one at the very start is no address): the address part is all there is
    v6_groups(s).map(|g| (s, Ip::V6(g)))
}

/// The WHATWG URL serializer of an IPv6 host: lower-case hex without leading zeros, the first longest run of two
/// or more zero groups as "::", and a dotted IPv4 tail as two hex groups (Rust's `Ipv6Addr` writes some as dotted).
pub(crate) fn serialize6(g: &[u16; 8]) -> String {
    let (mut compress, mut longest) = (None, 1);
    let mut i = 0;
    while i < 8 {
        if g[i] != 0 {
            i += 1;
            continue;
        }
        let start = i;
        while i < 8 && g[i] == 0 {
            i += 1;
        }
        if i - start > longest {
            compress = Some(start);
            longest = i - start;
        }
    }
    let mut out = String::with_capacity(39);
    let mut ignore0 = false;
    for (idx, &piece) in g.iter().enumerate() {
        if ignore0 && piece == 0 {
            continue;
        }
        ignore0 = false;
        if compress == Some(idx) {
            out.push_str(if idx == 0 { "::" } else { ":" });
            ignore0 = true;
            continue;
        }
        let _ = write!(out, "{piece:x}");
        if idx != 7 {
            out.push(':');
        }
    }
    out
}

/// An IPv6 host as a URL holds it: `new URL('http://[' + s + ']/').hostname` without its brackets, '' where that
/// throws (a zone: the URL parser takes no '%' in an IPv6 host).  `s` passed `net.isIPv6`.
pub(crate) fn canon6(s: &str) -> String {
    if s.contains('%') {
        return String::new();
    }
    v6_groups(s).map(|g| serialize6(&g)).unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ipv4_as_node_reads_it() {
        assert_eq!(v4("1.2.3.4"), Some(0x01020304));
        assert_eq!(v4("255.255.255.255"), Some(u32::MAX));
        assert_eq!(v4("0.0.0.0"), Some(0));
        for bad in ["01.2.3.4", "1.2.3.04", "256.1.1.1", "1.2.3", "1.2.3.4.", "1.2.3.4.5", "1..3.4", "", "1.2.3.-4", "1.2.3.4 ", "١.2.3.4"]
        {
            assert_eq!(v4(bad), None, "{bad}");
        }
    }

    #[test]
    fn ipv6_as_node_reads_it() {
        for ok in [
            "::",
            "::1",
            "1::",
            "1:2:3:4:5:6:7:8",
            "1:2:3:4:5:6:7::",
            "::2:3:4:5:6:7:8",
            "1::8",
            "1:2:3:4:5:6::8",
            "::ffff:1.2.3.4",
            "1:2:3:4:5:6:1.2.3.4",
            "1::1.2.3.4",
            "1:2:3:4:5::1.2.3.4",
            "1:2:3:4::5:1.2.3.4",
            "ABCD::ef",
            "fe80::1%eth0",
            "fe80::1%2",
            "::1%a-b.c:d",
            "0000:0000::0001",
        ] {
            assert!(is_v6(ok), "{ok}");
        }
        for bad in [
            "",
            ":",
            ":::",
            "1:::2",
            "1::2::3",
            "1:2:3:4:5:6:7:8:9",
            "1:2:3:4:5:6:7",
            "1:2:3:4:5:6:7:8::",
            "::1:2:3:4:5:6:7:8",
            "12345::",
            "g::",
            "1:2:3:4:5:6::1.2.3.4",
            "1:2:3:4:5:1.2.3.4",
            "::1.2.3.4:5",
            "::01.2.3.4",
            "::1.2.3",
            "fe80::1%",
            "fe80::1%e/0",
            "fe80::1%e%0",
            "[::1]",
            " ::1",
            ":1::2",
            "1::2:",
        ] {
            assert!(!is_v6(bad), "{bad}");
        }
        assert_eq!(v6_groups("1:2:3:4:5:6:7::"), Some([1, 2, 3, 4, 5, 6, 7, 0]));
        assert_eq!(v6_groups("::ffff:1.2.3.4"), Some([0, 0, 0, 0, 0, 0xffff, 0x102, 0x304]));
        assert_eq!(v6_groups("1::8"), Some([1, 0, 0, 0, 0, 0, 0, 8]));
    }

    #[test]
    fn whatwg_writes_ipv6_hosts() {
        for (a, s) in [
            ("::", "::"),
            ("0:0:0:0:0:0:0:1", "::1"),
            ("::ffff:1.2.3.4", "::ffff:102:304"),
            ("2001:DB8::0001", "2001:db8::1"),
            ("1:0:0:2:0:0:0:3", "1:0:0:2::3"),
            ("1:0:0:2:0:0:3:4", "1::2:0:0:3:4"),
            ("1:0:2:3:4:5:6:7", "1:0:2:3:4:5:6:7"),
            ("1:2:3:4:5:6:7::", "1:2:3:4:5:6:7:0"),
            ("1::", "1::"),
            ("::1.2.3.4", "::102:304"),
            ("fe80::1%eth0", ""),
        ] {
            assert_eq!(canon6(a), s, "{a}");
        }
    }

    #[test]
    fn bare_addresses() {
        assert_eq!(bare("::FFFF:1.2.3.4").map(|b| b.0), Some("1.2.3.4"));
        assert_eq!(bare(" [fe80::1%en0] ").map(|b| b.0), Some("fe80::1"));
        assert_eq!(bare("%1"), None);
        assert_eq!(bare("::ffff:01.2.3.4"), None);
        assert_eq!(bare("::ffff:7f00:1").map(|b| b.1.as_v6()), Some(MAPPED | 0x7f00_0001));
    }
}
