"""Tile-Matching: 16x16-Kacheln der Referenz gegen alle Atlas-Patches."""
import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, 'tools')
import _atest_imgc_np as D

FAS = ['out/romfs/lt6/arc/lt6_b.fa',
       'out/romfs/lt6/arc/lt6_uk.fa',
       'out/romfs/lt6/arc/lt6_a.fa']
REF = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 400, 3)
TS = 16
STEPA = 8
TH = TW = TS
DIM = TS * TS * 3


def patches(img, step):
    h, w = img.shape[:2]
    ys = np.arange(0, h - TH + 1, step)
    xs = np.arange(0, w - TW + 1, step)
    if len(ys) == 0 or len(xs) == 0:
        return None, None, None
    yy = ys[:, None] + np.arange(TH)[None, :]
    xx = xs[:, None] + np.arange(TW)[None, :]
    a = img[yy[:, None, :, None], xx[None, :, None, :]]
    a = a.reshape(-1, DIM).astype(np.float32)
    gy, gx = np.meshgrid(ys, xs, indexing='ij')
    return a, gx.ravel(), gy.ravel()


def normalize(a):
    m = a.mean(axis=1, keepdims=True)
    s = a.std(axis=1, keepdims=True)
    s = np.where(s < 1e-3, 1e-3, s)
    return (a - m) / s


def main():
    tiles = [(x, y) for y in range(0, 240, TS) for x in range(0, 400, TS)]
    rt = np.stack([REF[y:y + TH, x:x + TW] for (x, y) in tiles])
    RT = normalize(rt.reshape(len(tiles), DIM).astype(np.float32))
    n = len(tiles)
    best_ncc = np.full(n, -2.0, dtype=np.float32)
    best_src = np.zeros(n, dtype=np.int32)
    best_u = np.zeros(n, dtype=np.int32)
    best_v = np.zeros(n, dtype=np.int32)
    srcs = []
    t0 = time.time()
    for fa in FAS:
        d = open(fa, 'rb').read()
        i = 0
        while True:
            o = d.find(b'IMGC00', i)
            if o < 0:
                break
            i = o + 1
            hd = D.parse_header(d, o)
            if not hd or hd['bits'] not in (24, 32):
                continue
            try:
                r = D.decode_buf(d, o)
            except Exception:
                continue
            if r is None:
                continue
            w, h, rgb, al = r
            a, gx, gy = patches(rgb, STEPA)
            if a is None:
                continue
            A = normalize(a)
            sim = A @ RT.T
            k = np.argmax(sim, axis=0)
            v = sim[k, np.arange(n)]
            srcs.append((fa, o, w, h, hd['bits'], len(k)))
            idx = len(srcs) - 1
            upd = v > best_ncc
            best_ncc[upd] = v[upd]
            best_src[upd] = idx
            best_u[upd] = gx[k][upd]
            best_v[upd] = gy[k][upd]
    el = time.time() - t0
    print('atlanten', len(srcs), 'kacheln', n, 'dauer %.1fs' % el)
    rows = []
    for j in range(n):
        fa, o, w, h, bits, npatch = srcs[best_src[j]]
        rows.append(dict(tile=[int(tiles[j][0]), int(tiles[j][1])],
                         ncc=float(best_ncc[j]),
                         atlas='%s@%d' % (os.path.basename(fa), o),
                         u=int(best_u[j]), v=int(best_v[j]),
                         size=[w, h], bits=bits))
    json.dump(rows, open('tools/out/atest_tiles.json', 'w'), indent=1)
    ncc = np.array([r['ncc'] for r in rows])
    for thr in (0.95, 0.9, 0.8, 0.7, 0.6):
        print('NCC>=%.2f: %d/%d Kacheln (%.1f%%)'
              % (thr, int((ncc >= thr).sum()), n, (ncc >= thr).mean() * 100))
    print('NCC min/median/max: %.3f %.3f %.3f'
          % (ncc.min(), float(np.median(ncc)), ncc.max()))
    agg = {}
    for r in rows:
        agg.setdefault(r['atlas'], []).append(r['ncc'])
    top = sorted(agg.items(), key=lambda kv: -len(kv[1]))
    print('--- Top-Atlanten (Kacheln als bester Treffer) ---')
    for k, v in top[:12]:
        print('%-34s %3d Kacheln  meanNCC %.3f'
              % (k, len(v), float(np.mean(v))))
    # Komposition
    comp = np.zeros((240, 400, 3), dtype=np.uint8)
    for j, r in enumerate(rows):
        x, y = r['tile']
        fa, o = r['atlas'].split('@')
        comp[y:y + TH, x:x + TW] = REF[y:y + TH, x:x + TW]  # Platzhalter
    print('Hinweis: Komposition separat in _atest_compose.py')


if __name__ == '__main__':
    main()
