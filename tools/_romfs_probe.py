import struct

BLOB = 0x1000
d = open('out/romfs_image.bin', 'rb').read(BLOB + 0xDD80)[BLOB:]

DIR_HASH = 0x28
DIR_META = 0x4C
FILE_HASH = 0x160
FILE_META = 0xAF4
FILE_META_SIZE = 0xD284


def u32(off):
    return struct.unpack_from('<I', d, off)[0]


def u64(off):
    return struct.unpack_from('<Q', d, off)[0]


def name_at(off, maxlen=64):
    # UTF-16LE, terminated by 0
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
    name = name_at(base + 0x18)
    return dict(off=off, base=base, parent=parent, next=nxt, cdir=cdir,
                cfile=cfile, x=x, nlen=nlen, name=name)


def file_rec(off):
    base = FILE_META + off
    parent, nxt = struct.unpack_from('<II', d, base)
    doff = u64(base + 8)
    dsz = u64(base + 0x10)
    x, nlen = struct.unpack_from('<II', d, base + 0x18)
    name = name_at(base + 0x1C)
    return dict(off=off, base=base, parent=parent, next=nxt, doff=doff,
                dsz=dsz, x=x, nlen=nlen, name=name)


def rhash(name, seed):
    h = seed ^ 0x75BCD15
    for ch in name:
        c = ord(ch)
        h = (c ^ (((h >> 5) | (h << 27)) & 0xFFFFFFFF)) & 0xFFFFFFFF
    return h


print('--- root dir (offset 0) ---')
print(dir_rec(0)
      )
print('--- children of root ---')
root = dir_rec(0)
print('first child dir off =', hex(root['cdir']),
      'first child file off =', hex(root['cfile']))

# walk dir chain
cur = root['cdir']
seen = 0
while cur != 0xFFFFFFFF and seen < 30:
    r = dir_rec(cur)
    print('dir %04X parent=%04X next=%04X cdir=%04X cfile=%04X name=%r'
          % (r['off'], r['parent'], r['next'], r['cdir'], r['cfile'], r['name']))
    cur = r['next']
    seen += 1

print()
printbudget = 0
print('--- file hash table first 12 buckets ---')
for i in range(12):
    print('  bucket %3d = %04X' % (i, u32(FILE_HASH + i * 4)))

print()
print('--- candidate record at FILE_META+0x948 ---')
try:
    r = file_rec(0x948)
    print(r)
except Exception as e:
    print('err', e)
