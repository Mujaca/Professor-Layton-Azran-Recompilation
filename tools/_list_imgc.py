import struct
import sys

for p in sys.argv[1:]:
    d = open(p, 'rb').read()
    offs = []
    i = 0
    while True:
        j = d.find(b'IMGC', i)
        if j < 0:
            break
        offs.append(j)
        i = j + 1
    print(p, len(d), 'IMGC count', len(offs), 'offs', offs[:6])
    for o in offs[:6]:
        if o + 0x48 > len(d):
            continue
        w, h = struct.unpack_from('<HH', d, o + 0x10)
        bpp = d[o + 0x0D] // 8
        s1, s2, sz = struct.unpack_from('<iii', d, o + 0x34)
        print('   off=%d %dx%d bpp=%d s1=%d s2=%d sz=%d' % (o, w, h, bpp, s1, s2, sz))
