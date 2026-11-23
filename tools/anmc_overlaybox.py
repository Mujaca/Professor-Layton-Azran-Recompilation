"""Exakte Bounding-Box des Capture-Overlays um die Textzeile."""
import numpy as np

cap = np.frombuffer(open('reports/menu_official_bottom.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 320, 3).astype(np.int16)
bg = np.frombuffer(open('out/title_b_bg.bin', 'rb').read(),
                   dtype=np.uint8).reshape(240, 320, 3).astype(np.int16)
d = np.abs(cap - bg).max(axis=2)
# Region um die Zeile
for th in (30, 40, 60, 100):
    m = d > th
    print('\n--- threshold', th)
    for y in range(100, 136):
        xs = np.where(m[y])[0]
        if len(xs):
            print('y=%3d n=%3d x=%3d..%3d' % (y, len(xs), xs.min(), xs.max()))
# vertikale/horizontale Kanten im Bereich x=100..240
sub = d[100:140, 90:250]
colsum = (sub > 40).sum(axis=0)
rowsum = (sub > 40).sum(axis=1)
print('\ncols>40 count (x=90..249):')
print(' '.join('%d:%d' % (90 + i, c) for i, c in enumerate(colsum) if c > 0))
print('\nrows>40 count (y=100..139):')
print(' '.join('%d:%d' % (100 + i, c) for i, c in enumerate(rowsum) if c > 0))
# Mittelwert capture/bg in Kandidatenrechteck
for (x0, y0, w, h, name) in [(108, 108, 103, 24, 'btr_continue'),
                             (99, 86, 103, 24, 'btr01_continue'),
                             (107, 108, 105, 24, 's_btr_new'),
                             (107, 54, 105, 24, 'btr_new'),
                             (101, 86, 105, 24, 's01_btr_new'),
                             (110, 162, 99, 24, 'btr_scr'),
                             (83, 132, 99, 24, 'btr01_scr'),
                             (84, 178, 150, 24, 'btr01_setting'),
                             (32, 88, 256, 66, 'try_off'),
                             (32, 90, 256, 64, 'try_on'),
                             (90, 204, 144, 24, 'try_www'),
                             (296, -2, 24, 26, 'sound1'),
                             (139, 117, 24, 6, 'pointer')]:
    ys, ye = max(0, y0), min(240, y0 + h)
    xs, xe = max(0, x0), min(320, x0 + w)
    print('%-14s (%3d,%3d,%3d,%3d) cap_mean=%6.1f bg_mean=%6.1f '
          'diff>40=%5d'
          % (name, x0, y0, w, h, cap[ys:ye, xs:xe].mean(),
             bg[ys:ye, xs:xe].mean(),
             int((d[ys:ye, xs:xe] > 40).sum())))
