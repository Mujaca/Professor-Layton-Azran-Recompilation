import numpy as np

R = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                  dtype=np.uint8).reshape(240, 400, 3)[:226].astype(np.int16)
h, w = R.shape[:2]
best = None
for y in range(0, h - 16, 4):
    for x in range(0, w - 16, 4):
        p = R[y:y+16, x:x+16]
        v = p.std()
        if best is None or v < best[0]:
            best = (v, x, y)
v, x, y = best
p = R[y:y+16, x:x+16]
print('quiet block', x, y, 'std', round(float(v), 2))
print('unique colors', len(np.unique(p.reshape(-1, 3), axis=0)))
dc = np.abs(R[:, 1:] - R[:, :-1]).mean(axis=(0, 2))
dr = np.abs(R[1:] - R[:-1]).mean(axis=(1, 2))
print('col edge x%8==7', round(float(dc[7::8].mean()), 2),
      'other', round(float(np.delete(dc, np.arange(7, w-1, 8)).mean()), 2))
print('row edge y%8==7', round(float(dr[7::8].mean()), 2),
      'other', round(float(np.delete(dr, np.arange(7, h-1, 8)).mean()), 2))
