#!/usr/bin/env python3
"""Find string arguments pushed before calls to LoadLibrary*/GetProcAddress/GetModuleHandle* (linear sweep heuristic)."""
import sys, os, collections
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

TARGETS = ('LoadLibraryA','LoadLibraryW','LoadLibraryExA','LoadLibraryExW','GetProcAddress','GetModuleHandleA','GetModuleHandleW')
def run(path):
    pe = pefile.PE(path); IB = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    iat = {}
    for d in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
        for i in d.imports:
            if i.name and i.name.decode() in TARGETS: iat[i.address] = i.name.decode()
    def cstr(va, wide=False):
        o = va - IB
        if not (0 <= o < len(img)): return None
        s = bytearray()
        step = 2 if wide else 1
        while o < len(img) and img[o] and len(s) < 80:
            if wide and img[o+1]: return None
            c = img[o]
            if not (0x20 <= c < 0x7f): return None
            s.append(c); o += step
        return s.decode() if len(s) >= 3 else None
    md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True; md.skipdata = True
    res = collections.defaultdict(set)
    for s in pe.sections:
        if not s.Characteristics & 0x20000000: continue
        base = IB + s.VirtualAddress; data = s.get_data()
        win = collections.deque(maxlen=14); regfn = {}
        for ins in md.disasm(data, base):
            tgt = None
            if ins.mnemonic == 'mov' and len(ins.operands) == 2 and ins.operands[0].type == X86_OP_REG and ins.operands[1].type == X86_OP_MEM \
               and not ins.operands[1].mem.base and not ins.operands[1].mem.index:
                a = ins.operands[1].mem.disp & 0xffffffff
                if a in iat: regfn[ins.operands[0].reg] = iat[a]
                else: regfn.pop(ins.operands[0].reg, None)
            if ins.mnemonic == 'call' and ins.operands:
                o = ins.operands[0]
                if o.type == X86_OP_MEM and not o.mem.base and not o.mem.index: tgt = iat.get(o.mem.disp & 0xffffffff)
                elif o.type == X86_OP_REG: tgt = regfn.get(o.reg)
            if tgt:
                wide = tgt.endswith('W')
                # first push before the call = last arg pushed = arg1 ... GetProcAddress(h, name): name pushed first (furthest)
                strs = [cstr(p.operands[0].imm & 0xffffffff, wide) for p in win if p.mnemonic == 'push' and p.operands and p.operands[0].type == X86_OP_IMM]
                # also mov reg/[esp], imm patterns
                strs += [cstr(p.operands[1].imm & 0xffffffff, wide) for p in win if p.mnemonic in ('mov','lea') and len(p.operands) == 2 and p.operands[1].type == X86_OP_IMM]
                strs = [x for x in strs if x]
                res[tgt].add(strs[-1] if strs else '?(dynamic)')
                if tgt == 'GetProcAddress':
                    for x in strs: res['GetProcAddress*'].add(x)
            win.append(ins)
            if ins.mnemonic in ('ret','jmp'): regfn.clear() if ins.mnemonic=='ret' else None
    return res

for p in sys.argv[1:]:
    r = run(p)
    print('==', os.path.basename(p))
    for k in TARGETS:
        if k in r: print(f'  {k:18s}', sorted(r[k]))
