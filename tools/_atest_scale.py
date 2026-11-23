import struct
import sys

import numpy as np
from PIL import Image

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
for k in range(min(len(E), (w // 8) * (h // 8))):
    e = int(E[k])
    if 0 <= e < NT:
        ty, tx = divmod(k, cols)
        img[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8] = SRC[e]

for H in (240, 223):
    im = Image.fromarray(img).resize((320, H), Image.BILINEAR)
    a = np.array(im).astype(np.float64)
    b = CAP[:H].astype(np.float64)
    cs = [float(np.corrcoef(a[:, :, c].ravel(), b[:, :, c].ravel())[0, 1])
          for c in range(3)]
    print('resize %dx%d: corr %s mean %.2f'
          % (320, H, ' '.join('%.3f' % c for c in cs),
             float(np.abs(a - b).mean())))
# ohne Resize
a = img.astype(np.float64)
b = CAP[:223].astype(np.float64)
cs = [float(np.corrcoef(a[:223, :, c].ravel(), b[:, :, c].ravel())[0, 1])
      for c in range(3)]
print('no resize 223 rows: corr', ' '.join('%.3f' % c for c in cs))
# Kanal-Fit ohne Resize
for c in range(3):
    x = a[:223, :, c].ravel()
    y = b[:, :, c].ravel()
    co = np.polyfit(x, y, 1)
    print('channel fit %d: cap = %.3f*asset + %.2f' % (c, co[0], co[1]))
