"""Tile-Ordnung von 000.xi pruefen: linear vs Z-Ordnung.

Vergleicht die Text-Crops (Alpha) mit dem Capture-Bereich der Menue-Zeile
per NCC. Die Ordnung mit dem hoeheren NCC ist die richtige.

Aufruf: python tools/imgc_check.py
"""
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import imgc_order  # noqa: E402

XI = os.path.join(ROOT, 'tools/out/title_b_000.xi')
REF = os.path.join(ROOT, 'reports/menu_official.bin')
CROPS = [('new', 0, 0, 105, 22), ('continue', 0, 48, 103, 70),
         ('scr', 0, 96, 99, 118), ('setting', 0, 144, 150, 168)]


def ncc_at(img, t):
    """Max-NCC von Template t ueber img; gibt (score, x, y)."""
    H, W = img.shape
    h, w = t.shape
    tz = t - t.mean()
    tvar = float((tz * tz).sum())
    if tvar <= 1e-9:
        return -9.0, 0, 0
    best = (-9.0, 0, 0)
    for y in range(0, H - h + 1):
        for x in range(0, W - w + 1):
            win = img[y:y + h, x:x + w]
            wc = win - win.mean()
            d = np.sqrt(float((wc * wc).sum()) * tvar)
            if d <= 1e-9:
                continue
            s = float((wc * tz).sum() / d)
            if s > best[0]:
                best = (s, x, y)
    return best


def main():
    ref = np.fromfile(REF, dtype=np.uint8).reshape(240, 400, 3).astype(np.float64)
    g = ref.mean(2)
    for order in ('z', 'linear'):
        w, h, bpp, out = imgc_order.decode(XI, order)
        a = np.frombuffer(out, dtype=np.uint8).reshape(h, w, 4)
        print('=== order=%s' % order)
        for name, u0, v0, u1, v1 in CROPS:
            mask = (a[v0:v1, u0:u1, 3] > 32).astype(np.float64)
            if mask.sum() < 40:
                print('  %-9s alpha leer' % name)
                continue
            sc, x, y = ncc_at(g, mask)
            print('  %-9s ncc %.3f  @(%3d,%3d)' % (name, sc, x, y))


if __name__ == '__main__':
    main()
