#!/usr/bin/env python3
"""Rough function count: direct call targets + relocated pointers into .text (linear sweep)."""
import sys, struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
for p in sys.argv[1:]:
    pe = pefile.PE(p); IB = pe.OPTIONAL_HEADER.ImageBase
    t = next(s for s in pe.sections if s.Name.startswith(b'.text'))
    lo = IB + t.VirtualAddress; data = t.get_data()[:t.Misc_VirtualSize]; hi = lo + len(data)
    md = Cs(CS_ARCH_X86, CS_MODE_32); md.skipdata = True
    calls = set(); n = 0
    for (a, sz, m, ops) in md.disasm_lite(data, lo):
        n += 1
        if m == 'call' and ops.startswith('0x'):
            v = int(ops, 16)
            if lo <= v < hi: calls.add(v)
    img = pe.get_memory_mapped_image(); ptr = set()
    for b in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', []):
        for e in b.entries:
            if e.type == 3:
                v = struct.unpack_from('<I', img, e.rva)[0]
                if lo <= v < hi and not (lo <= IB + e.rva < hi): ptr.add(v)
    print(f'{p.split("/")[-1]:12s} text={len(data):#x} insns~{n} call-targets={len(calls)} data-ptrs-into-text={len(ptr)} union={len(calls|ptr)}')
