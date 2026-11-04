#!/usr/bin/env python3
"""Which kept runtime files use global functions/variables defined only in files the import left out."""
import re, os, sys, glob, collections
src, dst = sys.argv[1], sys.argv[2]
def files(root):
    return {os.path.relpath(p, root) for p in glob.glob(root + '/runtime/**/*.[chm]', recursive=True)}
kept = files(dst); dropped = files(src) - kept
DEF = re.compile(r'^(?!static\b)(?:[A-Za-z_][\w \*]*?)\b([A-Za-z_]\w*)\s*\([^;{]*\)\s*\{', re.M)
VAR = re.compile(r'^(?!static\b|extern\b|typedef\b|#)(?:const\s+)?[A-Za-z_]\w*[\s\*]+([a-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=[^;]*)?;', re.M)
syms = {}
for f in dropped:
    t = open(os.path.join(src, f), errors='replace').read()
    for m in DEF.finditer(t): syms.setdefault(m.group(1), f)
    for m in VAR.finditer(t): syms.setdefault(m.group(1), f)
keptdefs = set()
for f in kept:
    t = open(os.path.join(dst, f), errors='replace').read()
    keptdefs |= set(m.group(1) for m in DEF.finditer(t))
syms = {k: v for k, v in syms.items() if k not in keptdefs and k not in ('main',) and len(k) > 3}
use = collections.defaultdict(lambda: collections.Counter())
for f in sorted(kept):
    t = open(os.path.join(dst, f), errors='replace').read()
    for w in set(re.findall(r'\b[A-Za-z_]\w{3,}\b', t)):
        if w in syms: use[f][os.path.basename(syms[w])] += 1
for f in sorted(use, key=lambda f: -sum(use[f].values())):
    print(f'{f:28s} {sum(use[f].values()):4d}  ' + ', '.join(f'{k}:{v}' for k, v in use[f].most_common()))
