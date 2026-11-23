"""Menue-Elemente aus XPVB + Atlas verifizieren und im Capture wiederfinden.

Zwei Schritte, beide gemessen statt geraten:

1. `elements()` paart die ANMC-Elementnamen (`tools/profiles/anmc_menu.txt`)
   mit den XPVB-Quads aus `title_b.xa/000.pvb`. Die ANMC-Floats sind
   (Breite, Hoehe) - nicht Positionen; die Quads liefern Groesse + UV.
2. `match()` sucht jede Element-Textur (Crop aus `000.xi`) per maskierter
   Normalized-Cross-Correlation im Referenzbild und liefert die echte
   Bildschirmposition.

Aufruf: python tools/menu_match.py
"""
import os
import re
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _imgc  # noqa: E402

ATLAS = os.path.join(ROOT, 'tools/out/title_b_000.xi')
REF = os.path.join(ROOT, 'reports/menu_official.bin')
RECTS = os.path.join(ROOT, 'tools/out/titleb_rects.txt')


def load_pvb():
    pat = re.compile(r'quad\s+(\d+)\s+pos=\(\s*(-?[\d.]+),\s*(-?[\d.]+)\)'
                     r'\s+([\d.]+)x([\d.]+)\s+uv=\(\s*(\d+),\s*(\d+)\)-'
                     r'\(\s*(\d+),\s*(\d+)\)')
    rows = []
    for line in open(RECTS, encoding='utf-16'):
        m = pat.search(line)
        if m:
            rows.append((float(m.group(2)), float(m.group(3)),
                         float(m.group(4)), float(m.group(5)),
                         int(m.group(6)), int(m.group(7)),
                         int(m.group(8)), int(m.group(9))))
    return rows


def elements():
    return load_pvb()


def ncc_search(img, tmpl, mask):
    """Maskierte NCC: liefert (score, x, y) fuer jeden Platz im Bild."""
    H, W = img.shape
    h, w = tmpl.shape
    m = mask.astype(np.float64)
    n = m.sum()
    if n < 10:
        return -9.0, 0, 0
    tm = tmpl * m
    tmean = tm.sum() / n
    tz = (tm - tmean) * m
    tvar = (tz * tz).sum()
    if tvar <= 1e-9:
        return -9.0, 0, 0
    # FFT-Korrelation
    size = (H + 2 * h, W + 2 * w)
    FI = np.fft.rfft2(img, size)
    FT = np.fft.rfft2(np.flipud(np.fliplr(tz)), size)
    num = np.fft.irfft2(FI * FT, size)[h - 1:H + h - 1, w - 1:W + w - 1]
    # lokale Summen von I und I^2 unter der Maske
    ones = np.zeros_like(img)
    ones[:h, :w] = m
    FI1 = np.fft.rfft2(img, size)
    F1 = np.fft.rfft2(np.flipud(np.fliplr(ones)), size)
    s1 = np.fft.irfft2(FI1 * F1, size)[h - 1:H + h - 1, w - 1:W + w - 1]
    FI2 = np.fft.rfft2(img * img, size)
    s2 = np.fft.irfft2(FI2 * F1, size)[h - 1:H + h - 1, w - 1:W + w - 1]
    var = np.maximum(s2 - s1 * s1 / n, 1e-6)
    r = num / np.sqrt(var * tvar)
    k = int(np.argmax(r))
    yy, xx = divmod(k, r.shape[1])
    return float(r[yy, xx]), xx, yy


def main():
    w, h, bpp, px = _imgc.decode(ATLAS)
    at = np.frombuffer(px, dtype=np.uint8).reshape(h, w, 4).astype(np.float64)
    ref = np.fromfile(REF, dtype=np.uint8).reshape(240, 400, 3).astype(np.float64)
    g = ref.mean(2)
    print('Atlas %dx%d, Referenz 400x240' % (w, h))
    print('%5s %-8s %9s %9s  %s' % ('quad', 'size', 'uv', 'score', 'best-pos'))
    for i, (qx, qy, qw, qh, u0, v0, u1, v1) in enumerate(elements()):
        cw, ch = int(u1 - u0), int(v1 - v0)
        if cw <= 0 or ch <= 0 or u1 > w or v1 > h:
            continue
        crop = at[v0:v1, u0:u1]
        a = crop[..., 3] > 8
        if a.sum() < 40:
            print('%5d %-8s uv=%3d,%3d  (Alpha zu klein)' % (i, '%dx%d' % (cw, ch), u0, v0))
            continue
        t = crop[..., :3].mean(2)
        sc, x, y = ncc_search(g, t, a)
        print('%5d %-8s %3d,%-3d %3d,%-3d  %8.3f  (%3d,%3d)'
              % (i, '%dx%d' % (cw, ch), u0, v0, u1, v1, sc, x, y))


if __name__ == '__main__':
    main()
