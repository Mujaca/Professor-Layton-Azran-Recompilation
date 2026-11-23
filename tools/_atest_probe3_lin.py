"""Probe 3 (LINEAR): exakte, nicht-uniforme 8x8-Bloecke in der Referenz."""
import os
import sys
import time

import numpy as np

sys.path.insert(0, 'tools')
import _atest_botscan as BS
import _atest_imgc_np as D

FAS = ['out/romfs/lt6/arc/lt6_b.fa',
       'out/romfs/lt6/arc/lt6_uk.fa',
       'out/romfs/lt6/arc/lt6_a.fa']
REF = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 400, 3)
RH, RW = 240, 400


def main():
    keys = {}
    for y in range(0, RH - 7):
        for x in range(0, RW - 7):
            b = REF[y:y + 8, x:x + 8]
            if (b == b[0, 0]).all():
                continue
            keys.setdefault(b.tobytes(), (x, y))
    print('ref non-uniform keys', len(keys))
    rows = []
    t0 = time.time()
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
                img = BS.decode_linear(d, o)
            except Exception:
                continue
            if img is None:
                continue
            h, w = img.shape[:2]
            hit = tot = 0
            for y in range(0, h - 7, 8):
                for x in range(0, w - 7, 8):
                    b = img[y:y + 8, x:x + 8]
                    if (b == b[0, 0]).all():
                        continue
                    tot += 1
                    if b.tobytes() in keys:
                        hit += 1
            if hit:
                rows.append((hit, tot, fa, o, w, h, hd['bits']))
    rows.sort(key=lambda z: -z[0])
    print('atlanten mit Treffern', len(rows), 'dauer %.1fs' % (time.time() - t0))
    for r in rows[:20]:
        print('hit=%4d/%-5d %5.1f%%  %-14s off=%-10d %4dx%-4d bits=%d'
              % (r[0], r[1], r[0] / r[1] * 100, os.path.basename(r[2]), r[3],
                 r[4], r[5], r[6]))


if __name__ == '__main__':
    main()
