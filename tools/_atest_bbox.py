"""Bounding-Boxen der Alpha-Kanaele in title_a.xa-Texturen."""
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_imgc_np as D

UK = open('out/romfs/lt6/arc/lt6_uk.fa', 'rb').read()
OFF = 0x01428314
for name, rel in (('000.xi', 0x27C), ('001.xi', 0x463C), ('002.xi', 0x4733C),
                  ('003.xi', 0xC2D28), ('004.xi', 0xEF340),
                  ('005.xi', None)):
    if rel is None:
        continue
    r = D.decode_buf(UK, OFF + rel)
    if r is None:
        print(name, 'fail')
        continue
    w, h, rgb, al = r
    m = al > 8
    if m.any():
        ys, xs = np.nonzero(m)
        print('%s %4dx%-4d alpha>8: %d px, bbox x %d..%d y %d..%d'
              % (name, w, h, int(m.sum()), xs.min(), xs.max(),
                 ys.min(), ys.max()))
    else:
        print('%s %4dx%-4d kein Alpha' % (name, w, h))
