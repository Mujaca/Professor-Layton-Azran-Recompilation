"""Stufe 3: 3D-Titel-Render mit per-Quad-Textur, Z-Sort, Kamera-Suche.

Quads kommen aus den .prm-Meshes (title.xc), Texturen aus den ETC1-Bildern.
Pro Quad wird die Textur gewaehlt, die lokal am besten zum Capture passt;
global werden Brennweite f und vertikaler Versatz dy gesucht. Gezeichnet
wird von hinten (kleinstes -z) nach vorne.

Aufruf: python tools/title3d_render.py
"""
import os
import struct
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import title3d_probe as T     # noqa: E402
import _imgc                  # noqa: E402
import title3d_lib as L       # noqa: E402

REF = os.path.join(ROOT, 'reports/menu_official.bin')
W, H = 400, 240
TEXS = ['000.xi', '003.xi', '005.xi', '006.xi', '010.xi', '013.xi']


def prm_quads():
    d, ents = T.entries()
    out = []
    for s, fo, sz, _magic in ents:
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


def to_screen(vs, f, dy, dx=0.0):
    pts = []
    for x, y, z, u, v in vs:
        zz = -z if z < 0 else 1.0
        pts.append((W / 2.0 + f * x / zz + dx, H / 2.0 - f * y / zz + dy, u, v))
    return pts


def fill_tri(canvas, tex, p0, p1, p2):
    xs = (p0[0], p1[0], p2[0])
    ys = (p0[1], p1[1], p2[1])
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


def draw_quad(canvas, tex, pts):
    if len(pts) < 3:
        return
    for i in range(1, len(pts) - 1):
        fill_tri(canvas, tex, pts[0], pts[i], pts[i + 1])


def local_ncc(rg, canvas):
    m = canvas
    sel = (m > 1).any(2) if m.ndim == 3 else (m > 1)
    if sel.sum() < 50:
        return -9.0
    a = rg[sel]
    b = m[sel] if m.ndim == 2 else m.mean(2)[sel]
    va = a - a.mean()
    vb = b - b.mean()
    d = np.sqrt((va * va).sum() * (vb * vb).sum())
    return float((va * vb).sum() / d) if d > 1e-9 else -9.0


def main():
    quads = prm_quads()
    ref = np.fromfile(REF, dtype=np.uint8).reshape(240, 400, 3).astype(np.float64)
    rg = ref.mean(2)
    texs = {}
    for name in TEXS:
        w, h, rgb = L.xi_rgb(name)
        texs[name] = np.frombuffer(rgb, dtype=np.uint8).reshape(h, w, 3).astype(np.float64)
    order = sorted(range(len(quads)), key=lambda i: min(v[2] for v in quads[i][1]))
    best = (-2.0, None)
    for f in range(100, 261, 20):
        for dy in range(-40, 41, 40):
            chosen = []
            for qi in order:
                _s, vs = quads[qi]
                pts = to_screen(vs, f, dy)
                b = (-9.0, TEXS[0])
                for name in TEXS:
                    canvas = np.zeros((H, W, 3), np.float64)
                    draw_quad(canvas, texs[name], pts)
                    sc = local_ncc(rg, canvas)
                    if sc > b[0]:
                        b = (sc, name)
                chosen.append((qi, b[1], b[0]))
            canvas = np.zeros((H, W, 3), np.float64)
            for qi, name, _sc in chosen:
                draw_quad(canvas, texs[name], to_screen(quads[qi][1], f, dy))
            c = float(np.corrcoef(rg.ravel(), canvas.mean(2).ravel())[0, 1])
            if c > best[0]:
                best = (c, (f, dy, canvas, chosen))
    c, (f, dy, canvas, chosen) = best
    # Stufe 2: Kamera (f, dx, dy) mit fester Texturwahl fein suchen.
    best2 = (-2.0, None)
    for f2 in range(100, 481, 20):
        for dx in range(-60, 61, 20):
            for dy2 in range(-160, 41, 20):
                cv = np.zeros((H, W, 3), np.float64)
                for qi, name, _sc in chosen:
                    draw_quad(cv, texs[name], to_screen(quads[qi][1], f2, dy2, dx))
                c2 = float(np.corrcoef(rg.ravel(), cv.mean(2).ravel())[0, 1])
                if c2 > best2[0]:
                    best2 = (c2, (f2, dx, dy2, cv))
    c2, (f, dx, dy, canvas) = best2
    print('best corr %.3f  f=%d dx=%d dy=%d' % (c2, f, dx, dy))
    for qi, name, sc in chosen:
        print('  quad %2d %-14s tex=%s local %.2f' % (qi, quads[qi][0], name, sc))
    Image.fromarray(canvas.astype(np.uint8)).save(
        os.path.join(ROOT, 'out/title3d_render.png'))
    print('-> out/title3d_render.png')


if __name__ == '__main__':
    main()
