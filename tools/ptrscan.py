#!/usr/bin/env python3
"""Pseudo-relocations for an image without a .reloc table (war3.exe).

  ptrscan.py <image> <outdir> [<genout> <prefix>]

genall.py and argcount.py find the code that only pointers reach (vtables,
window procedures, thread entries, callbacks) through RELOCS, the list of
fixup slots.  war3.exe has no base relocations (it always loads at
0x400000), so this rebuilds an over-approximation of them:

  * every 4-aligned dword of the non-code sections whose value lies in the
    image (the linker aligns pointer data), and
  * every immediate or displacement field of an instruction in .text (linear
    sweep) whose value lies in the image (push offset fn, mov [x], offset fn).

A slot only adds function entries when its value points into .text, so a
false hit costs at most an extra lifted function, never a wrong one.
Outputs <outdir>/relocs.txt (slot VAs) and <outdir>/seeds.txt (entry point
and exports), the same files prepare_module.py writes for a DLL, and with
<genout> the module's identity <genout>/modinfo.c (g_<prefix>modinfo).
"""
import hashlib, os, struct, sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM, X86_OP_MEM


def main(path, outdir, genout=None, prefix=''):
    raw = open(path, 'rb').read()
    pe = pefile.PE(data=raw)
    ib = pe.OPTIONAL_HEADER.ImageBase
    lo, hi = ib, ib + pe.OPTIONAL_HEADER.SizeOfImage
    slots = set()
    md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True; md.skipdata = True
    tlo = thi = 0
    code = []                                       # (va, bytes) of the code sections
    tables = set()                                  # 'jmp [r*4 + table]' tables
    for s in pe.sections:
        va = ib + s.VirtualAddress
        data = s.get_data()[:s.Misc_VirtualSize]
        if s.Characteristics & 0x20000000:          # code
            if s.Name.rstrip(b'\0') == b'.text': tlo, thi = va, va + len(data)
            code.append((va, data))
            for ins in md.disasm(data, va):
                if ins.id == 0: continue            # skipdata
                if ins.mnemonic == 'jmp' and ins.operands and ins.operands[0].type == X86_OP_MEM:
                    m = ins.operands[0].mem
                    if not m.base and m.index and m.scale == 4: tables.add(m.disp & 0xffffffff)
                for op in ins.operands:
                    if op.type == X86_OP_IMM and ins.imm_offset and ins.imm_size == 4:
                        v = op.imm & 0xffffffff
                        if lo <= v < hi: slots.add(ins.address + ins.imm_offset)
                    elif op.type == X86_OP_MEM and ins.disp_offset and ins.disp_size == 4:
                        v = op.mem.disp & 0xffffffff
                        if lo <= v < hi: slots.add(ins.address + ins.disp_offset)
        else:
            for o in range(0, len(data) - 3, 4):
                v = struct.unpack_from('<I', data, o)[0]
                if lo <= v < hi: slots.add(va + o)
    # a jump table in the code (MSVC's CRT _output: "cmp eax, ecx; ja" the
    # lifter's JTSWITCH does not take): its entries as slots, so the targets
    # are lifted as entries the dispatcher finds; it stops at the first dword
    # that is not a code address (the table's end)
    # ... and the same pattern found in the raw bytes at every offset (FF 24 <SIB scale 4, no base> disp32):
    # a linear sweep that ran out of step on data inside a function (the CRT's memmove keeps its tables
    # right after its code) misses the jmp itself
    for va, data in code:
        i = data.find(bytes((0xFF, 0x24)))
        while i >= 0 and i + 7 <= len(data):
            if (data[i + 2] & 0xC7) == 0x85:
                tables.add(struct.unpack_from('<I', data, i + 3)[0])
            i = data.find(bytes((0xFF, 0x24)), i + 1)
    for t in tables:
        for va, data in code:
            if not va <= t < va + len(data): continue
            for k in range(1024):
                o = t - va + 4 * k
                if o + 4 > len(data): break
                v = struct.unpack_from('<I', data, o)[0]
                if not any(cv <= v < cv + len(cd) for cv, cd in code):
                    if k == 0: continue          # entry 0 unused (memmove: it overlaps the jmp's own bytes)
                    break
                slots.add(t + 4 * k)
    seeds = {ib + pe.OPTIONAL_HEADER.AddressOfEntryPoint}
    if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
        for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            v = ib + e.address
            if tlo <= v < thi: seeds.add(v)
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, 'relocs.txt'), 'w') as f:
        f.write('\n'.join(f'{a:08x}' for a in sorted(slots)))
    with open(os.path.join(outdir, 'seeds.txt'), 'w') as f:
        f.write('\n'.join(f'{a:08x}' for a in sorted(seeds)))
    if genout:
        os.makedirs(genout, exist_ok=True)
        sz = pe.OPTIONAL_HEADER.SizeOfImage
        with open(os.path.join(genout, 'modinfo.c'), 'w') as f:
            f.write('/* generated by tools/ptrscan.py: identity of the lifted module */\n#include "module.h"\n')
            f.write(f'const ModInfo g_{prefix}modinfo = {{\n')
            f.write(f'  "{os.path.basename(path).lower()}", "{hashlib.sha256(raw).hexdigest()}",\n')
            f.write(f'  0x{ib:08x}u, 0x{ib:08x}u, 0x{sz:08x}u, 0x{ib + pe.OPTIONAL_HEADER.AddressOfEntryPoint:08x}u,\n')
            f.write(f'  0x{tlo:08x}u, 0x{thi - tlo:08x}u, {len(raw)}u,\n}};\n')
    print(f"ptrscan: {path}: {len(slots)} pointer slots, {len(seeds)} seeds (entry + exports in .text)")


if __name__ == '__main__':
    if len(sys.argv) not in (3, 5): sys.exit(__doc__)
    main(*sys.argv[1:])
