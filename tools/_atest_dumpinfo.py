import struct

UK = open('out/romfs/lt6/arc/lt6_uk.fa', 'rb').read()


def dump(d, o, name):
    w, h = struct.unpack_from('<hh', d, o + 0x10)
    bits = d[o + 0x0D]
    bpt = struct.unpack_from('<h', d, o + 0x0E)[0]
    toff = struct.unpack_from('<i', d, o + 0x1C)[0]
    s1, s2, sz = struct.unpack_from('<iii', d, o + 0x34)
    print('%s off=0x%X %dx%d bits=%d bpt=%d toff=0x%X s1=%d s2=%d sz=%d'
          % (name, o, w, h, bits, bpt, toff, s1, s2, sz))
    for k in range(0, 0x48, 16):
        print('   %04X %s' % (k, ' '.join('%02X' % x
                                          for x in d[o + k:o + k + 16])))
    for label, off, ln in (('table', toff, s1), ('stream', toff + s2, sz)):
        b = d[o + off:o + off + min(ln, 48)]
        print('  %s @0x%X (%d B): meth=%d %s'
              % (label, off, ln, d[o + off] & 7,
                 ' '.join('%02X' % x for x in b)))


dump(UK, 0x75CCC4, 'uk/menu/uk/info_a.xi')
dump(UK, 0x75E104, 'uk/menu/uk/info_b.xi')
