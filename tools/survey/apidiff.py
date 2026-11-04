#!/usr/bin/env python3
"""Union of system imports of the modules to lift vs. API names the nfsu2-web runtime recognises."""
import json, re, glob, sys, collections
d = json.load(open('survey.json'))
MODS = ['war3.exe', 'game.dll', 'Storm.dll', 'Mss32.dll', 'ijl15.dll']
LIFTED = {'storm.dll', 'mss32.dll', 'ijl15.dll', 'game.dll'}
need = collections.defaultdict(lambda: collections.defaultdict(set))
for m in MODS:
    for dll, fs in d[m]['imports'].items():
        if dll.lower() in LIFTED: continue
        for f in fs: need[dll.upper()][f].add(m.split('.')[0])
# extra dynamically resolved
dyn = {'D3D8.DLL': ['Direct3DCreate8'], 'DSOUND.DLL': ['DirectSoundCreate', 'DirectSoundEnumerateA'],
       'MSWSOCK.DLL': ['AcceptEx', 'GetAcceptExSockaddrs'], 'WS2_32.DLL': ['WSAIoctl', 'WSARecv', 'WSASend'],
       'KERNEL32.DLL': ['GetDiskSpaceExA', 'GetSystemDefaultUILanguage', 'TryEnterCriticalSection', 'IsProcessorFeaturePresent'],
       'ADVAPI32.DLL': ['CryptAcquireContextA', 'CryptCreateHash', 'CryptDestroyHash', 'CryptDestroyKey', 'CryptHashData', 'CryptImportKey', 'CryptReleaseContext', 'CryptVerifySignatureA']}
for dll, fs in dyn.items():
    for f in fs: need[dll][f].add('dyn')
src = ''
for p in glob.glob(sys.argv[1] + '/runtime/*.c') + glob.glob(sys.argv[1] + '/runtime/web/*.c'):
    src += open(p, errors='replace').read()
have = set(re.findall(r'"([A-Za-z_][A-Za-z0-9_]{2,})"', src))
tot = [0, 0]
report = {}
for dll in sorted(need):
    fs = need[dll]
    h = sorted(f for f in fs if f in have); m = sorted(f for f in fs if f not in have)
    tot[0] += len(h); tot[1] += len(m)
    report[dll] = dict(have=h, missing=m)
    print(f'{dll:14s} need {len(fs):4d}  have {len(h):4d}  missing {len(m):4d}')
print('TOTAL have', tot[0], 'missing', tot[1])
json.dump(report, open('apidiff.json', 'w'), indent=1)
for dll in sorted(report):
    if report[dll]['missing']: print('\n##', dll, 'missing:', ' '.join(report[dll]['missing']))
