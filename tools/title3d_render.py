"""Erster 3D-Titel-Render: .prm-Quads mit Perspektive + ETC1-Texturen.

Die title.xc-Meshes sind wenige texturierte Quads bei z ~ -28 (Weltunits).
Dieses Tool rendert sie mit einer Perspektive (Brennweite f) und probiert
Textur x Brennweite durch; gemessen wird die Korrelation zum Capture.

Aufruf: python tools/title3d_render.py
"""
import os
import struct
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import title3d_probe as T  # noqa: E402
import _imgc  # noqa: E402
import title3d_lib as L  # noqa: E402

REF = os.path.join(ROOT, 'reports/menu_official.bin')
W, H = 400, 240
TEXS = ['000.xi', '003.xi', '005.xi', '006.xi', '010.xi', '013.xi']


def prm_quads():
    d, ents = T.entries()
    out = []
    for s, fo, sz, magic in ents:
        if not s.endswith('.prm'):
            continue
        i = d.find(b'XPVB', fo, fo + sz)
        if i < 0:
            continue
        vtx, stride, vcount = struct.unpack_from('<HHHHI', d, i + 4)[2:]
        vbuf = _imgc.decomp(d[i + vtx:i + vtx + 0x8000])
        vs = []
        for k in range(vcount):
            x, y, z = struct.unpack_from('<3f', vbuf, k * stride)
            u, v = struct.unpack_from('<2f', vbuf, k * stride + 12)
            vs.append((x, y, z, u, v))
        out.append((s, vs))
    return out


def to_screen(vs, f):
    pts = []
    for x, y, z, u, v in vs:
        zz = -z if z < 0 else 1.0
        pts.append((W / 2.0 + f * x / zz, H / 2.0 - f * y / zz, u, v))
    return pts


def fill_tri(canvas, tex, p0, p1, p2):
    xs = [p0[0], p1[0], p2[0]]
    ys = [p0[1], p1[1], p2[1]]
    x0, x1 = max(0, int(min(xs))), min(W - 1, int(max(xs)) + 1)
    y0, y1 = max(0, int(min(ys))), min(H - 1, int(max(ys)) + 1)
    if x1 <= x0 or y1 <= y0:
        return
    d = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1])
    if abs(d) < 1e-9:
        return
    yy, xx = np.mgrid[y0:y1 + 1, x0:x1 + 1]
    w0 = ((p1[0] - xx) * (p2[1] - yy) - (p2[0] - xx) * (p1[1] - yy)) / d
    w1 = ((p2[0] - xx) * (p0[1] - yy) - (p0[0] - xx) * (p2[1] - yy)) / d
    w2 = 1.0 - w0 - w1
    m = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
    if not m.any():
        return
    th, tw = tex.shape[:2]
    u = w0 * p0[2] + w1 * p1[2] + w2 * p2[2]
    v = w0 * p0[3] + w1 * p1[3] + w2 * p2[3]
    tx = np.clip((u * (tw - 1)).astype(int), 0, tw - 1)
    ty = np.clip((v * (th - 1)).astype(int), 0, th - 1)
    canvas[y0:y1 + 1, x0:x1 + 1][m] = tex[ty[m], tx[m]]


def main():
    quads = prm_quads()
    print('prm-Quads: %d' % len(quads))
    ref = np.fromfile(REF, dtype=np.uint8).reshape(240, 400, 3).astype(np.float64)
    rg = ref.mean(2)
    best = (-2.0, None)
    for name in TEXS:
        w, h, rgb = L.xi_rgb(name)
        tex = np.frombuffer(rgb, dtype=np.uint8).reshape(h, w, 3).astype(np.float64)
        for f in range(80, 421, 20):
            canvas = np.zeros((H, W, 3), np.float64)
            for _s, vs in quads:
                pts = to_screen(vs, f)
                if len(pts) < 3:
                    continue
                for i in range(1, len(pts) - 1):
                    fill_tri(canvas, tex, pts[0], pts[i], pts[i + 1])
            c = float(np.corrcoef(rg.ravel(), canvas.mean(2).ravel())[0, 1])
            if c > best[0]:
                best = (c, (name, f, canvas))
    c, (name, f, canvas) = best
    print('best corr %.3f  tex=%s f=%d' % (c, name, f))
    Image.fromarray(canvas.astype(np.uint8)).save(
        os.path.join(ROOT, 'out/title3d_render.png'))


if __name__ == '__main__':
    main()
