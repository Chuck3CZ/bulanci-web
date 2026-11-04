//! The two clocks of the Node server: `performance.now()` (milliseconds since the start, monotonic, fractional) and
//! `Date.now()` (milliseconds since 1970, whole).

use std::sync::OnceLock;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

static START: OnceLock<Instant> = OnceLock::new();

/// The instant `mono_ms()` counts from (the first call).
pub fn start() -> Instant {
    *START.get_or_init(Instant::now)
}

/// `performance.now()`: milliseconds since the process's first call, monotonic, with fractions.
pub fn mono_ms() -> f64 {
    start().elapsed().as_secs_f64() * 1000.0
}

/// The `mono_ms()` of an instant.
pub fn mono_ms_at(t: Instant) -> f64 {
    t.saturating_duration_since(start()).as_secs_f64() * 1000.0
}

/// `Date.now()`: whole milliseconds since 1970.
pub fn now_ms() -> i64 {
    match SystemTime::now().duration_since(UNIX_EPOCH) {
        Ok(d) => d.as_millis() as i64,
        Err(e) => -(e.duration().as_millis() as i64),
    }
}
