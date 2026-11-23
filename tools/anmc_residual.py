"""Residual Capture - title_b_bg: wo liegen die Overlays? (Analyse)"""
import numpy as np

cap = np.frombuffer(open('reports/menu_official_bottom.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 320, 3).astype(np.int16)
bg = np.frombuffer(open('out/title_b_bg.bin', 'rb').read(),
                   dtype=np.uint8).reshape(240, 320, 3).astype(np.int16)
d = np.abs(cap - bg).max(axis=2)
print('diff>30 pixels: %d/76800  diff>60: %d mean %.2f'
      % (int((d > 30).sum()), int((d > 60).sum()), d.mean()))
chars = ' .:-=+*#%@'
print('4x4-Bloecke: max diff (0..9 = 0..225)')
for by in range(0, 240, 4):
    row = ''
    for bx in range(0, 320, 4):
        v = d[by:by + 4, bx:bx + 4].max()
        row += chars[min(9, int(v / 25.6))]
    print('%3d|%s' % (by, row))
# connected-ish row bands with d>40
rowmask = (d > 40).sum(axis=1)
print('\nrow bands d>40 (count>2):')
y = 0
while y < 240:
    if rowmask[y] > 2:
        y0 = y
        while y < 240 and rowmask[y] > 2:
            y += 1
        xs = np.where((d[y0:y] > 40).any(axis=0))[0]
        print('y=%3d..%3d  x=%3d..%3d  max=%d'
              % (y0, y - 1, xs.min(), xs.max(),
                 int(d[y0:y].max())))
    else:
        y += 1
