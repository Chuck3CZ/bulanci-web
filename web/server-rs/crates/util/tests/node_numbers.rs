//! `js::number_to_string` against node's `String(x)` (skipped with a message without node): every kind of double,
//! and many with few significant bits (float32 values, dyadic fractions), where two shortest forms can tie.

use nfsu2_util::js::number_to_string;

fn numbers() -> Vec<f64> {
    let mut x: u64 = 0x9E37_79B9_7F4A_7C15;
    let mut next = move || {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        x
    };
    let mut out = vec![0.0, -0.0, 1.0, 0.1, 0.5, 1e21, 1e-7, 5e-324, f64::MAX, f64::MIN_POSITIVE, -69633.0 / 131072.0];
    for _ in 0..200_000 {
        // a float32's value (the game's times and positions)
        let f = f32::from_bits(next() as u32);
        if f.is_finite() {
            out.push(f as f64);
        }
    }
    for _ in 0..100_000 {
        // m / 2^q with few bits in m: the ties' home
        let m = next() >> (64 - 1 - (next() % 40)) as u32;
        let q = (next() % 30) as i32;
        out.push(m as f64 / 2f64.powi(q));
        out.push(-(m as f64) * 2f64.powi(q));
    }
    for _ in 0..100_000 {
        // any double
        let d = f64::from_bits(next());
        if d.is_finite() {
            out.push(d);
        }
    }
    out
}

#[test]
fn numbers_as_node_writes_them() {
    if std::process::Command::new("node").arg("--version").output().is_err() {
        println!("skipped: no node");
        return;
    }
    let xs = numbers();
    let dir = std::env::temp_dir().join(format!("nfsu2-util-numbers-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    // as bits, so nothing is lost on the way
    let input = dir.join("bits.txt");
    std::fs::write(&input, xs.iter().map(|x| format!("{:016x}\n", x.to_bits())).collect::<String>()).unwrap();
    let script = "const fs = require('fs'); const b = Buffer.alloc(8); const out = [];\n\
        for (const l of fs.readFileSync(process.argv[1], 'utf8').split('\\n')) { if (!l) continue; b.writeBigUInt64BE(BigInt('0x' + l)); out.push(String(b.readDoubleBE(0))); }\n\
        process.stdout.write(out.join('\\n'));";
    let out = std::process::Command::new("node").args(["-e", script, input.to_str().unwrap()]).output().expect("node");
    std::fs::remove_dir_all(&dir).ok();
    assert!(out.status.success(), "{}", String::from_utf8_lossy(&out.stderr));
    let js = String::from_utf8(out.stdout).unwrap();
    let js: Vec<&str> = js.split('\n').collect();
    assert_eq!(js.len(), xs.len());
    let bad: Vec<String> = xs
        .iter()
        .zip(&js)
        .filter(|(x, s)| number_to_string(**x) != **s)
        .map(|(x, s)| format!("{:e}: rust {}, js {s}", x, number_to_string(*x)))
        .collect();
    println!("number_to_string: {} numbers, {} differ", xs.len(), bad.len());
    assert!(bad.is_empty(), "{} differ, e.g.\n{}", bad.len(), bad[..bad.len().min(20)].join("\n"));
}
