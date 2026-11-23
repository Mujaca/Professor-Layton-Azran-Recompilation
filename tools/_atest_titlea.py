import struct
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_xpck as X
import _imgc
import _atest_imgc_np as D

d = open('out/romfs/lt6/arc/lt6_uk.fa', 'rb').read()
OFF = 0x01428314
REF = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 400, 3).astype(np.int16)
cnt, ents = X.parse(d, OFF, 0xFEAA8, _imgc.decomp)
for e in ents:
    if e['magic'] != 'IMGC00':
        continue
    o = OFF + e['off']
    r = D.decode_buf(d, o)
    if r is None:
        print(e['name'], 'decode fail')
        continue
    w, h, rgb, al = r
    print('%-8s %4dx%-4d alpha: mean %.1f  frac0 %.2f  frac255 %.2f  rgbmean %s'
          % (e['name'], w, h, al.mean(), (al == 0).mean(),
             (al == 255).mean(), np.round(rgb.mean(axis=(0, 1)), 1)))
