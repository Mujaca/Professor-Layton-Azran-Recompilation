"""Survey 2: Verteilung bits/bpt und dekodierbare Kandidaten."""
import collections
import struct

FAS = ['out/romfs/lt6/arc/lt6_b.fa', 'out/romfs/lt6/arc/lt6_uk.fa',
       'out/romfs/lt6/arc/lt6_a.fa']

WANT = ((400, 240), (320, 240), (512, 256), (256, 256), (512, 512),
        (128, 128), (1024, 512))


def main():
    bits_c = collections.Counter()
    per = collections.Counter()
    ok = collections.Counter()
    for fa in FAS:
        d = open(fa, 'rb').read()
        i = 0
        while True:
            o = d.find(b'IMGC00', i)
            if o < 0:
                break
            i = o + 1
            if o + 0x48 > len(d):
                continue
            bits = d[o + 0x0D]
            w, h = struct.unpack_from('<hh', d, o + 0x10)
            bpt = struct.unpack_from('<h', d, o + 0x0E)[0]
            bits_c[(bits, bpt)] += 1
            bpp = bits // 8 if bits >= 8 else 0
            valid = bits in (24, 32) and bpt == 64 * bpp
            per[(w, h, bits)] += 1
            if valid:
                ok[(w, h)] += 1
    print('--- (bits,bpt) ---')
    for k, v in bits_c.most_common(20):
        print(k, v)
    print('--- dekodierbar (24/32bpp, bpt ok), top sizes ---')
    for k, v in ok.most_common(25):
        print('%4dx%-4d %4d' % (k[0], k[1], v))
    print('sum decodable', sum(ok.values()))
    print('--- Bits je interessante Groesse ---')
    for (w, h, b), v in sorted(per.items()):
        if (w, h) in WANT:
            print('%4dx%-4d bits=%-2d %d' % (w, h, b, v))


if __name__ == '__main__':
    main()
