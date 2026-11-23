import struct
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_botscan as BS

CAP = BS.CAP
d = open('out/romfs/lt6/arc/lt6_b.fa', 'rb').read()
img = BS.decode_linear(d, 216281204)
print('decoded', img.shape)
best = []
for dy in range(-3, 4):
    for dx in range(-3, 4):
        a = np.roll(np.roll(img, dy, axis=0), dx, axis=1)[:223]
        b = CAP[:223]
        m = np.abs(a.astype(np.int16) - b.astype(np.int16))
        best.append((int((m.sum(2) > 60).sum()), float(m.mean()), dx, dy))
best.sort()
for r in best[:6]:
    print('diff %d mean %.2f shift %d,%d' % r)
