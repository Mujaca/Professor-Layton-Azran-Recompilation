"""Passen die ANMC-Positionen auf das Referenzbild? Transform-Suche.

Die ANMC-Records liefern (x,y) je Menuepunkt in einem eigenen Canvas
(Hauptpunkte bei y=24..26, x=24..256). Dieses Tool sucht die affin
einfachste Abbildung auf das 400x240-Referenzbild, bei der an den
Element-Punkten moeglichst viel Bilddetail liegt (lokale Streuung).

Damit ist messbar, ob (x,y) Bildschirmpositionen sind und mit welchem
Faktor - statt zu raten.

Aufruf: python tools/anmc_fit.py
"""
import os

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PTS = [
    ('continue', 103.0, 24.0), ('new', 105.0, 24.0), ('scr', 99.0, 24.0),
    ('setting', 150.0, 24.0), ('sound1', 24.0, 26.0),
    ('sound2', 24.0, 26.0), ('sound3', 24.0, 26.0),
    ('try', 256.0, 66.0), ('www', 144.0, 24.0),
    ('btr01_continue', 99.0, 86.0), ('btr_continue', 108.0, 108.0),
    ('btr01_setting', 84.0, 178.0), ('try_www', 160.0, 120.0),
]


def load_ref():
    return np.fromfile(os.path.join(ROOT, 'reports/menu_official.bin'),
                       dtype=np.uint8).reshape(240, 400, 3).astype(np.float32)


def detail(a, k=7):
    """Lokale Streuung per Boxfilter (Integralbild)."""
    g = a.mean(2)
    p = np.pad(g, ((k // 2, k // 2), (k // 2, k // 2)), mode='edge')
    ii = p.cumsum(0).cumsum(1)
    ii = np.pad(ii, ((1, 0), (1, 0)))
    s = ii[k:, k:] - ii[:-k, k:] - ii[k:, :-k] + ii[:-k, :-k]
    s2 = (p * p).cumsum(0).cumsum(1)
    s2 = np.pad(s2, ((1, 0), (1, 0)))
    q = s2[k:, k:] - s2[:-k, k:] - s2[k:, :-k] + s2[:-k, :-k]
    n = k * k
    return np.sqrt(np.maximum(q / n - (s / n) ** 2, 0.0))


def score(d, sx, sy, flip, dx, dy):
    v = []
    for _n, x, y in PTS:
        X = int(round(x * sx + dx))
        Y = int(round((240.0 - y * sy - dy) if flip else (y * sy + dy)))
        if 0 <= X < 400 and 0 <= Y < 240:
            v.append(d[Y, X])
        else:
            return -1.0
    return float(np.mean(v))


def main():
    ref = load_ref()
    d = detail(ref)
    print('Referenz-Detail: mean %.2f  p95 %.2f' % (d.mean(), np.percentile(d, 95)))
    print('%6s %6s %5s  %5s %5s  %6s' % ('sx', 'sy', 'flip', 'dx', 'dy', 'score'))
    best = (-1, None)
    for sx in (0.5, 0.75, 1.0, 1.25, 1.5625, 2.0):
        for sy in (0.5, 0.75, 1.0, 1.25, 1.5625, 2.0):
            for flip in (False, True):
                for dx in range(0, 41, 8):
                    for dy in range(0, 41, 8):
                        sc = score(d, sx, sy, flip, dx, dy)
                        if sc > best[0]:
                            best = (sc, (sx, sy, flip, dx, dy))
    print('--- Top-5')
    res = []
    for sx in (0.5, 0.75, 1.0, 1.25, 1.5625, 2.0):
        for sy in (0.5, 0.75, 1.0, 1.25, 1.5625, 2.0):
            for flip in (False, True):
                for dx in range(0, 41, 8):
                    for dy in range(0, 41, 8):
                        sc = score(d, sx, sy, flip, dx, dy)
                        res.append((sc, sx, sy, flip, dx, dy))
    res.sort(key=lambda r: -r[0])
    for sc, sx, sy, flip, dx, dy in res[:5]:
        print('%6.2f %6.2f %5s  %5d %5d  %6.2f' % (sx, sy, flip, dx, dy, sc))
    sc, (sx, sy, flip, dx, dy) = best
    print('\nBest: sx=%.4f sy=%.4f flip=%s dx=%d dy=%d score=%.2f'
          % (sx, sy, flip, dx, dy, sc))
    for n, x, y in PTS:
        X = int(round(x * sx + dx))
        Y = int(round((240.0 - y * sy - dy) if flip else (y * sy + dy)))
        print('  %-15s (%6.1f,%6.1f) -> (%3d,%3d) detail %.1f'
              % (n, x, y, X, Y, d[min(Y, 239), min(X, 399)]))


if __name__ == '__main__':
    main()
