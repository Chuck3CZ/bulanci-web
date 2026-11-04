//! Randomness from the OS: Node's `crypto.randomBytes(n)` and `crypto.randomInt(min, max)`.

/// n random bytes
pub fn bytes(n: usize) -> Vec<u8> {
    let mut b = vec![0u8; n];
    fill(&mut b);
    b
}

/// fills `b` with random bytes
pub fn fill(b: &mut [u8]) {
    getrandom::fill(b).expect("the OS's random numbers");
}

/// a random u32
pub fn u32() -> u32 {
    let mut b = [0u8; 4];
    fill(&mut b);
    u32::from_le_bytes(b)
}

/// a random u64
pub fn u64() -> u64 {
    let mut b = [0u8; 8];
    fill(&mut b);
    u64::from_le_bytes(b)
}

/// `crypto.randomInt(min, max)`: uniform in min..max (max exclusive), no modulo bias
pub fn int(min: i64, max: i64) -> i64 {
    assert!(max > min, "randomInt: max must be above min");
    let range = (max - min) as u64;
    let zone = u64::MAX - (u64::MAX % range);
    loop {
        let v = u64();
        if v < zone {
            return min + (v % range) as i64;
        }
    }
}

/// `Math.random()`: uniform in [0, 1)
pub fn unit() -> f64 {
    (u64() >> 11) as f64 / (1u64 << 53) as f64
}

/// lowercase hex of n random bytes (`crypto.randomBytes(n).toString('hex')`)
pub fn hex(n: usize) -> String {
    bytes(n).iter().map(|b| format!("{b:02x}")).collect()
}
