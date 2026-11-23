import struct
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_botscan as BS

CAP = BS.CAP
d = open('out/romfs/lt6/arc/lt6_b.fa', 'rb').read()
img = BS.decode_linear(d, 216281204).astype(np.float64)
b = CAP.astype(np.float64)
out = np.zeros_like(img)
for c in range(3):
    x = img[:223, :, c].ravel()
    y = b[:223, :, c].ravel()
    co = np.polyfit(x, y, 1)
    out[:, :, c] = img[:, :, c] * co[0] + co[1]
    print('ch %d: cap = %.4f*asset + %.2f' % (c, co[0], co[1]))
out = np.clip(out, 0, 255)
m = np.abs(out[:223].astype(np.int16) - CAP[:223].astype(np.int16))
print('nach Affinfit: mean %.2f diff>60 %d/%d (%.1f%%)'
      % (m.mean(), int((m.sum(2) > 60).sum()), 223 * 320,
         (m.sum(2) > 60).mean() * 100))
cs = [float(np.corrcoef(out[:223, :, c].ravel(), b[:223, :, c].ravel())[0, 1])
      for c in range(3)]
print('corr', ' '.join('%.4f' % c for c in cs))
# Gamma-Alternative
for g in (0.8, 0.9, 1.0, 1.1, 1.2):
    o2 = 255.0 * np.power(np.clip(img / 255.0, 0, 1), g)
    m2 = np.abs(o2[:223].astype(np.int16) - CAP[:223].astype(np.int16))
    print('gamma %.1f mean %.2f' % (g, m2.mean()))
