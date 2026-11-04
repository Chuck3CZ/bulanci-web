//! `JSON.stringify` of a `serde_json::Value`, byte for byte: keys in insertion order (serde_json's preserve_order),
//! strings escaped as JS escapes them (the same set serde_json escapes), numbers as JS writes them (serde_json
//! writes the double 5 as "5.0" and 1e21 as "1e21"; JS "5" and "1e+21").  Integers are integers already.

use serde_json::Value;

use crate::js::number_to_string;

/// `JSON.stringify(v)` (no spaces)
pub fn stringify(v: &Value) -> String {
    let mut out = String::with_capacity(64);
    write_value(&mut out, v);
    out
}

fn write_value(out: &mut String, v: &Value) {
    match v {
        Value::Null => out.push_str("null"),
        Value::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Value::Number(n) => {
            if let Some(i) = n.as_i64() {
                out.push_str(itoa_i64(i).as_str());
            } else if let Some(u) = n.as_u64() {
                out.push_str(&u.to_string());
            } else {
                let f = n.as_f64().unwrap_or(f64::NAN);
                // JSON.stringify writes NaN and the infinities as null
                if f.is_finite() { out.push_str(&number_to_string(f)) } else { out.push_str("null") }
            }
        }
        Value::String(s) => write_str(out, s),
        Value::Array(a) => {
            out.push('[');
            for (i, x) in a.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                write_value(out, x);
            }
            out.push(']');
        }
        Value::Object(m) => {
            out.push('{');
            for (i, (k, x)) in m.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                write_str(out, k);
                out.push(':');
                write_value(out, x);
            }
            out.push('}');
        }
    }
}

fn itoa_i64(i: i64) -> String {
    i.to_string()
}

/// a JSON string as `JSON.stringify` writes it: `"` and `\` escaped, \b \f \n \r \t, other controls as \u00XX
pub fn write_str(out: &mut String, s: &str) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\u{08}' => out.push_str("\\b"),
            '\u{0C}' => out.push_str("\\f"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {
                out.push_str(&format!("\\u{:04x}", c as u32));
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

/// A JSON number that JS would read from `x` (whole numbers as integers, so `stringify` writes "5", not "5.0").
pub fn num(x: f64) -> Value {
    if x.fract() == 0.0 && x.abs() < 9_007_199_254_740_992.0 {
        Value::from(x as i64)
    } else {
        serde_json::Number::from_f64(x).map(Value::Number).unwrap_or(Value::Null)
    }
}

/// JS `Number.isSafeInteger(v)` for a parsed JSON value (1.0 counts: JS reads it as 1)
pub fn safe_integer(v: &Value) -> Option<i64> {
    let f = v.as_f64()?;
    if f.fract() == 0.0 && f.abs() <= 9_007_199_254_740_991.0 { Some(f as i64) } else { None }
}

/// `v === n` in JS for a parsed JSON value and a small integer (1 and 1.0 are both 1)
pub fn is_num(v: Option<&Value>, n: f64) -> bool {
    v.and_then(Value::as_f64).is_some_and(|f| f == n)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn as_js_writes_it() {
        let v = json!({ "b": 5.0, "a": [1e21, 0.5, -0.0, "x\"\n\u{1}é"], "c": null, "d": true });
        assert_eq!(stringify(&v), r#"{"b":5.0,"a":[1e+21,0.5,0,"x\"\n\u0001é"],"c":null,"d":true}"#.replace("5.0", "5"));
        assert_eq!(stringify(&num(5.0)), "5");
        assert_eq!(safe_integer(&json!(3.0)), Some(3));
        assert_eq!(safe_integer(&json!(3.5)), None);
        assert!(is_num(Some(&json!(1.0)), 1.0));
    }
}
