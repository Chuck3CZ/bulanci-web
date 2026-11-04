#!/usr/bin/env python3
"""Pick the functions to inline (genall.py INLINE_LIST) from a call-count profile.

A race frame makes ~94,000 guest function calls; each costs a wasm call
(prologue, stack check, return) and the register stores and reloads around it
(LREGS).  Inlining removes that, but every call site gets its own copy, so the
pick is the most calls per byte of copies: a function qualifies when it is
called often (>= --min calls in the profile window), is at most --max x86
instructions, has direct call sites (indirect calls cannot be inlined) and
nothing genall.py refuses (tail jumps, run-time helpers, recursion,
overrides); then the best calls / (call sites x body size) go in until the
copies reach --budget bytes of C.  Measured in a race (web/tools/bench.mjs's
lockstep recipe): 1 MB (110 functions, 37 % of the calls) and 3 MB do the
same, 6 MB costs 3 % more cycles than 3 MB (more code to run through).

The profile: a COVERAGE=1 lift (COVERAGE=1 web/gen.sh), run through
tests/web/scripts/perf_cov.txt (covreset at the start of its window) with
COVREPORT=/save/cov.txt COVTOP=2000 in the environment, e.g.
  web/tools/bench.mjs --fps 120 --runs 1 --script /tests/web/scripts/perf_cov.txt
    --env COVREPORT=/save/cov.txt --env COVTOP=2000 (+ the recipe's lockstep env)
The exit report lists the most called functions ("<count>  sub_<addr>"
lines); this reads them from the run's log.

    tools/inline_pick.py <log> <gen/all> [--min N] [--max N] [--budget BYTES] > tools/inline_hot.txt
"""
import re, sys, os, glob, argparse, collections

ap = argparse.ArgumentParser()
ap.add_argument('log'); ap.add_argument('gen')
ap.add_argument('--min', type=int, default=30000, help='calls in the profile window (default 30000)')
ap.add_argument('--max', type=int, default=400, help='x86 instructions (default 400)')
ap.add_argument('--budget', type=int, default=1 << 20, help='bytes of C for the copies (default 1 MB)')
o = ap.parse_args()

log = open(o.log, errors='replace').read()
counts = {int(m.group(2), 16): int(m.group(1)) for m in re.finditer(r'^(?:\[\d+!\])?\s+(\d+)\s+sub_([0-9a-f]{8})\s*$', log, re.M)}
total = re.search(r'coverage: (\d+) calls in all', log)
if not counts: sys.exit('no "<count>  sub_<addr>" lines in the log (COVREPORT + COVTOP)')

hdr = re.compile(r'^/\* ===== sub_([0-9a-f]{8})  \((\d+) insns, (\d+) blocks\) ===== \*/')
fdef = re.compile(r'^void sub_([0-9a-f]{8})(_orig)?\(CPU')
deny = ('DISPATCH_TAIL', 'unsupported_insn', 'cpuid_shim', 'guest_hook', 'rdtsc_shim')
info, sites = {}, collections.Counter()
for f in sorted(glob.glob(os.path.join(o.gen, 'shard_*.c'))):
    text = open(f).read()
    for m in re.finditer(r'\bsub_([0-9a-f]{8})\(c\);', text): sites[int(m.group(1), 16)] += 1
    cur = None
    for part in re.split(r'(?m)^(?=/\* ===== sub_)', text):
        m = hdr.match(part)
        if not m: continue
        a = int(m.group(1), 16); d = fdef.search(part.split('\n', 2)[1] if '\n' in part else '')
        body = part.split('{', 1)[1] if '{' in part else ''
        info[a] = dict(n=int(m.group(2)), size=len(part), orig=bool(d and d.group(2)),
                       ok=not any(x in body for x in deny) and f'sub_{a:08x}(c)' not in body)

cand = []
for a, n in counts.items():
    i = info.get(a)
    if not i or not i['ok'] or i['orig'] or n < o.min or i['n'] > o.max or not sites[a]: continue
    cand.append((n / (sites[a] * i['size']), a, n, sites[a], i['n'], sites[a] * i['size']))
cand.sort(reverse=True)
pick, used, calls = [], 0, 0
for _, a, n, s, ins, g in cand:
    if used + g > o.budget: continue
    pick.append((a, n, s, ins)); used += g; calls += n
pick.sort()
tot = int(total.group(1)) if total else sum(counts.values())
print(f"# genall.py INLINE_LIST (web/gen.sh): {len(pick)} functions inlined at their direct call sites,")
print(f"# picked by tools/inline_pick.py from a race's call counts: {calls} of {tot} calls ({100 * calls / tot:.1f} %)")
print(f"# in the profile window, {used / 1048576:.1f} MB of C in copies.  address  calls  call sites  x86 instructions")
for a, n, s, ins in pick: print(f"{a:08x}  # {n} {s} {ins}")
print(f"inline_pick: {len(pick)} functions, {calls} of {tot} calls ({100 * calls / tot:.1f} %), {used / 1048576:.1f} MB of copies", file=sys.stderr)
