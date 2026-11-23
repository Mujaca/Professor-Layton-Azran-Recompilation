"""title_a.xa: Quads sauber geclippt gegen die Captures pruefen."""
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


def load():
    d = open(FA, 'rb').read()
    cnt, ents = X.parse(d, XA_OFF, XA_LEN, _imgc.decomp)
    ent = {e['name']: (e['off'], e['size']) for e in ents}
    tex = {}
    for e in ents:
        if e['magic'] == 'IMGC00':
            r = D.decode_buf(d, XA_OFF + e['off'])
            if r:
                w, h, rgb, al = r
                a = al if al is not None else np.full((h, w), 255, np.uint8)
                tex[e['name']] = (rgb, a)
    off, size = ent['000.pvb']
    pvb = d[XA_OFF + off:XA_OFF + off + size]
    vtx_off, stride, vcount = np.frombuffer(pvb, '<H', 3, 8)
    vtx = _imgc.decomp(pvb[vtx_off:])
    vs = []
    for i in range(vcount):
        px, py, pz = np.frombuffer(vtx, '<f4', 3, i * stride)
        u, v = np.frombuffer(vtx, '<f4', 2, i * stride + 12)
        vs.append((float(px), float(py), float(pz), float(u), float(v)))
    return tex, [ (vs[q*4], vs[q*4+1], vs[q*4+3])
                  for q in range(vcount // 4) ]


def bbox(p0, p1, p3):
    x0 = int(min(p0[0], p1[0], p3[0]) + 200)
    x1 = int(max(p0[0], p1[0], p3[0]) + 200)
    y0 = int(120 - max(p0[1], p1[1], p3[1]))
    y1 = int(120 - min(p0[1], p1[1], p3[1]))
    u0 = min(p0[3], p1[3], p3[3]); u1 = max(p0[3], p1[3], p3[3])
    v0 = min(p0[4], p1[4], p3[4]); v1 = max(p0[4], p1[4], p3[4])
    return x0, x1, y0, y1, u0, u1, v0, v1


def quad_patch(tex, u0, v0, u1, v1, x0, x1, y0, y1, flip):
    """Liefert (rgb,alpha) in Screengroesse inkl. Rand (auch ausserhalb)."""
    rgb, al = tex
    th, tw = rgb.shape[:2]
    w, h = x1 - x0, y1 - y0
    xs = np.clip(np.round(np.linspace(u0, u1, w)).astype(int), 0, tw - 1)
    if flip:
        ys = np.clip(np.round(np.linspace(v1, v0, h)).astype(int), 0, th - 1)
    else:
        ys = np.clip(np.round(np.linspace(v0, v1, h)).astype(int), 0, th - 1)
    return rgb[np.ix_(ys, xs)].astype(int), al[np.ix_(ys, xs)].astype(int)


def main():
    tex, quads = load()
    ref = np.frombuffer(open(os.path.join(
        ROOT, 'reports', 'menu_official.bin'), 'rb').read(),
        np.uint8).reshape(240, 400, 3).astype(int)
    print('%d Quads, Texten: %s' % (len(quads),
          {k: v[0].shape[:2] for k, v in tex.items()}))
    for q, (p0, p1, p3) in enumerate(quads):
        x0, x1, y0, y1, u0, u1, v0, v1 = bbox(p0, p1, p3)
        res = []
        for name, (rgb, al) in tex.items():
            tw, th = rgb.shape[1], rgb.shape[0]
            if u1 > tw + 1 or v1 > th + 1:
                continue
            for flip in (0, 1):
                s, sa = quad_patch((rgb, al), u0, v0, u1, v1,
                                   x0, x1, y0, y1, flip)
                sx0, sx1 = max(0, x0), min(400, x1)
                sy0, sy1 = max(0, y0), min(240, y1)
                sub = s[sy0-y0:sy1-y0, sx0-x0:sx1-x0]
                suba = sa[sy0-y0:sy1-y0, sx0-x0:sx1-x0]
                patch = ref[sy0:sy1, sx0:sx1]
                m = suba > 128
                if m.sum() < 20:
                    continue
                d = np.abs(sub - patch).sum(axis=2)[m]
                res.append((float(d.mean()), name, flip,
                            float(m.mean()), int(sy1-sy0),
                            int(sx1-sx0)))
        res.sort()
        print('quad %d  scr x=%d..%d y=%d..%d  uv=%.0f..%.0f v=%.0f..%.0f'
              % (q, x0, x1, y0, y1, u0, u1, v0, v1))
        for r in res[:3]:
            print('    %-8s flip=%d sad=%.2f cover=%.2f (%dx%d)'
                  % (r[1], r[2], r[0], r[3], r[5], r[4]))


if __name__ == '__main__':
    main()
