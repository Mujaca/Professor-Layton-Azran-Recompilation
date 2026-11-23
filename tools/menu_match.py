"""Menue-Elemente aus XPVB + Atlas im Capture wiederfinden (maskierte NCC).

Die ANMC-Floats sind (Breite, Hoehe) je Menuepunkt; Groesse + UV stehen im
XPVB (`title_b.xa/000.pvb`). Dieses Tool sucht die Element-Textur aus dem
Overlay-Atlas (000.xi) per maskierter Normalized-Cross-Correlation im
Referenzbild und liefert damit die ECHTE Bildschirmposition - gemessen,
nicht geraten.

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
RECTS = os.path.join(ROOT, 'tools/profiles/titleb_rects.txt')

PAT = re.compile(r'quad\s+(\d+)\s+pos=\(\s*(-?[\d.]+),\s*(-?[\d.]+)\)'
                 r'\s+([\d.]+)x([\d.]+)\s+uv=\(\s*(\d+),\s*(\d+)\)-'
                 r'\(\s*(\d+),\s*(\d+)\)')


def load_pvb():
    rows = []
    for line in open(RECTS, encoding='utf-8'):
        m = PAT.search(line)
        if m:
            rows.append((float(m.group(2)), float(m.group(3)),
                         float(m.group(4)), float(m.group(5)),
                         int(m.group(6)), int(m.group(7)),
                         int(m.group(8)), int(m.group(9))))
    return rows


def corr2d(img, tmpl):
    """Valid-Region von sum(I[i+u, j+v] * T[u, v])."""
    H, W = img.shape
    h, w = tmpl.shape
    s = (H + h - 1, W + w - 1)
    fi = np.fft.rfft2(img, s)
    ft = np.fft.rfft2(tmpl[::-1, ::-1], s)
    c = np.fft.irfft2(fi * ft, s)
    return c[h - 1:H, w - 1:W]


def ncc_search(img, tmpl, mask):
    h, w = tmpl.shape
    m = mask.astype(np.float64)
    n = float(m.sum())
    if n < 10.0:
        return -9.0, 0, 0
    tm = tmpl * m
    tmean = tm.sum() / n
    tz = (tm - tmean) * m
    tvar = float((tz * tz).sum())
    if tvar <= 1e-9:
        return -9.0, 0, 0
    num = corr2d(img, tz)
    s1 = corr2d(img, m)
    s2 = corr2d(img * img, m)
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
    print('%5s %-8s %-14s %8s  %s' % ('quad', 'size', 'uv', 'ncc', 'best-pos'))
    for i, (_qx, _qy, qw, qh, u0, v0, u1, v1) in enumerate(load_pvb()):
        if u1 > w or v1 > h or u1 <= u0 or v1 <= v0:
            continue
        crop = at[v0:v1, u0:u1]
        a = crop[..., 3] > 8
        tag = '%dx%d' % (int(qw), int(qh))
        if a.sum() < 40:
            print('%5d %-8s %3d,%-3d %3d,%-3d       -  (Alpha %d)'
                  % (i, tag, u0, v0, u1, v1, int(a.sum())))
            continue
        sc, x, y = ncc_search(g, crop[..., :3].mean(2), a)
        print('%5d %-8s %3d,%-3d %3d,%-3d  %7.3f  (%3d,%3d)'
              % (i, tag, u0, v0, u1, v1, sc, x, y))


if __name__ == '__main__':
    main()
