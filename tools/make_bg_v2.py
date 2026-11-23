"""BG v2: 3D-Render (0.426) + registriertes Spiel-Asset (0.435) mischen.

Gemessen: Blend 70/30 liefert corr 0.455 (besser als beide Einzelbilder).
Schreibt `tools/profiles/title_bg_v2.bin` (400x240 RGB).

Aufruf: python tools/make_bg_v2.py
"""
import os

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, 'reports/menu_official.bin')
A = os.path.join(ROOT, 'out/title3d_render.png')
B = os.path.join(ROOT, 'tools/profiles/title_bg_registered.bin')
OUT = os.path.join(ROOT, 'tools/profiles/title_bg_v2.bin')
ALPHA = 0.65


def main():
    ref = np.fromfile(REF, dtype=np.uint8).reshape(240, 400, 3).astype(np.float64)
    a = np.asarray(Image.open(A).convert('RGB'), dtype=np.float64)
    b = np.frombuffer(open(B, 'rb').read(), dtype=np.uint8).reshape(240, 400, 3)
    b = b.astype(np.float64)
    out = np.clip(ALPHA * a + (1.0 - ALPHA) * b, 0, 255)
    open(OUT, 'wb').write(out.astype(np.uint8).tobytes())
    c = float(np.corrcoef(ref.mean(2).ravel(), out.mean(2).ravel())[0, 1])
    print('BG v2 alpha=%.2f corr %.4f -> %s' % (ALPHA, c, OUT))


if __name__ == '__main__':
    main()
