"""Sucht die richtige 8x8-Makro-Ordnung fuer 000.xi (720 Permutationen).

`master_getitation` benutzt 6 Bit-Paare in fester Reihenfolge. Wenn die
Reihenfolge fuer title_b/000.xi anders ist, sind die 8x8-Bloecke permutiert
und die Textzeilen unlesbar. Dieses Tool probiert alle 720 Reihenfolgen,
baut das Bild vektorisiert und misst NCC zwischen dem "new"-Label-Crop
(Alpha) und dem Capture-Rechteck an der validierten ANMC-Position.

Aufruf: python tools/anmc_order_search.py
"""
import itertools
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import imgc_order  # noqa: E402

XI = os.path.join(ROOT, 'tools/out/title_b_000.xi')
REF = os.path.join(ROOT, 'reports/menu_official.bin')
BITS = [(0, 1), (1, 0), (0, 2), (2, 0), (0, 4), (4, 0)]
# "new"-Label: uv (0,0)-(105,22), ANMC-Rect bei (107,108)
CROP = (0, 0, 105, 22)
POS = (107, 108, 212, 130)


def main():
    bitdepth, bpt, w, h, pic = imgc_order.load(XI)
    bpp = bitdepth // 8
    w8 = (w + 7) & ~7
    ref = np.fromfile(REF, dtype=np.uint8).reshape(240, 400, 3).astype(np.float64)
    g = ref.mean(2)
    x0, y0, x1, y1 = POS
    cap = g[y0:y0 + 22, x0:x0 + 105]
    cap = cap - cap.mean()
    capn = np.sqrt((cap * cap).sum())

    yy, xx = np.mgrid[0:h, 0:w]
    idx = (yy * w8 + xx).ravel()
    picarr = np.frombuffer(bytes(pic), dtype=np.uint8).reshape(-1, bpp)

    best = []
    for perm in itertools.permutations(range(6)):
        px = (xx.ravel() & ~7).copy()
        py = (yy.ravel() & ~7).copy()
        for j, bi in enumerate(perm):
            bx, by = BITS[bi]
            sel = ((idx >> j) & 1).astype(bool)
            px[sel] ^= bx
            py[sel] ^= by
        src = (py * w8 + px)
        img = picarr[src].reshape(h, w, bpp)
        u0, v0, u1, v1 = CROP
        a = img[v0:v1, u0:u1, 3].astype(np.float64)
        a = a - a.mean()
        d = np.sqrt((a * a).sum()) * capn
        sc = float((a * cap).sum() / d) if d > 1e-9 else -9.0
        best.append((sc, perm))
    best.sort(key=lambda b: -b[0])
    print('Top-5 Makro-Ordnungen (Permutation der 6 Bit-Paare):')
    for sc, perm in best[:5]:
        print('  ncc %.4f  perm=%s' % (sc, perm))
    print('Identity war', tuple(range(6)), '->',
          '%.4f' % dict((p, s) for s, p in best)[tuple(range(6))])


if __name__ == '__main__':
    main()
