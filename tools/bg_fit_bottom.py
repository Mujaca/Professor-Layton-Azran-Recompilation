"""title_b_bg als Top-Hintergrund registrieren (weite Suche) + speichern.

Registriert das Spiel-Asset `out/title_b_bg.bin` (320x240) gegen den
Capture und schreibt das beste Ergebnis nach
`tools/profiles/title_bg_registered.bin` (400x240 RGB).

Aufruf: python tools/bg_fit_bottom.py
"""
import os

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, 'reports/menu_official.bin')
SRC = os.path.join(ROOT, 'out/title_b_bg.bin')
DST = os.path.join(ROOT, 'tools/profiles/title_bg_registered.bin')


def main():
    ref = np.fromfile(REF, dtype=np.uint8).reshape(240, 400, 3).astype(np.float32)
    rg = ref.mean(2)
    src = Image.frombytes('RGB', (320, 240), open(SRC, 'rb').read())
    best = (-2.0, None)
    for sx in np.arange(0.8, 1.61, 0.05):
        for sy in np.arange(0.8, 1.61, 0.05):
            w, h = int(320 * sx), int(240 * sy)
            im = src.resize((w, h))
            a = np.asarray(im, dtype=np.float32).mean(2)
            for dx in range(-w + 40, 401, 16):
                for dy in range(-h + 40, 241, 12):
                    canvas = np.zeros((240, 400), np.float32)
                    x0, y0 = max(0, dx), max(0, dy)
                    sx0, sy0 = max(0, -dx), max(0, -dy)
                    ww = min(400 - x0, w - sx0)
                    hh = min(240 - y0, h - sy0)
                    if ww <= 0 or hh <= 0:
                        continue
                    canvas[y0:y0 + hh, x0:x0 + ww] = a[sy0:sy0 + hh, sx0:sx0 + ww]
                    c = float(np.corrcoef(rg.ravel(), canvas.ravel())[0, 1])
                    if c > best[0]:
                        best = (c, (float(sx), float(sy), dx, dy))
    c, (sx, sy, dx, dy) = best
    print('best corr %.4f  sx=%.2f sy=%.2f dx=%d dy=%d' % (c, sx, sy, dx, dy))
    w, h = int(320 * sx), int(240 * sy)
    im = src.resize((w, h))
    canvas = Image.new('RGB', (400, 240), (0, 0, 0))
    canvas.paste(im, (dx, dy))
    open(DST, 'wb').write(canvas.tobytes())
    print('->', DST)


if __name__ == '__main__':
    main()
