"""menu/uk/title_b.xa (Bottom-Overlay): 23 XPVB-Quads als Rechtecke.

  python tools/titleb_rects.py
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _imgc
import _atest_xpck as X

FA = os.path.join(ROOT, 'out', 'romfs', 'lt6', 'arc', 'lt6_uk.fa')
XA_OFF, XA_LEN = 0x01526DC0, 0x9278


def main():
    d = open(FA, 'rb').read()
    cnt, ents = X.parse(d, XA_OFF, XA_LEN, _imgc.decomp)
    ent = {e['name']: (e['off'], e['size']) for e in ents}
    off, size = ent['000.pvb']
    pvb = d[XA_OFF + off:XA_OFF + off + size]
    vtx_off, stride, vcount = struct.unpack_from('<HHHHI', pvb, 4)[2:]
    vtx = _imgc.decomp(pvb[vtx_off:])
    vs = []
    for i in range(vcount):
        px, py, pz = struct.unpack_from('<3f', vtx, i * stride)
        u, v = struct.unpack_from('<2f', vtx, i * stride + 12)
        vs.append((px, py, pz, u, v))
    print('XPVB 000.pvb @%08X stride=%d count=%d (%d Quads)'
          % (XA_OFF + off, stride, vcount, vcount // 4))
    for q in range(vcount // 4):
        v = vs[q * 4:q * 4 + 4]
        sx = [p[0] for p in v]
        sy = [p[1] for p in v]
        us = [p[3] for p in v]
        vv = [p[4] for p in v]
        print('quad %2d  pos=(%6.1f,%6.1f) %5.1fx%-5.1f  '
              'uv=(%3.0f,%3.0f)-(%3.0f,%3.0f)'
              % (q, min(sx), min(sy), max(sx) - min(sx),
                 max(sy) - min(sy), min(us), min(vv), max(us), max(vv)))


if __name__ == '__main__':
    main()
