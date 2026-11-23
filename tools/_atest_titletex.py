"""Dekodiert die 4bpp-.xi aus menu/title.xc (als Luminanz)."""
import struct
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_imgc_np as D
import _atest_titlexc as T

for name, o, size in (( '000.xi', 0x00000E0C, 109356),
                      ('002.xi', 0x00029570, 113676),
                      ('004.xi', 0x00052E7C, 117116)):
    r = D.decode_buf(T.d, o)
    print(name, 'decode', None if r is None else (r[0], r[1]))
    if r is None:
        continue
    w, h, rgb, al = r
    g = rgb[:, :, 0].astype(np.int32) // 17
    print('  nibble uniq', np.unique(g), 'mean', round(float(g.mean()), 2),
          'std', round(float(g.std()), 2))
    u, c = np.unique(g, return_counts=True)
    print('  hist', dict(zip(u.tolist(),
                              (c / g.size * 100).round(1).tolist())))
    from PIL import Image
    Image.fromarray(rgb).save('tools/out/atest_%s.png' % name.replace('.', '_'))
