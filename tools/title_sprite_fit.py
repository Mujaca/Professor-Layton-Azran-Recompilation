"""title_a-Quads im Capture wiederfinden (maskierte NCC) + Komposition.

Nutzt den echten title_a-Atlas (Alpha-Maske) und sucht jedes XPVB-Quad
im Referenzbild. Die gefundenen Positionen werden auf den registrierten
Hintergrund komponiert; das Ergebnis wird als
`tools/profiles/title_bg_composed.binenz` gespeichert und gemessen.

Aufruf: python tools/title_sprite_fit.py
"""
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, 'reports/menu_official.bin')
ATLAS = os.path.join(ROOT, 'tools/profiles/title_a_atlas.png')
RECTS = os.path.join(ROOT, 'tools/profiles/title_a_rects.txt')
BG = os.path.join(ROOT, 'tools/profiles/title_bg_registered.bin')
OUT = os.path.join(ROOT, 'tools/profiles/title_bg_composed.bin')


def corr2d(img, tmpl):
    H, W = img.shape
    h, w = tmpl.shape
    s = (H + h - 1, W + w - 1)
    fi = np.fft.rfft2(img, s)
    ft = np.fft.rfft2(tmpl[::-1, ::-1], s)
    c = np.fft.irfft2(fi * ft, s)
    return c[h - 1:H, w - 1:W]


def main():
    at = np.asarray(Image.open(ATLAS).convert('RGBA'), dtype=np.float64)
    ref = np.fromfile(REF, dtype=np.uint8).reshape(240, 400, 3).astype(np.float64)
    g = ref.mean(2)
    rows = []
    for ln in open(RECTS, encoding='utf-8'):
        ln = ln.strip()
        if not ln or ln.startswith('#'):
            continue
        x, y, w, h, u0, v0, u1, v1 = [float(v) for v in ln.split()[:8]]
        x0, y0 = int(u0 * 1024), int(v0 * 1024)
        x1, y1 = int(u1 * 1024), int(v1 * 1024)
        crop = at[y0:y1, x0:x1]
        if crop.size == 0:
            continue
        m = (crop[..., 3] > 8).astype(np.float64)
        if m.sum() < 50 or crop.shape[0] > 240 or crop.shape[1] > 400:
            print('%4d,%4d %3dx%-3d  (zu gross/leer, uebersprungen)'
                  % (x, y, w, h))
            continue
        t = crop[..., :3].mean(2)
        n = m.sum()
        tmean = t[m > 0].mean()
        tz = (t - tmean) * m
        tvar = (tz * tz).sum()
        num = corr2d(g, tz)
        s1 = corr2d(g, m)
        s2 = corr2d(g * g, m)
        var = np.maximum(s2 - s1 * s1 / n, 1e-6)
        r = num / np.sqrt(var * tvar)
        k = int(np.argmax(r))
        yy, xx = divmod(k, r.shape[1])
        rows.append((float(r[yy, xx]), int(x), int(y), int(w), int(h),
                     xx, yy, crop, m))
        print('%4d,%4d %3dx%-3d -> ncc %6.3f  @(%3d,%3d)'
              % (x, y, w, h, r[yy, xx], xx, yy))
    # Komposition auf registrierten BG
    bg = np.fromfile(BG, dtype=np.uint8).reshape(240, 400, 3).astype(np.float64)
    for sc, ox, oy, w, h, sx, sy, crop, m in rows:
        if sc < 0.35:
            continue
        ch, cw = crop.shape[:2]
        x0, y0 = max(0, sx), max(0, sy)
        x1, y1 = min(400, sx + cw), min(240, sy + ch)
        if x1 <= x0 or y1 <= y0:
            continue
        sub = bg[y0:y1, x0:x1]
        cs = crop[y0 - sy:y1 - sy, x0 - sx:x1 - sx]
        ms = m[y0 - sy:y1 - sy, x0 - sx:x1 - sx][..., None]
        bg[y0:y1, x0:x1] = sub * (1 - ms) + cs[..., :3] * ms
    open(OUT, 'wb').write(bg.astype(np.uint8).tobytes())
    rg = np.zeros((240, 400), np.float64)
    rg[:] = ref.mean(2)
    print('corr komponiert %.3f -> %s'
          % (np.corrcoef(rg.ravel(), bg.mean(2).ravel())[0, 1], OUT))


if __name__ == '__main__':
    main()
