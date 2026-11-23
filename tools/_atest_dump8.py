"""Dump eines 8bpp- und 4bpp-IMGC (Header, Umfeld nach Stream)."""
import struct

B = open('out/romfs/lt6/arc/lt6_b.fa', 'rb').read()


def find(bits, w, h):
    i = 0
    while True:
        o = B.find(b'IMGC00', i)
        if o < 0:
            return None
        i = o + 1
        if B[o + 0x0D] != bits:
            continue
        ww, hh = struct.unpack_from('<hh', B, o + 0x10)
        if (ww, hh) == (w, h):
            return o


def dump(o):
    print('off', o)
    for k in range(0, 0x48, 16):
        print('  %04X %s' % (k, ' '.join('%02X' % x
                                          for x in B[o + k:o + k + 16])))
    toff = struct.unpack_from('<i', B, o + 0x1C)[0]
    s1, s2, sz = struct.unpack_from('<iii', B, o + 0x34)
    print('  toff', toff, 's1', s1, 's2', s2, 'sz', sz,
          'end=0x%X' % (o + toff + s2 + sz))


for (bits, w, h) in ((8, 512, 256), (4, 512, 256), (8, 256, 256)):
    o = find(bits, w, h)
    print('=== bits', bits, w, h, '->', o)
    if o:
        dump(o)
