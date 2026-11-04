#!/usr/bin/env python3
"""Manifest of a directory tree, to prove a game folder was not touched.

  tools/manifest.py <dir> <out.tsv>          write the manifest
  tools/manifest.py --diff <a.tsv> <b.tsv>   compare two; exit 1 if they differ

One line per entry (files, directories, symlinks), sorted by relative path:
type, size, mtime and ctime (ns), mode, extended-attribute names, SHA-256
(files).  ctime moves on any metadata change (chmod, xattr, rename), mtime on
any content change, and a directory's mtime when an entry is added or
removed in it, so writes, creations, deletions and attribute changes all
show up.  Access times are left out: reading a file (this tool included) may
update them.  Only reads the tree.
"""
import ctypes, hashlib, os, stat, sys

_libc = ctypes.CDLL(None, use_errno=True)
_libc.listxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]
_libc.listxattr.restype = ctypes.c_ssize_t


def xattr_names(path):
    """Extended-attribute names (macOS listxattr, XATTR_NOFOLLOW)."""
    p = os.fsencode(path)
    n = _libc.listxattr(p, None, 0, 1)
    if n <= 0:
        return [] if n == 0 else ['?']
    buf = ctypes.create_string_buffer(n)
    n = _libc.listxattr(p, buf, n, 1)
    return [x.decode('utf-8', 'replace') for x in buf.raw[:max(n, 0)].split(b'\0') if x]


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def entry(root, p):
    st = os.lstat(p)
    if stat.S_ISDIR(st.st_mode):
        kind, size, digest = 'd', 0, '-'
    elif stat.S_ISLNK(st.st_mode):
        kind, size, digest = 'l', st.st_size, os.readlink(p)
    else:
        kind, size, digest = 'f', st.st_size, sha256(p)
    xa = ','.join(sorted(xattr_names(p))) or '-'
    return (os.path.relpath(p, root), kind, size, st.st_mtime_ns, st.st_ctime_ns,
            '%o' % st.st_mode, xa, digest)


def entries(root):
    yield entry(root, root)
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(dirnames + filenames):
            yield entry(root, os.path.join(dirpath, name))


def write(root, out):
    n = total = 0
    with open(out, 'w') as f:
        f.write('# manifest of %s\n# path\ttype\tsize\tmtime_ns\tctime_ns\tmode\txattrs\tsha256\n' % os.path.abspath(root))
        for e in entries(root):
            f.write('\t'.join(str(x) for x in e) + '\n')
            n += 1
            total += e[2] if e[1] == 'f' else 0
    print('manifest: %d entries, %.2f GB of files -> %s' % (n, total / 1e9, out))


def load(path):
    rows = {}
    with open(path) as f:
        for line in f:
            if line.startswith('#'):
                continue
            c = line.rstrip('\n').split('\t')
            rows[c[0]] = c[1:]
    return rows


def diff(a, b):
    ra, rb = load(a), load(b)
    cols = ['type', 'size', 'mtime_ns', 'ctime_ns', 'mode', 'xattrs', 'sha256']
    changes = []
    for p in sorted(set(ra) | set(rb)):
        if p not in rb:
            changes.append('removed  %s' % p)
        elif p not in ra:
            changes.append('added    %s' % p)
        elif ra[p] != rb[p]:
            what = [cols[i] for i in range(len(cols)) if ra[p][i] != rb[p][i]]
            changes.append('changed  %s  (%s)' % (p, ', '.join(what)))
    print('manifest diff: %d entries before, %d after, %d differences' % (len(ra), len(rb), len(changes)))
    for c in changes[:200]:
        print('  ' + c)
    return 1 if changes else 0


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == '--diff':
        sys.exit(diff(sys.argv[2], sys.argv[3]))
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    write(sys.argv[1], sys.argv[2])
