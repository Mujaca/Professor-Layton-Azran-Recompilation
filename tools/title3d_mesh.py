"""title.xc-Scene: XPVB innerhalb der .prm-Eintraege vermessen.

Zeigt, ob die Titel-Geometrie echte 3D-Positionen enthaelt (Bounding-Box,
UV-Bereich), damit Stufe 3 (Modell-Render) absehbar ist.

Aufruf: python tools/title3d_mesh.py
"""
import os
import struct
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import title3d_probe as T  # noqa: E402
import _imgc  # noqa: E402


def pvb_at(d, off):
    """XPVB an off: (vcount, stride, vtx_off, pos, uv)."""
    assert d[off:off + 4] == b'XPVB', d[off:off + 4]
    vtx, stride, vcount = struct.unpack_from('<HHHHI', d, off + 4)[2:]
    vtx = vtx
    vbuf = _imgc.decomp(d[off + vtx:off + vtx + 0x4000])
    pos = []
    uv = []
    for i in range(vcount):
        p = i * stride
        x, y, z = struct.unpack_from('<3f', vbuf, p)
        u, v = struct.unpack_from('<2f', vbuf, p + 12)
        pos.append((x, y, z))
        uv.append((u, v))
    return vcount, stride, np.array(pos), np.array(uv)


def main():
    d, ents = T.entries()
    for s, fo, sz, magic in ents:
        if not s.endswith('.prm'):
            continue
        i = d.find(b'XPVB', fo, fo + sz)
        if i < 0:
            print('%s: kein XPVB' % s)
            continue
        try:
            n, stride, pos, uv = pvb_at(d, i)
        except Exception as e:
            print('%s: Fehler %s' % (s, e))
            continue
        print('%s XPVB @%06X verts=%d stride=%d' % (s, i, n, stride))
        print('   pos x[%.1f..%.1f] y[%.1f..%.1f] z[%.1f..%.1f]'
              % (pos[:, 0].min(), pos[:, 0].max(), pos[:, 1].min(),
                 pos[:, 1].max(), pos[:, 2].min(), pos[:, 2].max()))
        print('   uv  u[%.1f..%.1f] v[%.1f..%.1f]'
              % (uv[:, 0].min(), uv[:, 0].max(), uv[:, 1].min(),
                 uv[:, 1].max()))


if __name__ == '__main__':
    main()
