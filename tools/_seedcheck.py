import struct

d = open('out/romfs_image.bin', 'rb').read()
BLOB = 0x1000
FILE_META = BLOB + 0xAF4
FILE_HASH = BLOB + 0x160
FILE_BUCKETS = 0x994 // 4


def ror(h, n):
    return ((h >> n) | (h << (32 - n))) & 0xFFFFFFFF


def rhash(name, seed):
    h = seed ^ 0x75BCD15
    for ch in name:
        h = (ord(ch) ^ ror(h, 5)) & 0xFFFFFFFF
    return h


def u32(off):
    return struct.unpack_from('<I', d, off)[0]


def frec(off):
    base = FILE_META + off
    nlen = u32(base + 0x1C)
    nm = d[base + 0x20:base + 0x20 + nlen].decode('utf-16-le', 'replace')
    return dict(off=off, parent=u32(base), sibling=u32(base + 4),
                doff=u32(base + 8), dsize=u32(base + 0x10),
                hnext=u32(base + 0x18), name=nm)


def lookup(seed, name):
    b = rhash(name, seed) % FILE_BUCKETS
    c = u32(FILE_HASH + b * 4)
    for _ in range(700):
        if c == 0xFFFFFFFF:
            return None
        r = frec(c)
        if r['name'] == name:
            return r
        c = r['hnext']
    return None


for seed in (0, 24, 56):
    for name in ('lt6_a.fa', 'lt6_b.fa', 'lt6_uk.fa', 'product.bcsar'):
        r = lookup(seed, name)
        print('seed', seed, name, r['off'] if r else None)
