import struct

BLOB = 0x1000
d = open('out/romfs_image.bin', 'rb').read(BLOB + 0xDD80)[BLOB:]

DIR_HASH = 0x28
DIR_META = 0x4C
DIR_COUNT = 0x24 // 4
FILE_HASH = 0x160
FILE_META = 0xAF4
FILE_COUNT = 0x994 // 4


def u32(off):
    return struct.unpack_from('<I', d, off)[0]


def name16(off):
    out = []
    for i in range(80):
        c = struct.unpack_from('<H', d, off + i * 2)[0]
        if c == 0:
            break
        out.append(chr(c))
    return ''.join(out)


def rhash(name, seed):
    h = seed ^ 0x75BCD15
    for ch in name:
        h = (ord(ch) ^ (((h >> 5) | (h << 27)) & 0xFFFFFFFF)) & 0xFFFFFFFF
    return h


def dir_rec(off):
    base = DIR_META + off
    parent, nxt, cdir, cfile, x, nlen = struct.unpack_from('<IIIIII', d, base)
    return dict(off=off, parent=parent, next=nxt, cdir=cdir, cfile=cfile,
                name=name16(base + 0x18))


def file_rec(off):
    base = FILE_META + off
    parent, nxt = struct.unpack_from('<II', d, base)
    x, nlen = struct.unpack_from('<II', d, base + 0x18)
    name = name16(base + 0x20)
    return dict(off=off, parent=parent, next=nxt, name=name)


# collect all dirs
dirs = []
stack = [0]
seen = set()
while stack:
    off = stack.pop()
    if off in seen or off == 0xFFFFFFFF:
        continue
    seen.add(off)
    r = dir_rec(off)
    dirs.append(r)
    c = r['cdir']
    while c != 0xFFFFFFFF:
        stack.append(c)
        c = dir_rec(c)['next']


# dir hash consistency
ok = bad = 0
for r in dirs:
    if r['off'] == 0:
        continue
    b = rhash(r['name'], r['parent']) % DIR_COUNT
    cur = u32(DIR_HASH + b * 4)
    found = False
    n = 0
    while cur != 0xFFFFFFFF and n < 80:
        if cur == r['off']:
            found = True
            break
        cur = dir_rec(cur)['next']
        n += 1
    if found:
        ok += 1
    else:
        bad += 1
        if bad <= 8:
            print('DIR MISS %r parent=%04X bucket=%d head=%04X'
                  % (r['name'], r['parent'], b, u32(DIR_HASH + b * 4)))
print('dirs: %d ok, %d missing (of %d)' % (ok, bad, len(dirs) - 1))

# collect all files via dirs
files = []
for r in dirs:
    c = r['cfile']
    n = 0
    while c != 0xFFFFFFFF and n < 2000:
        fr = file_rec(c)
        files.append(fr)
        c = fr['next']
        n += 1

ok = bad = 0
for fr in files:
    b = rhash(fr['name'], fr['parent']) % FILE_COUNT
    cur = u32(FILE_HASH + b * 4)
    found = False
    n = 0
    while cur != 0xFFFFFFFF and n < 700:
        if cur == fr['off']:
            found = True
            break
        cur = file_rec(cur)['next']
        n += 1
    if found:
        ok += 1
    else:
        bad += 1
        if bad <= 10:
            print('FILE MISS %r parent=%04X off=%04X bucket=%d head=%04X'
                  % (fr['name'], fr['parent'], fr['off'], b, u32(FILE_HASH + b * 4)))
print('files: %d ok, %d missing (of %d)' % (ok, bad, len(files)))
