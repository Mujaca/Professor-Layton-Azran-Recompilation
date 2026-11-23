"""Welche ANMC-Menue-Rects haben im Capture wirklich Inhalt?

Misst Mittel/Streuung und den Anteil heller Pixel (Text) je Rect aus
`tools/out/anmc_placements.txt`. Nur Rects mit Text-Anteil werden als
Font-Zeilen gerendert.

Aufruf: python tools/menu_rect_probe.py
"""
import os
import re

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLACE = os.path.join(ROOT, 'tools/out/anmc_placements.txt')
REF = os.path.join(ROOT, 'reports/menu_official.bin')
PAT = re.compile(r'quad\s+(\d+)\s+->\s+(\S+)\s+@\(\s*(-?\d+),\s*(-?\d+)\)\s+(\d+)x(\d+)')


def main():
    ref = np.fromfile(REF, dtype=np.uint8).reshape(240, 400, 3).astype(np.float64)
    g = ref.mean(2)
    print('%5s %-14s %-12s %6s %6s %6s' % ('quad', 'name', 'rect', 'mean', 'std', 'bright%'))
    for line in open(PLACE, encoding='utf-16'):
        m = PAT.search(line)
        if not m:
            continue
        qi, name, x, y, w, h = (int(m.group(1)), m.group(2), int(m.group(3)),
                                int(m.group(4)), int(m.group(5)), int(m.group(6)))
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(400, x + w), min(240, y + h)
        if x1 <= x0 or y1 <= y0:
            print('%5d %-14s %-12s  (ausserhalb)' % (qi, name, '%d,%d %dx%d' % (x, y, w, h)))
            continue
        r = g[y0:y1, x0:x1]
        thr = g.mean() + g.std()
        print('%5d %-14s %-12s %6.1f %6.1f %6.1f'
              % (qi, name, '%d,%d %dx%d' % (x, y, w, h), r.mean(), r.std(),
                 100.0 * (r > thr).mean()))


if __name__ == '__main__':
    main()
