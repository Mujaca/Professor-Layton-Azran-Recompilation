"""title_a.xa: Quads gegen die Captures pruefen und zusammensetzen.

Mapping (Hypothese): Bildschirmkoordinaten x=pos_x+200, y=120-pos_y.
Damit ist Quad 3 (x=-200..200, y=-120..120) exakt der Top-Screen 400x240.

  python tools\\titlea_render.py
"""
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
    att_off, att_end, vtx_off, stride, vcount = np.frombuffer(
        pvb, np.uint16, 5, 4)
    vtx = _imgc.decomp(pvb[vtx_off:])
    vs = []
    for i in range(vcount):
        px, py, pz = np.frombuffer(vtx, '<f4', 3, i * stride)
        u, v = np.frombuffer(vtx, '<f4', 2, i * stride + 12)
        vs.append((float(px), float(py), float(pz), float(u), float(v)))
    quads = []
    for q in range(vcount // 4):
        i = q * 4
        quads.append((vs[i], vs[i + 1], vs[i + 3]))
    return tex, quads


def sample(tex, u0, v0, u1, v1, w, h, flip):
    rgb, al = tex
    th, tw = rgb.shape[:2]
    xs = np.clip(np.linspace(u0, u1, w), 0, tw - 1).astype(int)
    if flip:
        ys = np.clip(np.linspace(v1, v0, h), 0, th - 1).astype(int)
    else:
        ys = np.clip(np.linspace(v0, v1, h), 0, th - 1).astype(int)
    return rgb[np.ix_(ys, xs)].astype(int), al[np.ix_(ys, xs)]


def main():
    tex, quads = load()
    ref = np.frombuffer(open(os.path.join(
        ROOT, 'reports', 'menu_official.bin'), 'rb').read(),
        np.uint8).reshape(240, 400, 3).astype(int)
    bot = np.frombuffer(open(os.path.join(
        ROOT, 'reports', 'menu_official_bottom.bin'), 'rb').read(),
        np.uint8).reshape(240, 320, 3).astype(int)
    print('Textexturen:', {k: v[0].shape[:2] for k, v in tex.items()})

    # 1) asset 1:1 gegen Top/Bottom
    for name, img in tex.items():
        rgb, al = img
        h, w = rgb.shape[:2]
        for label, target in (('top', ref), ('bottom', bot)):
            th, tw = target.shape[:2]
            if (w, h) == (tw, th):
                d = np.abs(rgb.astype(int) - target).sum(axis=2)
                print('%s %s 1:1 diff>60 %6d/%6d mean %.2f'
                      % (name, label, (d > 60).sum(), d.size, d.mean()))

    print('\nQuad-BBoxen (x=px+200, y=120-py):')
    for q, (p0, p1, p3) in enumerate(quads):
        x0 = min(p0[0], p1[0], p3[0]) + 200
        x1 = max(p0[0], p1[0], p3[0]) + 200
        y0 = 120 - max(p0[1], p1[1], p3[1])
        y1 = 120 - min(p0[1], p1[1], p3[1])
        u0, u1 = min(p0[3], p1[3], p3[3]), max(p0[3], p1[3], p3[3])
        v0, v1 = min(p0[4], p1[4], p3[4]), max(p0[4], p1[4], p3[4])
        print(' quad %d screen x=%6.1f..%6.1f y=%6.1f..%6.1f '
              'uv=%6.0f..%6.0f v=%6.0f..%6.0f z=%.0f'
              % (q, x0, x1, y0, y1, u0, u1, v0, v1, p0[2]))

    # 2) Welche Textur passt zu welchem Quad?
    print('\nBest-Match Textur je Quad (Alpha-Comp vs Top-Capture):')
    for q, (p0, p1, p3) in enumerate(quads):
        x0 = int(min(p0[0], p1[0], p3[0]) + 200)
        x1 = int(max(p0[0], p1[0], p3[0]) + 200)
        y0 = int(120 - max(p0[1], p1[1], p3[1]))
        y1 = int(120 - min(p0[1], p1[1], p3[1]))
        u0, u1 = min(p0[3], p1[3], p3[3]), max(p0[3], p1[3], p3[3])
        v0, v1 = min(p0[4], p1[4], p3[4]), max(p0[4], p1[4], p3[4])
        w = max(1, x1 - x0)
        hgt = max(1, y1 - y0)
        best = []
        for name, (rgb, al) in tex.items():
            for flip in (0, 1):
                s, sa = sample((rgb, al), u0, v0, u1, v1, w, hgt, flip)
                patch = ref[max(0, y0):y1, max(0, x0):x1]
                if patch.shape[:2] != s.shape[:2]:
                    continue
                a = (sa.astype(float) / 255.0)[:, :, None]
                out = (s * a + patch * (1 - a)).astype(int)
                d = np.abs(out - patch).sum(axis=2)
                best.append((float(d.mean()), name, flip,
                             float((sa > 128).mean())))
        best.sort()
        print(' quad %d: %s' % (q, ' | '.join(
            '%s flip=%d sad=%.1f cover=%.2f' % (n, f, d, c)
            for d, n, f, c in best[:3])))

    # 3) Komposition in Quell-Z-Ordnung (klein zuerst / gross zuerst)
    for order in ('zd', 'za'):
        canvas = np.zeros((240, 400, 3), int)
        idx = sorted(range(len(quads)), key=lambda q: quads[q][0][2],
                     reverse=(order == 'za'))
        # 0..3 sind Fullscreen-Ebenen (unterste), 4..8 Overlays
        for q in idx:
            p0, p1, p3 = quads[q]
            x0 = int(min(p0[0], p1[0], p3[0]) + 200)
            x1 = int(max(p0[0], p1[0], p3[0]) + 200)
            y0 = int(120 - max(p0[1], p1[1], p3[1]))
            y1 = int(120 - min(p0[1], p1[1], p3[1]))
            u0, u1 = min(p0[3], p1[3], p3[3]), max(p0[3], p1[3], p3[3])
            v0, v1 = min(p0[4], p1[4], p3[4]), max(p0[4], p1[4], p3[4])
            w, hgt = x1 - x0, y1 - y0
            name = '002.xi' if q == 3 else ('004.xi' if q == 0 else
                                            '002.xi')
            rgb, al = tex[name]
            s, sa = sample((rgb, al), u0, v0, u1, v1, w, hgt, 0)
            sy0, sy1 = max(0, y0), min(240, y1)
            sx0, sx1 = max(0, x0), min(400, x1)
            if sy0 >= sy1 or sx0 >= sx1:
                continue
            sub = s[sy0 - y0:sy1 - y0, sx0 - x0:sx1 - x0]
            suba = (sa[sy0 - y0:sy1 - y0, sx0 - x0:sx1 - x0]
                    .astype(float) / 255.0)[:, :, None]
            canvas[sy0:sy1, sx0:sx1] = (
                sub * suba + canvas[sy0:sy1, sx0:sx1] * (1 - suba)
            ).astype(int)
        d = np.abs(canvas - ref).sum(axis=2)
        print('%s: diff>60 %6d/%6d mean %.2f' % (
            order, (d > 60).sum(), d.size, d.mean()))


if __name__ == '__main__':
    main()
