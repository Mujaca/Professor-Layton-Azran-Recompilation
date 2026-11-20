import struct

BLOB = 0x1000
d = open('out/romfs_image.bin', 'rb').read(BLOB + 0xDD80)[BLOB:]

DIR_HASH = 0x28
DIR_META = 0x4C
DIR_COUNT = 0x24 // 4
FILE_HASH = 0x160
FILE_META = 0xAF4


def u32(o):
    return struct.unpack_from('<I', d, o)[0]


def name16(base, nlen_off, name_off):
    n = u32(base + nlen_off)
    return d[base + name_off: base + name_off + n].decode('utf-16-le', 'replace')


def rhash(name, seed):
    h = seed ^ 0x75BCD15
    for ch in name:
        h = (ord(ch) ^ (((h >> 5) | (h << 27)) & 0xFFFFFFFF)) & 0xFFFFFFFF
    return h


def drec(off):
    base = DIR_META + off
    return dict(off=off, parent=u32(base), next=u32(base + 4),
                cdir=u32(base + 8), cfile=u32(base + 0xC),
                hnext=u32(base + 0x10),
                name=name16(base, 0x14, 0x18))


# gather dirs via childDir/sibling
dirs = {}
stack = [0]
while stack:
    off = stack.pop()
    if off in dirs or off == 0xFFFFFFFF:
        continue
    r = drec(off)
    dirs[off] = r
    c = r['cdir']
    while c != 0xFFFFFFFF:
        stack.append(c)
        c = drec(c)['next']

print('dirs found:', len(dirs))
print('bucket90 (image) = %04X' % u32(FILE_HASH + 90 * 4))

print('\n--- dir hash check (HASHNEXT @ +0x10) ---')
for off, r in sorted(dirs.items()):
    if off == 0:
        print('root: cdir=%04X cfile=%04X' % (r['cdir'], r['cfile']))
        continue
    b = rhash(r['name'], r['parent']) % DIR_COUNT
    head = u32(DIR_HASH + b * 4)
    cur = head
    found = False
    n = 0
    while cur != 0xFFFFFFFF and n < 50:
        if cur == off:
            found = True
            break
        cur = drec(cur)['hnext']
        n += 1
    print('dir %04X %-8r parent=%04X bucket=%d head=%04X found=%s'
          % (off, r['name'], r['parent'], b, head, found))
