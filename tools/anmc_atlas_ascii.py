"""ASCII-Dump der Atlas-Text-Rects (Alpha) zum Vergleich mit dem Capture."""
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _atest_xpck as X
import _atest_imgc_np as D
import _imgc

FA = os.path.join(ROOT, 'out/romfs/lt6/arc/lt6_uk.fa')
XA_OFF, XA_LEN = 0x01526DC0, 0x9278
RECTS = [(0, 0, 105, 22), (0, 24, 105, 46), (0, 48, 103, 70),
         (0, 72, 103, 94), (0, 96, 99, 118), (0, 120, 99, 142),
         (0, 144, 150, 168), (0, 168, 150, 192), (0, 192, 144, 216),
         (136, 168, 160, 174)]


def main():
    d = open(FA, 'rb').read()
    cnt, ents = X.parse(d, XA_OFF, XA_LEN, _imgc.decomp)
    e = [x for x in ents if x['name'] == '000.xi'][0]
    o = XA_OFF + e['off']
    w, h, rgb, a = D.decode_buf(d, o)
    aa = a if a is not None else np.full((h, w), 255, np.uint8)
    for (u0, v0, u1, v1) in RECTS:
        sub = aa[v0:v1, u0:u1]
        print('\n=== UV (%d,%d)-(%d,%d)  alpha stats min/max/mean %.0f/%.0f/%.1f'
              % (u0, v0, u1, v1, sub.min(), sub.max(), sub.mean()))
        for y in range(sub.shape[0]):
            print(''.join('#' if v > 200 else ('+' if v > 60 else
                                               ('.' if v > 0 else ' '))
                          for v in sub[y]))
        # RGB of alpha>200
        m = sub > 200
        if m.any():
            c = rgb[v0:v1, u0:u1][m].mean(axis=0)
            print('RGB(a>200) mean (%.0f,%.0f,%.0f) n=%d'
                  % (c[0], c[1], c[2], int(m.sum())))


if __name__ == '__main__':
    main()
