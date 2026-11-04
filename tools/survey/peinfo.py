#!/usr/bin/env python3
"""Survey PE modules: hash, version, base, size, relocs, imports, exports, dynamic-load strings."""
import sys, os, hashlib, json, re, collections
import pefile

def ver(pe):
    try:
        for fi in pe.FileInfo:
            for e in fi:
                if hasattr(e, 'StringTable'):
                    for st in e.StringTable:
                        d = {k.decode(): v.decode(errors='replace') for k, v in st.entries.items()}
                        return d.get('FileVersion', '') + ' / ' + d.get('ProductVersion', '')
    except Exception: pass
    if hasattr(pe, 'VS_FIXEDFILEINFO'):
        f = pe.VS_FIXEDFILEINFO[0]
        return f"{f.FileVersionMS>>16}.{f.FileVersionMS&0xffff}.{f.FileVersionLS>>16}.{f.FileVersionLS&0xffff}"
    return ''

DLLRX = re.compile(rb'[A-Za-z0-9_ .\-]{2,40}\.(?:dll|DLL|Dll|asi|ASI|flt|FLT|m3d|M3D|ax|drv|exe|mix)\x00')
def strings_ascii(data, n=4):
    return [m.group().decode() for m in re.finditer(rb'[\x20-\x7e]{%d,}' % n, data)]

out = {}
root = sys.argv[1]
for rel in sys.argv[2:]:
    p = os.path.join(root, rel)
    raw = open(p, 'rb').read()
    pe = pefile.PE(data=raw)
    oh = pe.OPTIONAL_HEADER
    relocs = collections.Counter()
    if hasattr(pe, 'DIRECTORY_ENTRY_BASERELOC'):
        for b in pe.DIRECTORY_ENTRY_BASERELOC:
            for e in b.entries: relocs[e.type] += 1
    imps = {}
    if hasattr(pe, 'DIRECTORY_ENTRY_IMPORT'):
        for d in pe.DIRECTORY_ENTRY_IMPORT:
            imps[d.dll.decode()] = [(i.name.decode() if i.name else f'#{i.ordinal}') for i in d.imports]
    dimps = {}
    if hasattr(pe, 'DIRECTORY_ENTRY_DELAY_IMPORT'):
        for d in pe.DIRECTORY_ENTRY_DELAY_IMPORT:
            dimps[d.dll.decode()] = [(i.name.decode() if i.name else f'#{i.ordinal}') for i in d.imports]
    exps = []
    if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
        for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            exps.append(((e.name or b'').decode(), e.ordinal))
    secs = [(s.Name.rstrip(b'\0').decode(errors='replace'), hex(s.VirtualAddress), hex(s.Misc_VirtualSize), hex(s.SizeOfRawData), hex(s.Characteristics)) for s in pe.sections]
    dll_strs = sorted({m.group()[:-1].decode() for m in DLLRX.finditer(raw)})
    out[rel] = dict(size=len(raw), sha256=hashlib.sha256(raw).hexdigest(), version=ver(pe),
        machine=hex(pe.FILE_HEADER.Machine), dll=bool(pe.FILE_HEADER.Characteristics & 0x2000),
        base=hex(oh.ImageBase), sizeofimage=hex(oh.SizeOfImage), entry=hex(oh.AddressOfEntryPoint),
        subsystem=oh.Subsystem, dllchar=hex(oh.DllCharacteristics), timestamp=pe.FILE_HEADER.TimeDateStamp,
        relocs={str(k): v for k, v in relocs.items()}, sections=secs, imports=imps, delay_imports=dimps,
        nexports=len(exps), exports=exps, dll_strings=dll_strs,
        tls=hasattr(pe, 'DIRECTORY_ENTRY_TLS'))
json.dump(out, sys.stdout, indent=1)
