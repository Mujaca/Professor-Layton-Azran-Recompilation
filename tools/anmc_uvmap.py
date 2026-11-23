"""Atlas-Analyse: je UV-Rect Alpha-Abdeckung, Glyph-Spanne, Farbe."""
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _atest_xpck as X
import _atest_imgc_np as D
import _imgc
import titleb_rects as TR

FA = os.path.join(ROOT, 'out/romfs/lt6/arc/lt6_uk.fa')
XA_OFF, XA_LEN = 0x01526DC0, 0x9278


def main():
    d = open(FA, 'rb').read()
    cnt, ents = X.parse(d, XA_OFF, XA_LEN, _imgc.decomp)
    e = [x for x in ents if x['name'] == '000.xi'][0]
    o = XA_OFF + e['off']
    w, h, rgb, a = D.decode_buf(d, o)
    aa = a if a is not None else np.full((h, w), 255, np.uint8)
    # Quads aus XPVB
    ent = {x['name']: x for x in ents}
    pvb = d[XA_OFF + ent['000.pvb']['off']:
            XA_OFF + ent['000.pvb']['off'] + ent['000.pvb']['size']]
    vtx_off, stride, vcount = __import__('struct').unpack_from('<HHHHI', pvb, 4)[2:]
    vtx = _imgc.decomp(pvb[vtx_off:])
    vs = []
    for i in range(vcount):
        px, py, pz = __import__('struct').unpack_from('<3f', vtx, i * stride)
        u, v = __import__('struct').unpack_from('<2f', vtx, i * stride + 12)
        vs.append((px, py, pz, u, v))
    print('quad  local_rect        uv_rect          alpha>0  glyph_x_span '
          'opaque_rgb')
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
        sub = aa[v0:v1, u0:u1]
        m = sub > 0
        if m.any():
            xs = np.where(m.any(axis=0))[0]
            sx = '%d..%d (%d)' % (xs.min(), xs.max(), xs.max() - xs.min() + 1)
            col = rgb[v0:v1, u0:u1][m].mean(axis=0)
            cstr = '(%3.0f,%3.0f,%3.0f)' % tuple(col)
        else:
            sx = '-'
            cstr = '-'
        print('%2d  (%6.1f,%5.1f) %5.1fx%-5.1f  (%3d,%3d)-(%3d,%3d) '
              '%6d  %-12s %s'
              % (q, x0, y0, x1 - x0, y1 - y0, u0, v0, u1, v1,
                 int(m.sum()), sx, cstr))


if __name__ == '__main__':
    main()
