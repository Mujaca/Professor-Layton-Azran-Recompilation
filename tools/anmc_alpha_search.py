"""Alpha-Compositing-Suche der Bottom-Quads (000.xi RGBA, ANMC-Positionen)."""
import os
import struct
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _atest_xpck as X
import _atest_imgc_np as D
import _imgc
import anmc_pack as P

VIS = 223


def draw_alpha(canvas, atlas, q, x, y):
    u0, v0, u1, v1 = q['u0'], q['v0'], q['u1'], q['v1']
    h, w = atlas.shape[:2]
    dstw, dsth = int(round(q['w'])), int(round(q['h']))
    tx = np.clip(np.round(u0 + (np.arange(dstw) + 0.5) * (u1 - u0) / dstw)
                 .astype(int), 0, w - 1)
    ty = np.clip(np.round(v0 + (np.arange(dsth) + 0.5) * (v1 - v0) / dsth)
                 .astype(int), 0, h - 1)
    src = atlas[np.ix_(ty, tx)]
    xs = np.arange(x, x + dstw)
    ys = np.arange(y, y + dsth)
    okx = (xs >= 0) & (xs < 320)
    oky = (ys >= 0) & (ys < 240)
    src = src[np.ix_(oky, okx)].astype(np.float32)
    a = src[..., 3:4] / 255.0
    dst = canvas[np.ix_(ys[oky], xs[okx])].astype(np.float32)
    out = src[..., :3] * a + dst * (1.0 - a)
    canvas[np.ix_(ys[oky], xs[okx])] = np.clip(out, 0, 255).astype(np.uint8)


def metric(img, ref, rows=VIS):
    d = np.abs(img[:rows].astype(int) - ref[:rows].astype(int)).sum(axis=2)
    return int((d > 60).sum()), float(np.abs(img[:rows].astype(int) -
                                             ref[:rows].astype(int)).mean())


def main():
    quads, atlas = P.load_quads_and_atlas()
    cap = np.frombuffer(open(os.path.join(
        ROOT, 'reports/menu_official_bottom.bin'), 'rb').read(),
        dtype=np.uint8).reshape(240, 320, 3)
    bg = np.frombuffer(open(os.path.join(ROOT, 'out/title_b_bg.bin'), 'rb').read(),
                       dtype=np.uint8).reshape(240, 320, 3)
    # Positionen wie in anmc_pack
    pos = {}
    for qi, (off, name, special) in P.PLACE.items():
        rec = P.anmc_rec(off)
        pos[qi] = P.quad_placement(quads[qi], rec, special) + (name,)
    print('BASE', metric(bg, cap))
    print('\nEinzel:')
    for qi in range(23):
        x, y, name = pos[qi]
        img = bg.copy()
        draw_alpha(img, atlas, quads[qi], x, y)
        print('%2d %-12s @(%3d,%3d) %s' % (qi, name, x, y, metric(img, cap)))
    print('\nGreedy (diff, dann mean):')
    cur = bg.copy()
    used = set()
    bestm = metric(cur, cap)
    order = []
    for step in range(10):
        best = None
        for qi in range(23):
            if qi in used:
                continue
            x, y, name = pos[qi]
            img = cur.copy()
            draw_alpha(img, atlas, quads[qi], x, y)
            m = metric(img, cap)
            if best is None or (m[0], m[1]) < (best[0][0], best[0][1]):
                best = (m, qi, img)
        if best is None or (best[0][0], best[0][1]) >= (bestm[0], bestm[1]):
            break
        bestm, qi, img = best
        used.add(qi)
        order.append(qi)
        cur = img
        print(' + %2d %-12s -> %s' % (qi, pos[qi][2], bestm))
    print('order', order)
    # auch nur diff minimieren, mean ignorieren
    print('\nGreedy (nur diff>60):')
    cur = bg.copy()
    used = set()
    bestm = metric(cur, cap)
    order2 = []
    for step in range(10):
        best = None
        for qi in range(23):
            if qi in used:
                continue
            x, y, name = pos[qi]
            img = cur.copy()
            draw_alpha(img, atlas, quads[qi], x, y)
            m = metric(img, cap)
            if best is None or m[0] < best[0][0]:
                best = (m, qi, img)
        if best is None or best[0][0] >= bestm[0]:
            break
        bestm, qi, img = best
        used.add(qi)
        order2.append(qi)
        cur = img
        print(' + %2d %-12s -> %s' % (qi, pos[qi][2], bestm))
    print('order', order2)


if __name__ == '__main__':
    main()
