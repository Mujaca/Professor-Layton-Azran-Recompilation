"""Exploration: Passen 8x8-Bloecke der IMGC-Atlanten in die Menue-Referenz?

Fuer jeden dekodierbaren Kandidaten (bevorzugt 24/32bpp) werden 8x8-Bloecke
auf 4 bit/Kanal quantisiert und gegen die Referenz-Bloecke geprueft
(Referenz in Schrittweite 2 px). Ausgabe: Trefferquote je Atlas.
"""
import collections
import os
import struct
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _atest_imgc_np as D

FAS = ['out/romfs/lt6/arc/lt6_b.fa', 'out/romfs/lt6/arc/lt6_uk.fa',
       'out/romfs/lt6/arc/lt6_a.fa']
REF = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 400, 3)
RW, RH = 400, 240
STEP = 2


def ref_keys():
    q = (REF >> 4).astype(np.uint8)
    ks = set()
    for y in range(0, RH - 7, STEP):
        for x in range(0, RW - 7, STEP):
            ks.add(q[y:y + 8, x:x + 8].tobytes())
    return ks


def atlas_hits(rgb, rkeys):
    h, w = rgb.shape[:2]
    q = (rgb >> 4).astype(np.uint8)
    hit = 0
    tot = 0
    for y in range(0, h - 7, 8):
        for x in range(0, w - 7, 8):
            tot += 1
            if q[y:y + 8, x:x + 8].tobytes() in rkeys:
                hit += 1
    return hit, tot


def main():
    rkeys = ref_keys()
    print('ref blocks', len(rkeys))
    rows = []
    t0 = time.time()
    n = 0
    for fa in FAS:
        d = open(fa, 'rb').read()
        i = 0
        while True:
            o = d.find(b'IMGC00', i)
            if o < 0:
                break
            i = o + 1
            hd = D.parse_header(d, o)
            if not hd or hd['bits'] not in (24, 32):
                continue
            if hd['s1'] <= 0 or hd['sz'] <= 0:
                continue
            try:
                r = D.decode_buf(d, o)
            except Exception:
                continue
            if r is None:
                continue
            w, h, rgb, alpha = r
            hit, tot = atlas_hits(rgb, rkeys)
            rows.append((hit, tot, hit / tot if tot else 0.0, fa, o, w, h,
                         hd['bits']))
            n += 1
            if n % 100 == 0:
                print('... %d atlanten %.1fs' % (n, time.time() - t0))
                sys.stdout.flush()
    rows.sort(key=lambda z: -z[2])
    print('--- Top 25 nach Trefferquote ---')
    for r in rows[:25]:
        print('%6.1f%%  %5d/%-5d  %-14s off=%-10d %4dx%-4d bits=%d' % (
            r[2] * 100, r[0], r[1], os.path.basename(r[3]), r[4], r[5],
            r[6], r[7]))
    print('--- Verteilung ---')
    qs = collections.Counter(int(r[2] * 10) for r in rows)
    for k in sorted(qs):
        print('%d0%%: %d' % (k, qs[k]))


if __name__ == '__main__':
    main()
