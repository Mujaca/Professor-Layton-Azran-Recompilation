"""menu/uk/title_a.xa: XPCK -> XPVB/XPVI -> Quads (Position + UV).

Format wie title_b.xa (tools/titleb_sprites.py):
  XPVB: '<HHHHI' @4 = attr_off, attr_end, vtx_off, stride, count
        attr-Slots: slot0 = float32x3 Position, slot4 = float32x2 UV (Pixel)
  XPVI: u32 count @8, Dreiecksindizes u16 ab 0x10.

Ausgabe: Datei/Offset, Stride, Count, 9 Quads als x y w h u0 v0 u1 v1.

  python tools\\titlea_quads.py
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _imgc
import _atest_xpck as X

FA = os.path.join(ROOT, 'out', 'romfs', 'lt6', 'arc', 'lt6_uk.fa')
XA_OFF, XA_LEN = 0x01428314, 0xFEAA8
NAME = 'menu/uk/title_a.xa'


def main():
    d = open(FA, 'rb').read()
    cnt, ents = X.parse(d, XA_OFF, XA_LEN, _imgc.decomp)
    ent = {e['name']: (e['off'], e['size']) for e in ents}
    print('XPCK %s @%08X: %d Eintraege' % (NAME, XA_OFF, cnt))
    for e in ents:
        print('  %-10s off=%08X (abs %08X) size=%d %s'
              % (e['name'], e['off'], XA_OFF + e['off'], e['size'],
                 e['magic']))

    off, size = ent['000.pvb']
    pvb = d[XA_OFF + off:XA_OFF + off + size]
    att_off, att_end, vtx_off, stride, vcount = struct.unpack_from(
        '<HHHHI', pvb, 4)
    att = _imgc.decomp(pvb[att_off:att_end])
    vtx = _imgc.decomp(pvb[vtx_off:])
    print('\nXPVB 000.pvb @abs %08X: attr=%d..%d vtx@0x%X stride=%d '
          'count=%d -> %d Bytes'
          % (XA_OFF + off, att_off, att_end, vtx_off, stride, vcount,
             len(vtx)))
    for slot in range(10):
        c, o, s, t = att[slot * 4:slot * 4 + 4]
        if c or s:
            print('  slot %d count=%d off=%d size=%d type=%d'
                  % (slot, c, o, s, t))
    print('  vtx bytes %d == stride*count %d: %s'
          % (len(vtx), stride * vcount, len(vtx) == stride * vcount))

    vs = []
    for i in range(vcount):
        px, py, pz = struct.unpack_from('<3f', vtx, i * stride)
        u, v = struct.unpack_from('<2f', vtx, i * stride + 12)
        vs.append((px, py, pz, u, v))
    for i, (px, py, pz, u, v) in enumerate(vs):
        print('  v%02d pos=(%8.3f,%8.3f,%8.3f) uv=(%7.2f,%7.2f)'
              % (i, px, py, pz, u, v))

    print('\nQuads (v0,v1,v3 = Ecken):')
    for q in range(vcount // 4):
        i = q * 4
        p0, p1, p3 = vs[i], vs[i + 1], vs[i + 3]
        w = max(p0[0], p1[0], p3[0]) - min(p0[0], p1[0], p3[0])
        h = max(p0[1], p1[1], p3[1]) - min(p0[1], p1[1], p3[1])
        u0 = min(p0[3], p1[3], p3[3])
        v0 = min(p0[4], p1[4], p3[4])
        u1 = max(p0[3], p1[3], p3[3])
        v1 = max(p0[4], p1[4], p3[4])
        print('  quad %2d x=%8.2f y=%8.2f w=%8.2f h=%8.2f '
              'uv=(%7.2f,%7.2f)-(%7.2f,%7.2f)'
              % (q, p0[0], p0[1], w, h, u0, v0, u1, v1))

    print('\nPBI-Frames:')
    for n in range(10):
        nm = '%03d.pbi' % n
        if nm not in ent:
            continue
        o, s = ent[nm]
        p = d[XA_OFF + o:XA_OFF + o + s]
        idxc = struct.unpack_from('<I', p, 8)[0]
        idx = struct.unpack_from('<%dH' % idxc, p, 0x10) if idxc else ()
        qs = sorted(set(i // 4 for i in idx))
        print('  %s @abs %08X n=%d quads=%s idx=%s'
              % (nm, XA_OFF + o, idxc, qs, list(idx)))


if __name__ == '__main__':
    main()
