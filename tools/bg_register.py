"""Registrierung: welches Spiel-Asset enthaelt den Top-Screen (400x240)?

Zwei Modi je Asset:
  crop  - 400x240-Fenster aus dem Asset (dx/dy-Raster)
  scale - ganzes Asset auf 400x240 skaliert (nur wenn Seitenverhaeltnis passt)

Meldet die beste Korrelation der Helligkeit gegen den Capture.

Aufruf: python tools/bg_register.py
"""
import glob
import os

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, 'reports/menu_official.bin')


def corr(rg, g):
    return float(np.corrcoef(rg.ravel(), g.ravel())[0, 1])


def main():
    ref = np.fromfile(REF, dtype=np.uint8).reshape(240, 400, 3).astype(np.float32)
    rg = ref.mean(2)
    results = []
    for p in sorted(glob.glob(os.path.join(ROOT, 'out/title3d_*.png'))):
        im = Image.open(p).convert('RGB')
        w, h = im.size
        a = np.asarray(im, dtype=np.float32)
        if w >= 400 and h >= 240:
            best = (-2.0, None)
            for dx in range(0, w - 400 + 1, 4):
                for dy in range(0, h - 240 + 1, 2):
                    c = corr(rg, a[dy:dy + 240, dx:dx + 400].mean(2))
                    if c > best[0]:
                        best = (c, ('crop', dx, dy))
            results.append((best[0], os.path.basename(p), best[1]))
        best = (-2.0, None)
        for sx in np.arange(0.6, 1.3, 0.05):
            for sy in np.arange(0.6, 1.3, 0.05):
                im2 = im.resize((int(w * sx), int(h * sy)))
                a2 = np.asarray(im2, dtype=np.float32).mean(2)
                H, W = a2.shape
                if H > 240 or W > 400:
                    continue
                for dy in range(0, 240 - H + 1, 8):
                    for dx in range(0, 400 - W + 1, 8):
                        canvas = np.zeros((240, 400), np.float32)
                        canvas[dy:dy + H, dx:dx + W] = a2
                        c = corr(rg, canvas)
                        if c > best[0]:
                            best = (c, ('scale', round(float(sx), 2),
                                        round(float(sy), 2), dx, dy))
        results.append((best[0], os.path.basename(p), best[1]))
    results.sort(key=lambda r: -r[0])
    for c, name, how in results[:10]:
        print('%6.3f  %-24s %s' % (c, name, how))


if __name__ == '__main__':
    main()
