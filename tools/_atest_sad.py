"""SAD-Tilematching: 16x16-Referenzkacheln gegen alle 24/32bpp-Atlas-Patches.

Stufe 1: Kandidaten ueber mittlere Farbe (mean RGB, Schwelle).
Stufe 2: Verifikation per SAD. Ausgabe der besten Treffer je Kachel.
"""
import heapq
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
REF = np.frombuffer(open(sys.argv[1] if len(sys.argv) > 1
                         else 'reports/menu_official.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 400, 3)
TS = 16
STEP = 8
TOPK = 3
MEANDIST = 24.0


def patches(img, step):
    h, w = img.shape[:2]
    ys = np.arange(0, h - TS + 1, step)
    xs = np.arange(0, w - TS + 1, step)
    if len(ys) == 0 or len(xs) == 0:
        return None
    yy = ys[:, None] + np.arange(TS)[None, :]
    xx = xs[:, None] + np.arange(TS)[None, :]
    a = img[yy[:, None, :, None], xx[None, :, None, :]]
    a = a.reshape(-1, TS, TS, 3)
    gy, gx = np.meshgrid(ys, xs, indexing='ij')
    return a, gx.ravel(), gy.ravel()


def main():
    tiles = [(x, y) for y in range(0, 240, TS) for x in range(0, 400, TS)]
    rt = np.stack([REF[y:y + TS, x:x + TS] for (x, y) in tiles])
    rt = rt.astype(np.int16)
    rmean = rt.reshape(len(tiles), -1, 3).mean(axis=1)
    heaps = [[] for _ in tiles]
    n = len(tiles)
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
            pr = patches(rgb, STEP)
            if pr is None:
                continue
            a, gx, gy = pr
            pmean = a.reshape(len(a), -1, 3).mean(axis=1)
            md = np.abs(pmean[:, None, :] - rmean[None, :, :]).sum(axis=2)
            srcs.append((fa, o, w, h, hd['bits']))
            idx = len(srcs) - 1
            for j in range(n):
                m = md[:, j]
                cand = np.nonzero(m < MEANDIST)[0]
                if len(cand) == 0:
                    continue
                order = cand[np.argsort(m[cand])[:TOPK]]
                for p in order:
                    dist = float(m[p])
                    item = (-dist, idx, int(gx[p]), int(gy[p]))
                    hp = heaps[j]
                    if len(hp) < TOPK:
                        heapq.heappush(hp, item)
                    elif dist < -hp[0][0]:
                        heapq.heapreplace(hp, item)
    print('scan dauer %.1fs, atlanten %d' % (time.time() - t0, len(srcs)))
    out = []
    for j, (x, y) in enumerate(tiles):
        best = None
        for (negd, idx, u, v) in heaps[j]:
            fa, o, w, h, bits = srcs[idx]
            key = (fa, o)
            rgb = CACHE.get(key)
            if rgb is None:
                rgb = D.decode_buf(open(fa, 'rb').read(), o)[2]
                CACHE[key] = rgb
            patch = rgb[v:v + TS, u:u + TS].astype(np.int16)
            sad = float(np.abs(patch - rt[j]).mean())
            if best is None or sad < best[0]:
                best = (sad, fa, o, u, v, w, h, bits)
        if best is None:
            out.append(dict(tile=[x, y], sad=999.0, atlas=None, u=0, v=0))
        else:
            out.append(dict(tile=[x, y], sad=best[0],
                            atlas='%s@%d' % (os.path.basename(best[1]),
                                             best[2]),
                            u=best[3], v=best[4], size=[best[5], best[6]],
                            bits=best[7]))
    json.dump(out, open('tools/out/atest_tiles_sad%s.json'
                        % ('' if len(sys.argv) <= 1 else '_control'), 'w'),
              indent=1)
    sads = np.array([r['sad'] for r in out])
    for thr in (4, 6, 8, 10, 15, 20):
        print('SAD<=%2d: %3d/%d Kacheln (%.1f%%)'
              % (thr, int((sads <= thr).sum()), n,
                 (sads <= thr).mean() * 100))
    print('SAD min/median/max %.2f %.2f %.2f'
          % (sads.min(), float(np.median(sads)), sads.max()))
    agg = {}
    for r in out:
        if r['atlas']:
            agg.setdefault(r['atlas'], []).append(r['sad'])
    for k, v in sorted(agg.items(), key=lambda kv: -len(kv[1]))[:10]:
        print('%-32s %3d Kacheln meanSAD %.2f'
              % (k, len(v), float(np.mean(v))))


CACHE = {}

if __name__ == '__main__':
    main()
