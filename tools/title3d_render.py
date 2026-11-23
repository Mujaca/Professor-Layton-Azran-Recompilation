"""Host-Render des 3D-Grundlayers aus menu/title.xc (Kamera-Suche).

  python tools/out/title3d_render.py
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import title3d_lib as L

REF = np.frombuffer(open(os.path.join(L.ROOT, 'reports/menu_official.bin'),
                         'rb').read(), dtype=np.uint8).reshape(240, 400, 3)
RH, RW = 240, 400
OUT = os.path.join(L.ROOT, 'tools/out')


def scene():
    if 'scene' in L._cache:
        return L._cache['scene']
    nodes = []
    for i in range(17):
        nm = '%03d.prm' % i
        try:
            p = L.prm_info(nm)
        except KeyError:
            continue
        if i < 12:
            xn = '%03d.xi' % i
        elif i == 12:
            xn = '012.xi'
        else:
            xn = '013.xi'
        w, h, tex = L.xi_rgb(xn)
        nodes.append(dict(prm=nm, verts=p['verts'], tex=tex, tw=w, th=h))
    L._cache['scene'] = nodes
    return nodes


def sample(tex, u, v):
    th, tw = tex.shape[:2]
    x = np.clip(u * (tw - 1), 0, tw - 1)
    y = np.clip(v * (th - 1), 0, th - 1)
    x0 = np.floor(x).astype(np.int32)
    y0 = np.floor(y).astype(np.int32)
    x1 = np.minimum(x0 + 1, tw - 1)
    y1 = np.minimum(y0 + 1, th - 1)
    fx = (x - x0)[..., None]
    fy = (y - y0)[..., None]
    a = tex[y0, x0] * (1 - fx) * (1 - fy)
    b = tex[y0, x1] * fx * (1 - fy)
    c = tex[y1, x0] * (1 - fx) * fy
    d = tex[y1, x1] * fx * fy
    return a + b + c + d


def project(v, f, cx, cy):
    z = -v[:, 2]
    return cx + f * v[:, 0] / z, cy - f * v[:, 1] / z


def draw(f, cx, cy, blend='add', order=None, gain=1.0):
    img = np.zeros((RH, RW, 3), dtype=np.float32)
    nodes = scene()
    idx = list(order) if order is not None else list(range(len(nodes)))
    for k in idx:
        nd = nodes[k]
        v = nd['verts']
        xs, ys = project(v, f, cx, cy)
        for group in range(len(v) // 4):
            o = group * 4
            g = v[o:o + 4]
            gg = slice(o, o + 4)
            u, vv = g[:, 3], g[:, 4]
            x0, x1 = float(xs[gg].min()), float(xs[gg].max())
            y0, y1 = float(ys[gg].min()), float(ys[gg].max())
            ix0, ix1 = max(0, int(np.floor(x0))), min(RW, int(np.ceil(x1)))
            iy0, iy1 = max(0, int(np.floor(y0))), min(RH, int(np.ceil(y1)))
            if ix1 <= ix0 or iy1 <= iy0:
                continue
            px = np.arange(ix0, ix1) + 0.5
            py = np.arange(iy0, iy1) + 0.5
            fxa = (px - x0) / max(1e-6, x1 - x0)
            fya = (py - y0) / max(1e-6, y1 - y0)
            uu = np.clip(u.min() + fxa * (u.max() - u.min()), 0, 1)
            vv2 = np.clip(vv.min() + fya * (vv.max() - vv.min()), 0, 1)
            U, V = np.meshgrid(uu, vv2)
            src = sample(nd['tex'], U, V) * gain
            if blend == 'opaque':
                img[iy0:iy1, ix0:ix1] = src
            elif blend == 'add':
                img[iy0:iy1, ix0:ix1] += src
            else:
                a = img[iy0:iy1, ix0:ix1] / 255.0
                b = src / 255.0
                img[iy0:iy1, ix0:ix1] = (1 - (1 - a) * (1 - b)) * 255.0
    return np.clip(img, 0, 255).astype(np.uint8)


def ncc(a, b, y0=0, y1=RH):
    x = a[y0:y1].astype(np.float64).mean(2)
    y = b[y0:y1].astype(np.float64).mean(2)
    x = x - x.mean()
    y = y - y.mean()
    d = np.sqrt((x * x).sum() * (y * y).sum())
    return float((x * y).sum() / d) if d > 0 else 0.0


def main():
    best = None
    for blend in ('add', 'opaque', 'screen'):
        for cy in range(60, 181, 20):
            for f in range(300, 701, 50):
                img = draw(f, 200, cy, blend)
                c = ncc(img, REF, 0, 60)
                if best is None or c > best[0]:
                    best = (c, blend, f, cy, img)
    print('best bg-NCC(rows0-60) %.3f blend=%s f=%d cy=%d' % best[:4])
    Image.fromarray(best[4]).save(os.path.join(OUT, 'title3d_render.ppm'))
    open(os.path.join(OUT, 'title3d_render.bin'), 'wb').write(best[4].tobytes())
    print('full-frame NCC %.3f' % ncc(best[4], REF))


if __name__ == '__main__':
    main()
