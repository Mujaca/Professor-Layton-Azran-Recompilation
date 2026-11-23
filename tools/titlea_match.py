"""Template-Matching: title_a-Quads im Top-Capture suchen (NCC + Position).

Damit wird bestimmt, (a) welche Textur zu welchem Quad gehoert und
(b) wo das Sprite wirklich auf dem 400x240-Screen liegt.

  python tools\\titlea_match.py
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
                tex[e['name']] = (rgb.astype(np.float64),
                                  a.astype(np.float64))
    off, size = ent['000.pvb']
    pvb = d[XA_OFF + off:XA_OFF + off + size]
    vtx_off, stride, vcount = np.frombuffer(pvb, '<H', 3, 8)
    vtx = _imgc.decomp(pvb[vtx_off:])
    vs = []
    for i in range(vcount):
        px, py, pz = np.frombuffer(vtx, '<f4', 3, i * stride)
        u, v = np.frombuffer(vtx, '<f4', 2, i * stride + 12)
        vs.append((float(px), float(py), float(pz), float(u), float(v)))
    quads = []
    for q in range(vcount // 4):
        v = vs[q * 4:q * 4 + 4]
        quads.append(v)
    return tex, quads


def region(tex, u0, v0, u1, v1, flip):
    rgb, al = tex
    th, tw = rgb.shape[:2]
    xs = np.clip(np.round(np.arange(u0, u1)), 0, tw - 1).astype(int)
    if flip:
        ys = np.clip(np.round(np.arange(v1, v0, -1)), 0, th - 1).astype(int)
    else:
        ys = np.clip(np.round(np.arange(v0, v1)), 0, th - 1).astype(int)
    return rgb[np.ix_(ys, xs)], al[np.ix_(ys, xs)]


def ncc_search(img, tmpl, mask, step=1):
    ih, iw = img.shape[:2]
    th, tw = tmpl.shape[:2]
    if th > ih or tw > iw:
        return -2, (0, 0)
    lum_i = img.mean(axis=2)
    lum_t = tmpl.mean(axis=2)
    n = mask.sum()
    if n < 10:
        return -2, (0, 0)
    tm = lum_t[mask > 0]
    tm = tm - tm.mean()
    tn = np.sqrt((tm * tm).sum()) + 1e-9
    best = (-2.0, (0, 0))
    for y in range(0, ih - th + 1, step):
        for x in range(0, iw - tw + 1, step):
            patch = lum_i[y:y + th, x:x + tw][mask > 0]
            p = patch - patch.mean()
            d = np.sqrt((p * p).sum()) + 1e-9
            c = float((p * tm).sum() / (d * tn))
            if c > best[0]:
                best = (c, (x, y))
    return best


def main():
    tex, quads = load()
    ref = np.frombuffer(open(os.path.join(
        ROOT, 'reports', 'menu_official.bin'), 'rb').read(),
        np.uint8).reshape(240, 400, 3).astype(np.float64)
    for q, v in enumerate(quads):
        us = [p[3] for p in v]
        vv = [p[4] for p in v]
        u0, u1, v0, v1 = min(us), max(us), min(vv), max(vv)
        print('quad %d uv=%.0f..%.0f v=%.0f..%.0f' % (q, u0, u1, v0, v1))
        res = []
        for name, t in tex.items():
            tw, th = t[0].shape[1], t[0].shape[0]
            if u1 > tw or v1 > th:
                continue
            for flip in (0, 1):
                s, a = region(t, u0, v0, u1, v1, flip)
                m = a > 200
                if m.mean() < 0.02:
                    continue
                c, pos = ncc_search(ref, s, m)
                res.append((c, name, flip, pos, m.mean()))
        res.sort(reverse=True)
        for c, name, flip, pos, cov in res[:3]:
            print('   %-8s flip=%d ncc=%+.3f pos=%s cover=%.2f'
                  % (name, flip, c, pos, cov))


if __name__ == '__main__':
    main()
