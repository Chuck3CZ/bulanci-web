#!/usr/bin/env python3
"""Win32 stdcall argument counts from Wine's .spec files.

  winapi_args.py <dir with <dll>.spec files> > tools/winapi_nargs.txt

The HLE pops a stdcall import's arguments itself (win32.c: esp += 4*nargs),
so a wrong count unbalances the guest stack: LoadImageA counted as 3 instead
of 6 (tools/argcount.py's guess from the push sequences) cost WC3 its
CGxDevice pointer.  Wine's specs list each export's parameters (long ptr str
wstr handle segptr float: one stack slot; int64 double: two; varargs: cdecl,
the caller pops: 0).  Output: "<dll> <name> <nargs>" per line, sorted.
The specs come from https://github.com/wine-mirror/wine (dlls/<dll>/<dll>.spec).
"""
import os, re, sys

SLOTS = {'long': 1, 'ptr': 1, 'str': 1, 'wstr': 1, 'handle': 1, 'segptr': 1, 'float': 1, 'segstr': 1,
         'int64': 2, 'double': 2, 'int128': 4}
LINE = re.compile(r'^\s*(?:\d+|@)\s+(stdcall|cdecl|varargs|thiscall|fastcall)\s+((?:-\S+\s+)*)(\S+?)\s*\(([^)]*)\)')

def main(d):
    out = {}
    for fn in sorted(os.listdir(d)):
        if not fn.endswith('.spec'): continue
        dll = fn[:-5].lower() + '.dll'
        for ln in open(os.path.join(d, fn), errors='replace'):
            m = LINE.match(ln)
            if not m: continue
            conv, flags, name, params = m.groups()
            if '-arch=' in flags and not re.search(r'-arch=(?:\S*,)?(?:win32|i386)', flags): continue
            name = name.split('@')[0]
            if conv in ('varargs', 'cdecl'): n = 0
            elif conv == 'stdcall':
                n = 0
                for p in params.split():
                    if p not in SLOTS: n = None; break
                    n += SLOTS[p]
                if n is None: continue
            else: continue
            out[(dll, name)] = n
    for (dll, name), n in sorted(out.items()):
        print(f'{dll} {name} {n}')
    print(f'winapi_args: {len(out)} functions from {d}', file=sys.stderr)

if __name__ == '__main__':
    if len(sys.argv) != 2: sys.exit(__doc__)
    main(sys.argv[1])
