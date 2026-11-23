"""A2/A-test Survey: IMGC00-Kandidaten in den .fa-Archiven zaehlen und
Headerfelder (Groesse, bpp, Kompression) auswerten."""
import collections
import os
import struct
import sys

FAS = ['out/romfs/lt6/arc/lt6_b.fa', 'out/romfs/lt6/arc/lt6_uk.fa',
       'out/romfs/lt6/arc/lt6_a.fa']


def scan(path):
    d = open(path, 'rb').read()
    offs = []
    i = 0
    while True:
        j = d.find(b'IMGC00', i)
        if j < 0:
            break
        offs.append(j)
        i = j + 1
    return d, offs


def parse(d, o):
    if o + 0x48 > len(d):
        return None
    bitdepth = d[o + 0x0D]
    bpt = struct.unpack_from('<h', d, o + 0x0E)[0]
    w, h = struct.unpack_from('<hh', d, o + 0x10)
    table_off = struct.unpack_from('<i', d, o + 0x1C)[0]
    s1, s2, sz = struct.unpack_from('<iii', d, o + 0x34)
    return dict(off=o, w=w, h=h, bpp=bitdepth // 8, bitdepth=bitdepth,
                bpt=bpt, table_off=table_off, s1=s1, s2=s2, sz=sz,
                meth_t=(d[o + table_off] & 7) if 0 <= table_off < len(d) - o else -1,
                meth_x=(d[o + 0x48] & 7) if o + 0x48 < len(d) else -1)


def main():
    total = collections.Counter()
    sizes = collections.Counter()
    for fa in FAS:
        d, offs = scan(fa)
        cnt = collections.Counter()
        for o in offs:
            p = parse(d, o)
            if not p:
                continue
            total['hdr_ok'] += 1
            sizes[(p['w'], p['h'])] += 1
            cnt[(p['w'], p['h'])] += 1
        print('%s: %d IMGC00, top sizes: %s' % (
            os.path.basename(fa), len(offs),
            ', '.join('%dx%d:%d' % (k[0], k[1], v)
                      for k, v in cnt.most_common(8))))
    print('--- global top sizes ---')
    for (w, h), n in sizes.most_common(30):
        print('%4dx%-4d %5d' % (w, h, n))
    print('--- Beispiele (erste 12 mit hdr) ---')
    d, offs = scan(FAS[0])
    k = 0
    for o in offs:
        p = parse(d, o)
        if not p:
            continue
        print('%9d %4dx%-4d bpp=%d bpt=%-5d table_off=0x%X s1=%-7d s2=%-7d '
              'sz=%-8d methT=%d meth0x48=%d' % (
                  p['off'], p['w'], p['h'], p['bpp'], p['bpt'],
                  p['table_off'], p['s1'], p['s2'], p['sz'],
                  p['meth_t'], p['meth_x']))
        k += 1
        if k >= 12:
            break


if __name__ == '__main__':
    main()
