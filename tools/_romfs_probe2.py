import struct

BLOB = 0x1000
d = open('out/romfs_image.bin', 'rb').read(BLOB + 0xDD80)[BLOB:]

DIR_META = 0x4C
FILE_HASH = 0x160
FILE_META = 0xAF4
FILE_HASH_COUNT = 0x994 // 4


def u32(off):
    return struct.unpack_from('<I', d, off)[0]


def u64(off):
    return struct.unpack_from('<Q', d, off)[0]


def name16(off, maxlen=200):
    out = []
    for i in range(maxlen):
        c = struct.unpack_from('<H', d, off + i * 2)[0]
        if c == 0:
            break
        out.append(chr(c))
    return ''.join(out)


def dir_rec(off):
    base = DIR_META + off
    parent, nxt, cdir, cfile, x, nlen = struct.unpack_from('<IIIIII', d, base)
    return dict(off=off, parent=parent, next=nxt, cdir=cdir, cfile=cfile,
                x=x, nlen=nlen, name=name16(base + 0x18))


def file_rec(off):
    base = FILE_META + off
    parent, nxt = struct.unpack_from('<II', d, base)
    doff = u64(base + 8)
    dsz = u64(base + 0x10)
    x, nlen = struct.unpack_from('<II', d, base + 0x18)
    return dict(off=off, parent=parent, next=nxt, doff=doff, dsz=dsz,
                x=x, nlen=nlen, name=name16(base + 0x1C))


def rhash(name, seed):
    h = seed ^ 0x75BCD15
    for ch in name:
        h = (ord(ch) ^ (((h >> 5) | (h << 27)) & 0xFFFFFFFF)) & 0xFFFFFFFF
    return h


# --- walk dirs recursively ---
print('--- dirs ---')
stack = [0]
seen = set()
while stack:
    off = stack.pop()
    if off in seen or off == 0xFFFFFFFF:
        continue
    seen.add(off)
    r = dir_rec(off)
    print('dir %04X parent=%04X name=%r cdir=%04X cfile=%04X'
          % (r['off'], r['parent'], r['name'], r['cdir'], r['cfile']))
    c = r['cdir']
    while c != 0xFFFFFFFF:
        stack.append(c)
        c = dir_rec(c)['next']

uk = None
for off in seen:
    r = dir_rec(off)
    if r['name'] == 'uk' and r['parent'] != 0xFFFFFFFF:
        p = dir_rec(r['parent'])
        if p['name'] == 'snd':
            uk = r['off']
print('\nuk dir offset =', hex(uk) if uk is not None else None)

if uk is not None:
    h = rhash('product.bcsar', uk)
    b = h % FILE_HASH_COUNT
    print('hash(product.bcsar, seed=%04X) = %08X -> bucket %d' % (uk, h, b))
    head = u32(FILE_HASH + b * 4)
    print('bucket[%d] = %04X' % (b, head))
    cur = head
    n = 0
    while cur != 0xFFFFFFFF and n < 40:
        r = file_rec(cur)
        print('   %04X parent=%04X next=%04X size=%d name=%r'
              % (r['off'], r['parent'], r['next'], r['dsz'], r['name']))
        cur = r['next']
        n += 1
