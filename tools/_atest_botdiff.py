import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_botscan as BS

CAP = BS.CAP
d = open('out/romfs/lt6/arc/lt6_b.fa', 'rb').read()
img = BS.decode_linear(d, 216281204)
m = np.abs(img.astype(np.int16) - CAP.astype(np.int16)).sum(2)
print('mean', m.mean(), 'median', np.median(m))
print('pct diff<=15', (m <= 15).mean() * 100, '<=30', (m <= 30).mean() * 100)
# pro 16x16-Tile
th, tw = 240 // 16, 320 // 16
mt = m[:th * 16, :tw * 16].reshape(th, 16, tw, 16).mean(axis=(1, 3))
print('tile means: min %.1f median %.1f max %.1f'
      % (mt.min(), np.median(mt), mt.max()))
print('tiles with mean<=10:', int((mt <= 10).sum()), 'of', mt.size)
print('tiles with mean>=40:', int((mt >= 40).sum()))
# bester 16x16-Block
k = np.unravel_index(np.argmin(mt), mt.shape)
print('best tile at row', k[0] * 16, 'col', k[1] * 16, 'mean %.2f' % mt[k])
# Kanalmittel des besten Tiles
y, x = k[0] * 16, k[1] * 16
a = img[y:y + 16, x:x + 16].astype(int)
b = CAP[y:y + 16, x:x + 16].astype(int)
print('asset mean', a.mean(axis=(0, 1)), 'cap mean', b.mean(axis=(0, 1)))
