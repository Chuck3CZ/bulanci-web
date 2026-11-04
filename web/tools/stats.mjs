// Small statistics for the measurement tools (bench.mjs).
// pct: the p-quantile by linear interpolation between the closest ranks
// (numpy's default): the median of an even count is the mean of the two
// middle values, not the upper one (the perf review: with 2 runs the
// "median" was the maximum).
export const pct = (a, p) => {
  if (!a.length) return NaN;
  const s = a.slice().sort((x, y) => x - y), i = Math.min(Math.max(p, 0), 1) * (s.length - 1), lo = Math.floor(i), hi = Math.ceil(i);
  return lo === hi ? s[lo] : s[lo] + (s[hi] - s[lo]) * (i - lo);
};
export const dist = a => a.length ? { n: a.length, mean: a.reduce((x, y) => x + y, 0) / a.length, p50: pct(a, 0.5), p95: pct(a, 0.95), p99: pct(a, 0.99), max: Math.max(...a) } : { n: 0 };
