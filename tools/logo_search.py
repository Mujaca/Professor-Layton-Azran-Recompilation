"""Steckt das Logo (helle Zone im Capture) in einer title.xc-Textur?

Nimmt die helle Zone `x 100..275, y 60..110` als Template und sucht sie
in den dekodierten Texturen (mehrere Skalen) per NCC.

Aufruf: python tools/logo_search.py
"""
import glob
import os

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, 'reports/menu_official.bin')


def corr2d(img, tmpl):
    H, W = img.shape
    h, w = tmpl.shape
    s = (H + h - 1, W + w - 1)
    fi = np.fft.rfft2(img, s)
    ft = np.fft.rfft2(tmpl[::-1, ::-1], s)
    c = np.fft.irfft2(fi * ft, s)
    return c[h - 1:H, w - 1:W]


def ncc_max(img, tmpl):
    t = tmpl - tmpl.mean()
    d = np.sqrt((t * t).sum())
    if d < 1e-6:
        return -9.0, 0, 0
    num = corr2d(img, t)
    s1 = corr2d(img, np.ones_like(t))
    s2 = corr2d(img * img, np.ones_like(t))
    n = float(t.size)
    var = np.maximum(s2 - s1 * s1 / n, 1e-6)
    r = num / np.sqrt(var * (t * t).sum())
    k = int(np.argmax(r))
    yy, xx = divmod(k, r.shape[1])
    return float(r[yy, xx]), xx, yy


def main():
    ref = np.fromfile(REF, dtype=np.uint8).reshape(240, 400, 3).astype(np.float64)
    band = ref[60:110, 100:275].mean(2)
    results = []
    for p in sorted(glob.glob(os.path.join(ROOT, 'out/title3d_*.png'))):
        im = Image.open(p).convert('RGB')
        for sc in (0.5, 0.75, 1.0, 1.25, 1.5):
            w = max(8, int(band.shape[1] * sc))
            h = max(8, int(band.shape[0] * sc))
            t = np.asarray(im.resize((w, h)), dtype=np.float64).mean(2)
            a = np.asarray(im, dtype=np.float64).mean(2)
            if t.shape[0] > a.shape[0] or t.shape[1] > a.shape[1]:
                continue
            s, x, y = ncc_max(a, t)
            results.append((s, os.path.basename(p), sc, x, y))
    results.sort(key=lambda r: -r[0])
    for s, name, sc, x, y in results[:8]:
        print('%6.3f  %-24s scale %.2f @(%3d,%3d)' % (s, name, sc, x, y))


if __name__ == '__main__':
    main()
