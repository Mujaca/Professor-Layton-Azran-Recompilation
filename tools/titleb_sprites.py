"""title_b.xa: XPCK -> IMGC + XPVB/XPVI (PBI) -> Quads/UVs.

Beleg fuer den Vertex-Bitstream:
  XPVB: attr @0x10..0x28 (RLE, 40 B), vtx @0x34 (LZ10),
        stride 0x14, 92 Vertices.
  Attribute: Slot0 = float32x3 @0, Slot4 = float32x2 (UV in Pixeln) @12.
  Der Vertex-Buffer enthaelt 23 Quads; die .pbi-Dateien listen
  Dreiecksindizes und waehlen 1-2 Quads pro Frame.

  python tools\\titleb_sprites.py
"""
import os
import struct
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _imgc


def xpck_entries(d):
    fc1, fc2 = d[4], d[5]
    count = ((fc2 & 0xF) << 8) | fc1
    info = struct.unpack_from('<H', d, 6)[0] << 2
    names = struct.unpack_from('<H', d, 8)[0] << 2
    data = struct.unpack_from('<H', d, 0xA)[0] << 2
    nsize = struct.unpack_from('<H', d, 0xE)[0] << 2
    table = _imgc.decomp(d[names:names + nsize])
    out = {}
    for i in range(count):
        e = info + i * 12
        h, no, t1, t2, z1, z2 = struct.unpack_from('<IHHHBB', d, e)
        off = data + (((z1 << 16) | t1) << 2)
        size = (z2 << 16) | t2
        s = table[no:table.find(b'\0', no)].decode('latin1')
        out[s] = (off, size)
    return out


def main():
    xa = open(os.path.join(ROOT, 'out/fa_uk/menu/uk/title_b.xa'), 'rb').read()
    ent = xpck_entries(xa)
    off, size = ent['000.pvb']
    pvb = xa[off:off + size]
    att_off, att_end, vtx_off, stride, count = struct.unpack_from(
        '<HHHHI', pvb, 4)
    att = _imgc.decomp(pvb[att_off:att_end])
    vtx = _imgc.decomp(pvb[vtx_off:])
    print('XPVB attr=%d..%d vtx@0x%X stride=%d count=%d -> %d Bytes'
          % (att_off, att_end, vtx_off, stride, count, len(vtx)))
    for slot in range(10):
        c, o, s, t = att[slot * 4:slot * 4 + 4]
        if c or s:
            print('  slot %d count=%d off=%d size=%d type=%d'
                  % (slot, c, o, s, t))
    if len(vtx) != stride * count:
        print('WARN vtx %d != %d' % (len(vtx), stride * count))

    for q in range(count // 4):
        i = q * 4
        p0 = struct.unpack_from('<2f', vtx, i * stride)
        p2 = struct.unpack_from('<2f', vtx, (i + 2) * stride)
        u0 = struct.unpack_from('<2f', vtx, i * stride + 12)
        u3 = struct.unpack_from('<2f', vtx, (i + 3) * stride + 12)
        print('quad %2d pos %gx%g uv (%g,%g)-(%g,%g)'
              % (q, p2[0] - p0[0], p2[1] - p0[1],
                 u0[0], u0[1], u3[0], u3[1]))

    print('--- PBI frames')
    for n in range(21):
        name = '%03d.pbi' % n
        o, s = ent[name]
        p = xa[o:o + s]
        cnt = struct.unpack_from('<I', p, 8)[0]
        idx = struct.unpack_from('<%dH' % cnt, p, 0x10)
        qs = sorted(set(i // 4 for i in idx))
        print('%s quads=%s' % (name, qs))


if __name__ == '__main__':
    main()
