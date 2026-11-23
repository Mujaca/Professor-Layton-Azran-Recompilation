"""Hough: Translationen aus exakten 8x8-Blocktreffern.

Fuer jeden Atlas wird fuer jeden 8x8-Block (Schritt 8) die Referenzposition
des exakt gleichen Blocks gesucht. Die resultierende Translation (dx,dy)
wird gezaehlt; Gruppen mit vielen Stimmen sind platzierte Sprites/Grossbilder.

Aufruf:  python tools\\_atest_hough.py [min_pct] [max_mehrdeutig]
"""
import collections
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _atest_imgc_np as D

FAS = ['out/romfs/lt6/arc/lt6_b.fa', 'out/romfs/lt6/arc/lt6_uk.fa',
       'out/romfs/lt6/arc/lt6_a.fa']
REF = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 400, 3)
RH, RW = 240, 400


def main():
    min_pct = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
    maxamb = int(sys.argv[2]) if len(sys.argv) > 2 else 6
    keys = {}
    for y in range(0, RH - 7):
        for x in range(0, RW - 7):
            keys.setdefault(REF[y:y + 8, x:x + 8].tobytes(), []).append((x, y))
    print('ref keys', len(keys))
    rows = []
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
            try:
                r = D.decode_buf(d, o)
            except Exception:
                continue
            if r is None:
                continue
            w, h, rgb, alpha = r
            votes = collections.Counter()
            hit = tot = skipped = 0
            for y in range(0, h - 7, 8):
                for x in range(0, w - 7, 8):
                    tot += 1
                    pos = keys.get(rgb[y:y + 8, x:x + 8].tobytes())
                    if not pos:
                        continue
                    hit += 1
                    if len(pos) > maxamb:
                        skipped += 1
                        continue
                    for (rx, ry) in pos:
                        votes[(rx - x, ry - y)] += 1
            if hit * 100.0 / max(tot, 1) >= min_pct:
                rows.append((hit, tot, skipped, votes, fa, o, w, h,
                             hd['bits']))
    rows.sort(key=lambda z: -z[0])
    print('--- Kandidaten mit >= %.0f%% Blocktreffern ---' % min_pct)
    for (hit, tot, skipped, votes, fa, o, w, h, bits) in rows:
        top = votes.most_common(8)
        print('%-14s off=%-10d %4dx%-4d bits=%2d hit=%4d/%-4d ambig=%-4d top=%s'
              % (os.path.basename(fa), o, w, h, bits, hit, tot, skipped,
                 ', '.join('(%d,%d):%d' % (k[0], k[1], v) for k, v in top)))


if __name__ == '__main__':
    main()
