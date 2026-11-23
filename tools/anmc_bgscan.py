"""ASCII-Vorschau von out/title_b_bg.bin + Pixelstatistik Referenz-Textzeile."""
import numpy as np

bg = np.frombuffer(open('out/title_b_bg.bin', 'rb').read(),
                   dtype=np.uint8).reshape(240, 320, 3)
cap = np.frombuffer(open('reports/menu_official_bottom.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 320, 3)
lum = bg.mean(axis=2)
chars = ' .:-=+*#%@'
print('BG luma min/max/mean %.1f %.1f %.2f' % (lum.min(), lum.max(), lum.mean()))
for by in range(0, 240, 4):
    row = ''
    for bx in range(0, 320, 4):
        v = lum[by:by + 4, bx:bx + 4].mean()
        row += chars[min(9, int(v / 25.6))]
    print('%3d|%s' % (by, row))
print('\nBG rows with bright>3 (lum>180):')
b = lum > 180
for y in range(240):
    n = int(b[y].sum())
    if n > 3:
        xs = np.where(b[y])[0]
        print('y=%3d n=%3d x=%d..%d' % (y, n, xs.min(), xs.max()))
print('\nCapture text region x=100..240,y=104..128:')
sub = cap[104:128, 100:240]
sl = sub.mean(axis=2)
for yy in range(sl.shape[0]):
    print('%3d|' % (104 + yy) + ''.join(chars[min(9, int(v / 25.6))]
                                        for v in sl[yy]))
print('\nRGB at brightest capture pixels:')
order = np.argsort(cap.mean(axis=2).ravel())[-8:]
idx = np.unravel_index(order, (240, 320))
for y, x in zip(*idx):
    print('(%d,%d) %s' % (x, y, cap[y, x]))
