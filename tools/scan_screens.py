"""Scan: alle IMGCs mit 400x240 / 320x240 in den .fa-Archiven gegen die
Captures vergleichen (findet info_a/info_b/title_b_bg & Co.).

  python tools/scan_screens.py
"""
import os
import struct
import sys
import time

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _atest_imgc_np as D

FAS = ['out/romfs/lt6/arc/lt6_a.fa',
       'out/romfs/lt6/arc/lt6_b.fa',
       'out/romfs/lt6/arc/lt6_uk.fa']
TOP = np.frombuffer(open(os.path.join(ROOT, 'reports', 'menu_official.bin'),
                         'rb').read(), np.uint8).reshape(240, 400, 3).astype(int)
BOT = np.frombuffer(open(os.path.join(
    ROOT, 'reports', 'menu_official_bottom.bin'), 'rb').read(),
    np.uint8).reshape(240, 320, 3).astype(int)


def main():
    for rel in FAS:
        p = os.path.join(ROOT, rel)
        d = open(p, 'rb').read()
        t = time.time()
        offs = []
        i = 0
        while True:
            j = d.find(b'IMGC00', i)
            if j < 0:
                break
            if j + 0x18 <= len(d):
                wh = struct.unpack_from('<hh', d, j + 0x10)
                if wh in ((400, 240), (320, 240)):
                    offs.append((j, wh[0], wh[1]))
            i = j + 6
        print('%s (%d B): %d Kandidaten, Scan %.1fs'
              % (rel, len(d), len(offs), time.time() - t))
        res = []
        for j, w, h in offs:
            try:
                r = D.decode_buf(d, j)
            except Exception:
                r = None
            if not r:
                continue
            ww, hh, rgb, al = r
            if (ww, hh) != (w, h):
                continue
            if (w, h) == (400, 240):
                dd = np.abs(rgb.astype(int) - TOP)
            else:
                dd = np.abs(rgb.astype(int) - BOT)
            rows = 226 if w == 400 else 223
            res.append((int((dd.sum(axis=2)[:rows] > 60).sum()), j, w, h,
                        float(dd.mean())))
        res.sort()
        for r in res[:5]:
            tot = (226 * 400) if r[2] == 400 else (223 * 320)
            print('   diff>60 %6d/%6d  @%08X %dx%d mean %.2f'
                  % (r[0], tot, r[1], r[2], r[3], r[4]))


if __name__ == '__main__':
    main()
