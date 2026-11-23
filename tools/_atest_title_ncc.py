"""NCC-Tilematching der 14 4bpp-Titel-Texturen (menu/title.xc) gegen den Top-Capture."""
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_imgc_np as D
import _atest_titlexc as T

REF = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 400, 3)
RL = REF.astype(np.float32).mean(axis=2)
TS = 16
STEP = 8


def main():
    tiles = [(x, y) for y in range(0, 240, TS) for x in range(0, 400, TS)]
    rt = np.stack([RL[y:y + TS, x:x + TS] for (x, y) in tiles])
    RT = rt.reshape(len(tiles), -1)
    RT = (RT - RT.mean(1, keepdims=True)) / (RT.std(1, keepdims=True) + 1e-3)
    best = np.full(len(tiles), -2.0, dtype=np.float32)
    info = [None] * len(tiles)
    for (i, h, name, off, size, mg, inf) in T.ents:
        if mg != b'IMGC00':
            continue
        r = D.decode_buf(T.d, off)
        if r is None:
            continue
        w, hh = r[0], r[1]
        img = r[2].astype(np.float32).mean(axis=2)
        ys = np.arange(0, hh - TS + 1, STEP)
        xs = np.arange(0, w - TS + 1, STEP)
        yy = ys[:, None] + np.arange(TS)[None, :]
        xx = xs[:, None] + np.arange(TS)[None, :]
        a = img[yy[:, None, :, None], xx[None, :, None, :]]
        a = a.reshape(-1, TS * TS)
        A = (a - a.mean(1, keepdims=True)) / (a.std(1, keepdims=True) + 1e-3)
        gy, gx = np.meshgrid(ys, xs, indexing='ij')
        sim = A @ RT.T
        k = np.argmax(sim, axis=0)
        v = sim[k, np.arange(len(tiles))]
        upd = v > best
        best[upd] = v[upd]
        for j in np.nonzero(upd)[0]:
            info[j] = (name, int(gx.ravel()[k[j]]), int(gy.ravel()[k[j]]))
    print('NCC-Verteilung: min %.3f median %.3f max %.3f'
          % (best.min(), np.median(best), best.max()))
    for thr in (0.95, 0.9, 0.85, 0.8, 0.7):
        print('NCC>=%.2f: %d/%d (%.1f%%)'
              % (thr, int((best >= thr).sum()), len(tiles),
                 (best >= thr).mean() * 100))
    import collections
    c = collections.Counter(i[0] if i else None for i in info)
    print('bester Atlas je Kachel:', c.most_common(8))


if __name__ == '__main__':
    main()
