"""Offline-Evaluierung: Quads aus title_b.xa an ANMC-Positionen auf bg."""
import os
import struct
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _atest_xpck as X
import _atest_imgc_np as D
import _imgc

FA = os.path.join(ROOT, 'out/romfs/lt6/arc/lt6_uk.fa')
XA_OFF, XA_LEN = 0x01526DC0, 0x9278
VIS = 223


def f32(v):
    return struct.unpack('<f', struct.pack('<I', v))[0]


def load_quads():
    d = open(FA, 'rb').read()
    cnt, ents = X.parse(d, XA_OFF, XA_LEN, _imgc.decomp)
    ent = {e['name']: e for e in ents}
    pvb = d[XA_OFF + ent['000.pvb']['off']:
            XA_OFF + ent['000.pvb']['off'] + ent['000.pvb']['size']]
    vtx_off, stride, vcount = struct.unpack_from('<HHHHI', pvb, 4)[2:]
    vtx = _imgc.decomp(pvb[vtx_off:])
    vs = []
    for i in range(vcount):
        px, py, pz = struct.unpack_from('<3f', vtx, i * stride)
        u, v = struct.unpack_from('<2f', vtx, i * stride + 12)
        vs.append((px, py, pz, u, v))
    quads = []
    for q in range(vcount // 4):
        v = vs[q * 4:q * 4 + 4]
        x0 = min(p[0] for p in v)
        y0 = min(p[1] for p in v)
        x1 = max(p[0] for p in v)
        y1 = max(p[1] for p in v)
        u0 = int(round(min(p[3] for p in v)))
        v0 = int(round(min(p[4] for p in v)))
        u1 = int(round(max(p[3] for p in v)))
        v1 = int(round(max(p[4] for p in v)))
        quads.append(dict(q=q, lx=x0, ly=y0, w=x1 - x0, h=y1 - y0,
                          u0=u0, v0=v0, u1=u1, v1=v1))
    e = ent['000.xi']
    o = XA_OFF + e['off']
    w, h, rgb, a = D.decode_buf(d, o)
    if a is None:
        a = np.full((h, w), 255, np.uint8)
    rgba = np.empty((h, w, 4), np.uint8)
    rgba[..., :3] = rgb
    rgba[..., 3] = a
    return quads, rgba


def anmc_records():
    """btr/sound-Records: (name, x, y, lx, ly, x1, y1)."""
    d = open(os.path.join(ROOT, 'tools/out/anmc_res.bin'), 'rb').read()
    out = []
    for start, n in ((0xB2C, 21), (0xF1C, 8), (0x109C, 1)):
        for i in range(n):
            o = start + i * 0x30
            h = struct.unpack_from('<I', d, o)[0]
            f = [f32(struct.unpack_from('<I', d, o + 8 + 4 * k)[0])
                 for k in range(10)]
            x = f[1] + f[4]
            y = f[2] + f[5]
            out.append((h, x, y, f[7] - f[4], f[8] - f[5], o))
    return out


def metric(img, ref):
    d = np.abs(img.astype(int) - ref.astype(int)).sum(axis=2)
    return int((d[:VIS] > 60).sum()), float(np.abs(img[:VIS].astype(int) -
                                                 ref[:VIS].astype(int)).mean())


def composite(bg, atlas, qp, x, y, sw):
    w, h = atlas.shape[1], atlas.shape[0]
    a = atlas[..., 3].astype(np.float32) / 255.0
    rgb = atlas[..., :3].astype(np.float32)
    out = bg.astype(np.float32).copy()
    # Quell-Rechteck in Atlas-Pixeln, Ziel w x h
    u0, v0, u1, v1 = qp['u0'], qp['v0'], qp['u1'], qp['v1']
    dstw, dsth = int(round(qp['w'])), int(round(qp['h']))
    for j in range(dsth):
        sy = int(v0 + (v1 - v0) * (j + 0.5) / dsth)
        yy = int(y) + j
        if yy < 0 or yy >= 240:
            continue
        for i in range(dstw):
            sx = int(u0 + (u1 - u0) * (i + 0.5) / dstw)
            xx = int(x) + i
            if xx < 0 or xx >= 320:
                continue
            al = a[min(max(sy, 0), h - 1), min(max(sx, 0), w - 1)]
            if al <= 0:
                continue
            out[yy, xx] = rgb[min(max(sy, 0), h - 1),
                              min(max(sx, 0), w - 1)] * al + \
                out[yy, xx] * (1.0 - al)
    return np.clip(out, 0, 255).astype(np.uint8)


def main():
    quads, atlas = load_quads()
    recs = anmc_records()
    cap = np.frombuffer(open(os.path.join(
        ROOT, 'reports/menu_official_bottom.bin'), 'rb').read(),
        dtype=np.uint8).reshape(240, 320, 3)
    bg = np.frombuffer(open(os.path.join(ROOT, 'out/title_b_bg.bin'), 'rb').read(),
                       dtype=np.uint8).reshape(240, 320, 3)
    print('BASE', metric(bg, cap))
    # Kandidaten mit passender Groesse
    for qp in quads:
        best = []
        for (h, x, y, w, hh, o) in recs:
            if abs(w - qp['w']) <= 2.5 and abs(hh - qp['h']) <= 2.5:
                best.append((x, y, w, hh, o))
        print('\nquad %2d %gx%g uv=%s  placements=%s'
              % (qp['q'], qp['w'], qp['h'],
                 (qp['u0'], qp['v0'], qp['u1'], qp['v1']),
                 ', '.join('(%.0f,%.0f,%.0fx%.0f)' % b[:4] for b in best)))
        for (x, y, w, hh, o) in best:
            img = composite(bg, atlas, qp, x, y, True)
            m = metric(img, cap)
            # lokal
            ys, ye = max(0, int(y)), min(VIS, int(y + hh))
            xs, xe = max(0, int(x)), min(320, int(x + w))
            loc = np.abs(img[ys:ye, xs:xe].astype(int) -
                         cap[ys:ye, xs:xe].astype(int)).sum()
            loc0 = np.abs(bg[ys:ye, xs:xe].astype(int) -
                          cap[ys:ye, xs:xe].astype(int)).sum()
            print('   @%04X (%3.0f,%3.0f) -> %s  local %d->%d'
                  % (o, x, y, m, loc0, loc))


if __name__ == '__main__':
    main()
