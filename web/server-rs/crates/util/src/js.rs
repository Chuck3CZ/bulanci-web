//! Text and numbers the way JavaScript makes them, so that what the server prints, stores and sends is byte for byte
//! what the Node server it replaced did (the state files it left, the pages and tools that read the answers).

/// JS `\s` and what `String#trim()` removes: WhiteSpace and LineTerminator (ECMA-262), which is not Rust's
/// `char::is_whitespace` (that has U+0085 and not U+FEFF).
#[inline]
pub fn is_space(c: char) -> bool {
    matches!(
        c,
        '\u{0009}' | '\u{000A}' | '\u{000B}' | '\u{000C}' | '\u{000D}' | '\u{0020}' | '\u{00A0}' | '\u{1680}' | '\u{2000}'
            ..='\u{200A}' | '\u{2028}' | '\u{2029}' | '\u{202F}' | '\u{205F}' | '\u{3000}' | '\u{FEFF}'
    )
}

/// JS `String#trim()`
pub fn trim(s: &str) -> &str {
    s.trim_matches(is_space)
}

/// JS `s.replace(/\s+/g, rep)`
pub fn collapse_space(s: &str, rep: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut in_space = false;
    for c in s.chars() {
        if is_space(c) {
            if !in_space {
                out.push_str(rep);
                in_space = true;
            }
        } else {
            out.push(c);
            in_space = false;
        }
    }
    out
}

/// Length in UTF-16 code units (JS `s.length`).
pub fn utf16_len(s: &str) -> usize {
    s.chars().map(char::len_utf16).sum()
}

/// Text for a console line: every character outside printable ASCII a '?', at most `n` of them (JS
/// `String(x).replace(/[^\x20-\x7e]/g, '?').slice(0, n)`, over UTF-16 code units: a character outside the BMP is two
/// code units, so two '?').
pub fn printable(s: &str, n: usize) -> String {
    let mut out = String::with_capacity(s.len().min(n));
    for c in s.chars() {
        if out.len() >= n {
            break;
        }
        if (' '..='~').contains(&c) {
            out.push(c);
        } else {
            for _ in 0..c.len_utf16() {
                out.push('?');
            }
        }
    }
    out.truncate(n);
    out
}

/// JS `Number.prototype.toString()` (radix 10): the shortest digits that read back as the same double, in JS's
/// layout (no exponent from 1e-6 up to 1e21; "1e+21", "1.5e-7" beyond).
pub fn number_to_string(x: f64) -> String {
    if x.is_nan() {
        return "NaN".into();
    }
    if x == 0.0 {
        return "0".into();
    }
    if x.is_infinite() {
        return if x > 0.0 { "Infinity".into() } else { "-Infinity".into() };
    }
    if x < 0.0 {
        return format!("-{}", number_to_string(-x));
    }
    // Rust's `{:e}` is the shortest round-trip digits in exponent form: "d.ddde<exp>"
    let e = format!("{x:e}");
    let (mant, exp) = e.split_once('e').expect("exponent form");
    let exp: i32 = exp.parse().expect("exponent");
    let mut digits: String = mant.chars().filter(|c| *c != '.').collect();
    let n = exp + 1; // value = 0.digits * 10^n
    if let Some(even) = even_of_a_tie(x, &digits, n) {
        digits = even;
    }
    let k = digits.len() as i32;
    if k <= n && n <= 21 {
        let mut s = digits;
        for _ in 0..(n - k) {
            s.push('0');
        }
        s
    } else if 0 < n && n <= 21 {
        format!("{}.{}", &digits[..n as usize], &digits[n as usize..])
    } else if -6 < n && n <= 0 {
        format!("0.{}{}", "0".repeat((-n) as usize), digits)
    } else {
        let sign = if n - 1 < 0 { '-' } else { '+' };
        if k == 1 {
            format!("{}e{}{}", digits, sign, (n - 1).abs())
        } else {
            format!("{}.{}e{}{}", &digits[..1], &digits[1..], sign, (n - 1).abs())
        }
    }
}

/// When x lies exactly halfway between two shortest forms (digits, and the one a unit of its last digit away), JS
/// takes the even one (Number::toString: "if there are two such possible values of s, choose the one that is
/// even"); Rust's formatter may take the other.  Only a double with few significant bits just past the point can be
/// such a tie (a float32's value, as the game's race times are): its exact value then has one digit more than the
/// shortest form, a 5.  Some(the even digits) where Rust took the odd ones.
fn even_of_a_tie(x: f64, digits: &str, n: i32) -> Option<String> {
    let exact = short_exact_digits(x)?;
    let k = digits.len();
    if exact.len() != k + 1 || !exact.ends_with('5') || digits.as_bytes()[k - 1] & 1 == 0 {
        return None;
    }
    // the two candidates: the exact digits cut after k, and one more (a carry past the first digit makes "10...0",
    // even already)
    let lower = &exact[..k];
    let mut upper = lower.as_bytes().to_vec();
    let mut i = k;
    loop {
        if i == 0 {
            return None;
        }
        i -= 1;
        if upper[i] == b'9' {
            upper[i] = b'0';
        } else {
            upper[i] += 1;
            break;
        }
    }
    let upper = String::from_utf8(upper).ok()?;
    let other = if digits == lower {
        upper
    } else if digits == upper {
        lower.to_string()
    } else {
        return None;
    };
    // the even one, if it reads back as x too (at a power of two the gap below is half the one above)
    let back = format!("0.{other}e{n}").parse::<f64>().ok()?;
    (back == x).then(|| other.trim_end_matches('0').to_string()).filter(|s| !s.is_empty())
}

/// The significant digits of x's exact value when there are at most 18 of them, else None.  x = m * 2^e with m odd:
/// for e >= 0 an integer (never a tie: its last nonzero digit cannot be a 5 the double's gap reaches), for e < 0 the
/// digits are those of m * 5^-e, which has more than 18 once -e > 25.
fn short_exact_digits(x: f64) -> Option<String> {
    let bits = x.abs().to_bits();
    let exp = ((bits >> 52) & 0x7ff) as i32;
    let frac = bits & ((1u64 << 52) - 1);
    let (mut m, mut e) = if exp == 0 { (frac, -1074) } else { (frac | (1u64 << 52), exp - 1075) };
    if m == 0 {
        return None;
    }
    let tz = m.trailing_zeros();
    m >>= tz;
    e += tz as i32;
    if !(-25..0).contains(&e) {
        return None;
    }
    let p = (m as u128) * 5u128.pow((-e) as u32);
    let s = p.to_string();
    (s.len() <= 18).then_some(s)
}

/// JS `Number.prototype.toFixed(digits)` (0 to 100 digits): the exact value rounded half up at `digits` decimals
/// (Rust's `{:.N}` rounds a tie to even: 0.125 -> "0.12", where JS says "0.13").  1e21 and beyond as `toString()`.
/// More than 100 digits (a RangeError in JS) are 100.
pub fn to_fixed(x: f64, digits: usize) -> String {
    // (past the 1100 decimals worked out below, the digits ran out: a panic)
    let digits = digits.min(100);
    if x.is_nan() {
        return "NaN".into();
    }
    if x.abs() >= 1e21 || x.is_infinite() {
        return number_to_string(x);
    }
    // every double has at most 1074 decimals: this is its exact value
    let exact = format!("{:.1100}", x.abs());
    let (int, frac) = exact.split_once('.').unwrap_or((&exact, ""));
    let mut ds: Vec<u8> = int.bytes().chain(frac.bytes().take(digits)).map(|b| b - b'0').collect();
    let round_up = frac.as_bytes().get(digits).is_some_and(|d| *d >= b'5');
    if round_up {
        let mut i = ds.len();
        loop {
            if i == 0 {
                ds.insert(0, 1);
                break;
            }
            i -= 1;
            if ds[i] == 9 {
                ds[i] = 0;
            } else {
                ds[i] += 1;
                break;
            }
        }
    }
    let int_len = ds.len() - digits;
    let mut s = String::with_capacity(ds.len() + 2);
    // JS keeps the sign of a negative number that rounds to zero: (-0.0001).toFixed(2) is "-0.00"
    let negative = x < 0.0;
    if negative {
        s.push('-');
    }
    for d in &ds[..int_len] {
        s.push((b'0' + d) as char);
    }
    if digits > 0 {
        s.push('.');
        for d in &ds[int_len..] {
            s.push((b'0' + d) as char);
        }
    }
    s
}

/// The parts of a UTC time (JS `new Date(ms).getUTC...()`): year, month 1-12, day 1-31, hours, minutes, seconds, ms.
pub fn utc_parts(ms: i64) -> (i64, u32, u32, u32, u32, u32, u32) {
    let days = ms.div_euclid(86_400_000);
    let rem = ms.rem_euclid(86_400_000);
    // Howard Hinnant's civil_from_days
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    let y = yoe + era * 400 + if m <= 2 { 1 } else { 0 };
    let h = (rem / 3_600_000) as u32;
    let mi = (rem / 60_000 % 60) as u32;
    let s = (rem / 1000 % 60) as u32;
    let msec = (rem % 1000) as u32;
    (y, m, d, h, mi, s, msec)
}

/// JS `new Date(ms).toISOString()`: "2026-10-21T06:55:40.123Z"
pub fn iso_string(ms: i64) -> String {
    let (y, m, d, h, mi, s, msec) = utc_parts(ms);
    if (0..=9999).contains(&y) {
        format!("{y:04}-{m:02}-{d:02}T{h:02}:{mi:02}:{s:02}.{msec:03}Z")
    } else {
        let sign = if y < 0 { '-' } else { '+' };
        format!("{sign}{:06}-{m:02}-{d:02}T{h:02}:{mi:02}:{s:02}.{msec:03}Z", y.abs())
    }
}

/// Node's `Buffer#toString('base64url')` (no padding)
pub fn base64url(b: &[u8]) -> String {
    use base64::Engine;
    base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(b)
}

/// Node's `Buffer.from(s, 'base64url')`: lenient as Node is (padding and characters outside the alphabet are
/// skipped; '+' and '/' read as '-' and '_')
pub fn from_base64url(s: &str) -> Vec<u8> {
    use base64::Engine;
    let clean: String = s
        .chars()
        .filter_map(|c| match c {
            'A'..='Z' | 'a'..='z' | '0'..='9' | '-' | '_' => Some(c),
            '+' => Some('-'),
            '/' => Some('_'),
            _ => None,
        })
        .collect();
    // a last group of one character holds no byte (Node drops it)
    let usable = if clean.len() % 4 == 1 { &clean[..clean.len() - 1] } else { &clean[..] };
    base64::engine::general_purpose::URL_SAFE_NO_PAD
        .decode(usable)
        .or_else(|_| {
            base64::engine::GeneralPurpose::new(
                &base64::alphabet::URL_SAFE,
                base64::engine::GeneralPurposeConfig::new()
                    .with_decode_allow_trailing_bits(true)
                    .with_encode_padding(false)
                    .with_decode_padding_mode(base64::engine::DecodePaddingMode::Indifferent),
            )
            .decode(usable)
        })
        .unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn numbers_as_js_writes_them() {
        for (x, s) in [
            (0.0, "0"),
            (-0.0, "0"),
            (1.0, "1"),
            (-1.5, "-1.5"),
            (0.1, "0.1"),
            (1e21, "1e+21"),
            (1e20, "100000000000000000000"),
            (123456789012345680000.0, "123456789012345680000"),
            (1.5e-7, "1.5e-7"),
            (1e-6, "0.000001"),
            (1e-7, "1e-7"),
            (1729512345123.4567, "1729512345123.4568"),
            (0.30000000000000004, "0.30000000000000004"),
            (5e-324, "5e-324"),
            (1.7976931348623157e308, "1.7976931348623157e+308"),
            (f64::NAN, "NaN"),
            (f64::INFINITY, "Infinity"),
            (2.5e25, "2.5e+25"),
            (100.0, "100"),
            (1234.5678, "1234.5678"),
            // -0.53125762939453125, halfway between two shortest forms: the even one, as JS
            (-69633.0 / 131072.0, "-0.5312576293945312"),
        ] {
            assert_eq!(number_to_string(x), s, "{x}");
        }
    }

    #[test]
    fn to_fixed_rounds_half_up() {
        assert_eq!(to_fixed(0.125, 2), "0.13");
        assert_eq!(to_fixed(1.005, 2), "1.00"); // 1.005 is 1.00499999999999989...
        assert_eq!(to_fixed(61.125, 2), "61.13");
        assert_eq!(to_fixed(9.995, 2), "9.99");
        assert_eq!(to_fixed(99.999, 2), "100.00");
        assert_eq!(to_fixed(-0.0001, 2), "-0.00");
        assert_eq!(to_fixed(-0.0, 2), "0.00");
        assert_eq!(to_fixed(-0.5, 0), "-1");
        assert_eq!(to_fixed(1e20, 2), "100000000000000000000.00");
        assert_eq!(to_fixed(-1.5, 0), "-2");
        assert_eq!(to_fixed(2.5, 0), "3");
        assert_eq!(to_fixed(12.0, 3), "12.000");
        assert_eq!(to_fixed(1.234, 0), "1");
        // more digits than JS allows: 100, not a panic
        assert_eq!(to_fixed(0.5, 5000), to_fixed(0.5, 100));
        assert_eq!(to_fixed(0.5, 100).len(), 102);
    }

    #[test]
    fn printable_counts_utf16() {
        assert_eq!(printable("a\u{1F600}b", 160), "a??b");
        assert_eq!(printable("héllo\n", 4), "h?ll");
        assert_eq!(printable("abc", 2), "ab");
    }

    #[test]
    fn trim_and_space() {
        assert_eq!(trim("\u{FEFF} a b \u{3000}"), "a b");
        assert_eq!(trim("\u{0085}x"), "\u{0085}x");
        assert_eq!(collapse_space("a \t\n b", " "), "a b");
    }

    #[test]
    fn dates() {
        assert_eq!(iso_string(0), "1970-01-01T00:00:00.000Z");
        assert_eq!(iso_string(1_792_565_740_123), "2026-10-21T06:55:40.123Z");
        assert_eq!(utc_parts(951_782_400_000).0, 2000);
        assert_eq!(utc_parts(951_782_400_000).1, 2);
        assert_eq!(utc_parts(951_782_400_000).2, 29);
    }

    #[test]
    fn base64url_both_ways() {
        assert_eq!(base64url(&[0xfb, 0xff]), "-_8");
        assert_eq!(from_base64url("-_8"), vec![0xfb, 0xff]);
        assert_eq!(from_base64url("-_8="), vec![0xfb, 0xff]);
    }
}
