"""title_a.xa: Welche Textur gehoert zu welchem Quad? Brute-Force-Suche.

Mapping: screen_x = pos_x + 200, screen_y = pos_y + 120 (origin = Mitte).
Rendereihenfolge: z absteigend (grosse z = hinten).
  python tools\\titlea_search.py
"""
import itertools
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _imgc
import _atest_imgc_np as D
import _atest_xpck as X

FA = os.path.join(ROOT, 'out', 'romfs', 'lt6', 'arc', 'lt6_uk.fa')
XA_OFF, XA_LEN = 0x01428314, 0xFEAA8
NAMES = ['000.xi', '001.xi', '002.xi', '003.xi', '004.xi']


def load():
    d = open(FA, 'rb').read()
    cnt, ents = X.parse(d, XA_OFF, XA_LEN, _imgc.decomp)
    ent = {e['name']: (e['off'], e['size']) for e in ents}
    tex = {}
    for e in ents:
        if e['magic'] == 'IMGC00':
            w, h, rgb, al = D.decode_buf(d, XA_OFF + e['off'])
            a = al if al is not None else np.full((h, w), 255, np.uint8)
            tex[e['name']] = (rgb.astype(np.float64), a.astype(np.float64))
    off, size = ent['000.pvb']
    pvb = d[XA_OFF + off:XA_OFF + off + size]
    vtx_off, stride, vcount = np.frombuffer(pvb, '<H', 3, 8)
    vtx = _imgc.decomp(pvb[vtx_off:])
    vs = []
    for i in range(vcount):
        px, py, pz = np.frombuffer(vtx, '<f4', 3, i * stride)
        u, v = np.frombuffer(vtx, '<f4', 2, i * stride + 12)
        vs.append((float(px), float(py), float(pz), float(u), float(v)))
    quads = [vs[q*4:q*4+4] for q in range(vcount // 4)]
    return tex, quads


def quad_rect(v):
    us = [p[3] for p in v]; vv = [p[4] for p in v]
    xs = [p[0] for p in v]; ys = [p[1] for p in v]
    return (min(xs) + 200, min(ys) + 120, max(xs) - min(xs),
            max(ys) - min(ys), min(us), min(vv), max(us), max(vv), v[0][2])


def draw(canvas, tex, name, r, flip=0):
    x, y, w, h, u0, v0, u1, v1, z = r
    rgb, al = tex[name]
    th, tw = rgb.shape[:2]
    xi0, xi1 = int(round(x)), int(round(x + w))
    yi0, yi1 = int(round(y)), int(round(y + h))
    xs = np.clip(np.round(np.linspace(u0, u1, xi1 - xi0)).astype(int),
                 0, tw - 1)
    if flip:
        ys = np.clip(np.round(np.linspace(v1, v0, yi1 - yi0)).astype(int),
                     0, th - 1)
    else:
        ys = np.clip(np.round(np.linspace(v0, v1, yi1 - yi0)).astype(int),
                     0, th - 1)
    s = rgb[np.ix_(ys, xs)]
    a = (al[np.ix_(ys, xs)] / 255.0)[:, :, None]
    sx0, sx1 = max(0, xi0), min(400, xi1)
    sy0, sy1 = max(0, yi0), min(240, yi1)
    if sx0 >= sx1 or sy0 >= sy1:
        return
    sub = s[sy0 - yi0:sy1 - yi0, sx0 - xi0:sx1 - xi0]
    sa = a[sy0 - yi0:sy1 - yi0, sx0 - xi0:sx1 - xi0]
    canvas[sy0:sy1, sx0:sx1] = (sub * sa +
                                canvas[sy0:sy1, sx0:sx1] * (1 - sa))


def score(canvas, ref):
    d = np.abs(canvas - ref)
    return float(d.mean()), int((d.sum(axis=2) > 60).sum())


def main():
    tex, quads = load()
    ref = np.frombuffer(open(os.path.join(
        ROOT, 'reports', 'menu_official.bin'), 'rb').read(),
        np.uint8).reshape(240, 400, 3).astype(np.float64)
    rects = [quad_rect(v) for v in quads]
    order = sorted(range(9), key=lambda q: -rects[q][8])
    print('Render-Reihenfolge (z desc):', [(q, rects[q][8]) for q in order])
    # Nur die 4 grossen Ebenen: 0..3. quad0 = 004.xi fix (UV passt exakt).
    best = []
    choices = [('004.xi',), NAMES, NAMES, NAMES]
    for combo in itertools.product(*choices):
        canvas = np.zeros((240, 400, 3))
        for q in order:
            if q > 3:
                continue
            draw(canvas, tex, combo[q], rects[q])
        best.append((score(canvas, ref), combo))
    best.sort()
    print('Top-5 Kombinationen (Ebenen 0..3, kein Flip):')
    for s, c in best[:5]:
        print('   mean %.2f diff>60 %6d  %s' % (s[0], s[1], c))

    # Overlays mit den besten Ebenen
    s0, c0 = best[0]
    best2 = []
    och = [NAMES, NAMES, NAMES]  # quad4/6, 5/8, 7
    for combo in itertools.product(och[0], och[1], och[2]):
        canvas = np.zeros((240, 400, 3))
        for q in order:
            if q == 4 or q == 6:
                nm = combo[0]
            elif q == 5 or q == 8:
                nm = combo[1]
            elif q == 7:
                nm = combo[2]
            else:
                nm = c0[q]
            draw(canvas, tex, nm, rects[q])
        best2.append((score(canvas, ref), combo))
    best2.sort()
    print('Top-5 mit Overlays:')
    for s, c in best2[:5]:
        print('   mean %.2f diff>60 %6d  4/6=%s 5/8=%s 7=%s'
              % (s[0], s[1], c[0], c[1], c[2]))


if __name__ == '__main__':
    main()
