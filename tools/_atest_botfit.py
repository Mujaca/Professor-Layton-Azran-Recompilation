import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_imgc_np as D

B = open('out/romfs/lt6/arc/lt6_b.fa', 'rb').read()
w, h, rgb, al = D.decode_buf(B, 0xCE43074)
cap = np.frombuffer(open('reports/menu_official_bottom.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 320, 3)
vis = 223
a = rgb[:vis].astype(np.float64)
b = cap[:vis].astype(np.float64)
for c, nm in enumerate('RGB'):
    x = a[:, :, c].ravel()
    y = b[:, :, c].ravel()
    A = np.stack([x, np.ones_like(x)], 1)
    coef, *_ = np.linalg.lstsq(A, y, rcond=None)
    pred = A @ coef
    resid = y - pred
    print('%s: cap = %.4f*asset + %.2f  corr=%.4f  resid_std=%.2f'
          % (nm, coef[0], coef[1], float(np.corrcoef(x, y)[0, 1]),
             float(resid.std())))
# Vergleich: identitaet
print('identity mean abs diff', float(np.abs(a - b).mean()))
print('capture mean', b.mean(axis=(0, 1)), 'asset mean', a.mean(axis=(0, 1)))
# 2D-Histogramm-Streuung fuer einen Kanal
x = a[:, :, 1].ravel()
y = b[:, :, 1].ravel()
for lo in range(0, 256, 32):
    m = (x >= lo) & (x < lo + 32)
    if m.sum():
        print('G asset %3d-%3d: n=%6d cap_mean=%.1f' %
              (lo, lo + 31, int(m.sum()), float(y[m].mean())))
