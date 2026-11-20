import struct

BLOB = 0x1000
d = open('out/romfs_image.bin', 'rb').read(BLOB + 0xDD80)[BLOB:]

FILE_HASH = 0x160
FILE_META = 0xAF4
COUNT = 0x994 // 4


def u32(off):
    return struct.unpack_from('<I', d, off)[0]


def rhash(name, seed):
    h = seed ^ 0x75BCD15
    for ch in name:
        h = (ord(ch) ^ (((h >> 5) | (h << 27)) & 0xFFFFFFFF)) & 0xFFFFFFFF
    return h


def name16(off):
    out = []
    for i in range(60):
        c = struct.unpack_from('<H', d, off + i * 2)[0]
        if c == 0:
            break
        out.append(chr(c))
    return ''.join(out)


print('dir rec 0xD4 bytes:')
base = 0x4C + 0xD4
for i in range(0, 0x40, 4):
    print('  +%02X: %08X' % (i, u32(base + i)))
print('  name@+0x18 =', repr(name16(base + 0x18)))
print('  name@+0x1A =', repr(name16(base + 0x1A)))

print('\nfile rec 0x948 bytes:')
fbase = FILE_META + 0x948
for i in range(0, 0x40, 4):
    print('  +%02X: %08X' % (i, u32(fbase + i)))
print('  name@+0x18 =', repr(name16(fbase + 0x18)))
print('  name@+0x1A =', repr(name16(fbase + 0x1A)))
print('  name@+0x1C =', repr(name16(fbase + 0x1C)))

h = rhash('product.bcsar', 0xD4)
b = h % COUNT
print('\nhash = %08X  bucket = %d' % (h, b))
print('bucket[%d] = %04X' % (b, u32(FILE_HASH + b * 4)))

# walk chain from that bucket
cur = u32(FILE_HASH + b * 4)
n = 0
while cur != 0xFFFFFFFF and n < 60:
    fb = FILE_META + cur
    nxt = u32(fb + 4)
    nm = name16(fb + 0x1A) if False else name16(fb + 0x1C)
    print('   %04X next=%04X name@1C=%r' % (cur, nxt, nm))
    cur = nxt
    n += 1
