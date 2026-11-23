"""Ehrlicher Bildvergleich: Referenz-Capture vs. Port-Screenshot.

Die bisherige Messung (`tools/measure_ref.py`) zaehlt nur Pixel mit
Farbdifferenz > 60 und ist fuer "sieht aus wie das Menue" blind: ein
einfarbiger Farbverlauf kann dort bessere Werte erzielen als das echte
Menue. Dieses Tool misst drei Dinge, die Struktur pruefen:

  corr        Korrelation der Helligkeit pro Pixel (Layout-Treffer)
  std         mittlere lokale Standardabweichung in 25x20-Bloecken
              (Detailreichtum; Referenz ~48)
  detail-hit  Anteil der Bloecke, in denen die Referenz Detail hat und
              das Port-Bild ebenfalls (Textur-/Kanten-Treffer)

Aufruf: python tools/menu_compare.py out/shot_engine.bmp
"""
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, 'reports/menu_official.bin')
W, H = 400, 240


def load_ref():
    return np.fromfile(REF, dtype=np.uint8).reshape(H, W, 3).astype(np.float32)


def load_shot(p):
    im = Image.open(p).convert('RGB')
    a = np.asarray(im, dtype=np.float32)
    return a[:H, :W]


def block_std(a, w=25, h=20):
    v = []
    for by in range(0, H, h):
        for bx in range(0, W, w):
            v.append(a[by:by + h, bx:bx + w].reshape(-1, 3).std())
    return np.array(v)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    ref = load_ref()
    ours = load_shot(sys.argv[1])
    corr = float(np.corrcoef(ref.reshape(-1, 3).mean(1),
                             ours.reshape(-1, 3).mean(1))[0, 1])
    sr, so = block_std(ref), block_std(ours)
    det = sr > 30.0
    hit = float((so[det] > 20.0).mean()) if det.any() else 0.0
    print('corr       : %.3f   (Layout; 1.0 = identisch)' % corr)
    print('std        : ref %.1f  ours %.1f   (Detail)' % (sr.mean(), so.mean()))
    print('detail-hit : %.2f   (%d Bloecke mit Referenz-Detail)' % (hit, int(det.sum())))
    print('mean-RGB   : ref %s  ours %s'
          % (ref.reshape(-1, 3).mean(0).round(1),
             ours.reshape(-1, 3).mean(0).round(1)))


if __name__ == '__main__':
    main()
