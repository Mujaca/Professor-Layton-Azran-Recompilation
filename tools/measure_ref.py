"""Vergleicht einen 400x480-Portshot mit den offiziellen Azahar-Captures.

Die Captures sind Fensterfotos und enthalten nur einen Teil der Screens:
  Top    : 400x226 sichtbar (14 Zeilen fehlen, schwarz)
  Bottom : 320x223 sichtbar (17 Zeilen fehlen, schwarz)
Gemessen wird darum (a) nur der sichtbare Bereich und (b) der volle Screen.

Aufruf: python tools\\measure_ref.py out\\shot.bmp
"""
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _cmp_shot

TOP_VIS = 226
BOT_VIS = 223


def main():
    shot = sys.argv[1] if len(sys.argv) > 1 else 'out/shot.bmp'
    w, h, px = _cmp_shot.load_bmp(shot)
    if (w, h) != (400, 480):
        print('kein 400x480-Shot:', w, h)
        return 1
    a = np.array(px, dtype=int).reshape(h, w, 3)

    top_ref = np.frombuffer(open(os.path.join(
        ROOT, 'reports', 'menu_official.bin'), 'rb').read(),
        np.uint8).reshape(240, 400, 3).astype(int)
    bot_ref = np.frombuffer(open(os.path.join(
        ROOT, 'reports', 'menu_official_bottom.bin'), 'rb').read(),
        np.uint8).reshape(240, 320, 3).astype(int)

    top = a[0:240, 0:400]
    bot = a[240:480, 40:360]
    for name, img, ref, vis in (('TOP', top, top_ref, TOP_VIS),
                                ('BOTTOM', bot, bot_ref, BOT_VIS)):
        d = np.abs(img - ref).sum(axis=2)
        print('%s sichtbar : diff>60 %6d/%6d   mean %.2f'
              % (name, int((d[:vis] > 60).sum()), vis * ref.shape[1],
                 float(np.abs(img[:vis] - ref[:vis]).mean())))
        print('%s voll     : diff>60 %6d/%6d   mean %.2f'
              % (name, int((d > 60).sum()), 240 * ref.shape[1],
                 float(np.abs(img - ref).mean())))
    return 0


if __name__ == '__main__':
    sys.exit(main())
