import struct

d = open('out/romfs_image.bin', 'rb').read()
BLOB = 0x1000
DIR_META = BLOB + 0x4C
DIR_HASH = BLOB + 0x28
DIR_BUCKETS = 0x24 // 4


def ror(h, n):
    return ((h >> n) | (h << (32 - n))) & 0xFFFFFFFF


def rhash(name, seed):
    h = seed ^ 0x75BCD15
    for ch in name:
        h = (ord(ch) ^ ror(h, 5)) & 0xFFFFFFFF
    return h


def u32(off):
    return struct.unpack_from('<I', d, off)[0]


def dir_rec(off):
    base = DIR_META + off
    nlen = u32(base + 0x14)
    nm = d[base + 0x18:base + 0x18 + nlen].decode('utf-16-le', 'replace')
    return dict(off=off, parent=u32(base), sibling=u32(base + 4),
                childdir=u32(base + 8), childfile=u32(base + 0xC),
                hnext=u32(base + 0x10), name=nm, nlen=nlen)


def dir_lookup(parent_off, name):
    b = rhash(name, parent_off) % DIR_BUCKETS
    c = u32(DIR_HASH + b * 4)
    steps = 0
    while c != 0xFFFFFFFF and steps < 64:
        r = dir_rec(c)
        if r['name'] == name:
            return r
        c = r['hnext']
        steps += 1
    return None


print('root', dir_rec(0))
p = 0
for part in ('lt6', 'arc'):
    r = dir_lookup(p, part)
    print('dir', part, '->', r)
    if r is None:
        break
    p = r['off']
