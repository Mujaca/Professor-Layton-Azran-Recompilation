"""Verteilung des Headerfelds 0x0A (Tile-Modus?) je bpp/Groesse."""
import collections
import struct
import sys

FAS = ['out/romfs/lt6/arc/lt6_b.fa',
       'out/romfs/lt6/arc/lt6_uk.fa',
       'out/romfs/lt6/arc/lt6_a.fa']

c = collections.Counter()
per = collections.defaultdict(collections.Counter)
for fa in FAS:
    d = open(fa, 'rb').read()
    i = 0
    while True:
        o = d.find(b'IMGC00', i)
        if o < 0:
            break
        i = o + 1
        f = struct.unpack_from('<H', d, o + 0x0A)[0]
        bits = d[o + 0x0D]
        c[f] += 1
        per[f][bits] += 1
for f, n in c.most_common():
    print('0x%04X %5d  bits=%s' % (f, n, dict(per[f])))
