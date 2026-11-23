"""ASCII-Vorschau des Bottom-Captures + Luma-Profile (Analyse)."""
import numpy as np

CAP = np.frombuffer(open('reports/menu_official_bottom.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 320, 3)
lum = CAP.mean(axis=2)
print('capture min/max/mean %.1f %.1f %.2f' % (lum.min(), lum.max(), lum.mean()))
# 4x4 block ASCII
chars = ' .:-=+*#%@'
for by in range(0, 240, 4):
    row = ''
    for bx in range(0, 320, 4):
        v = lum[by:by + 4, bx:bx + 4].mean()
        row += chars[min(9, int(v / 25.6))]
    print('%3d|%s' % (by, row))
print('     ' + ''.join(str((x // 4 // 10) % 10) for x in range(0, 320, 4)))
print('     ' + ''.join(str((x // 4) % 10) for x in range(0, 320, 4)))
# row profile of bright pixels
b = (lum > 180)
print('\nrows with bright>3:')
for y in range(240):
    n = int(b[y].sum())
    if n > 3:
        xs = np.where(b[y])[0]
        print('y=%3d n=%3d x=%d..%d' % (y, n, xs.min(), xs.max()))
