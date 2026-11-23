import struct
import sys

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, 'tools')
import _imgc

B = open('out/romfs/lt6/arc/lt6_b.fa', 'rb').read()
OFF = 0xCE43074
CAP = np.frombuffer(open('reports/menu_official_bottom.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 320, 3)

o = OFF
s1, s2, sz = struct.unpack_from('<iii', B, o + 0x34)
w, h = struct.unpack_from('<hh', B, o + 0x10)
bpp = B[o + 0x0D] // 8
bpt = 64 * bpp
TAB = _imgc.decomp(B[o + 0x48:o + 0x48 + s1])
TEX = _imgc.decomp(B[o + 0x48 + s2:o + 0x48 + s2 + sz])
E = np.frombuffer(TAB, dtype='<u2').astype(np.int64)
NT = len(TEX) // bpt
SRC = np.frombuffer(TEX, dtype=np.uint8)[:NT * bpt].reshape(NT, 8, 8, bpp)[:, :, :, ::-1]
img = np.zeros((h, w, 3), dtype=np.uint8)
cols = w // 8
for k in range(min(len(E), cols * (h // 8))):
    e = int(E[k])
    if 0 <= e < NT:
        ty, tx = divmod(k, cols)
        img[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8] = SRC[e]

Bcap = CAP[:220].astype(np.float64)


def corr(a, b):
    return float(np.corrcoef(a.ravel(), b.ravel())[0, 1])


base = img[:220].astype(np.float64)
print('base corr (mean channels)', np.mean([corr(base[:, :, c], Bcap[:, :, c])
                                            for c in range(3)]))
for sig in (0.5, 0.8, 1.0, 1.5, 2.0):
    im = Image.fromarray(img).filter(ImageFilter.GaussianBlur(sig))
    a = np.array(im)[:220].astype(np.float64)
    print('blur %.1f corr %.4f' % (sig, np.mean([corr(a[:, :, c], Bcap[:, :, c])
                                            for c in range(3)])))
