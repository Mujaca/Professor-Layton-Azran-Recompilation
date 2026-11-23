"""PBI-Frames: welche Quad-Indizes je Frame (title_b.xa)."""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _atest_xpck as X
import _imgc

FA = os.path.join(ROOT, 'out/romfs/lt6/arc/lt6_uk.fa')
XA_OFF, XA_LEN = 0x01526DC0, 0x9278


def main():
    d = open(FA, 'rb').read()
    cnt, ents = X.parse(d, XA_OFF, XA_LEN, _imgc.decomp)
    for e in sorted(ents, key=lambda z: z['name']):
        if not e['name'].endswith('.pbi'):
            continue
        o = XA_OFF + e['off']
        p = d[o:o + e['size']]
        n = struct.unpack_from('<I', p, 8)[0]
        idx = struct.unpack_from('<%dH' % n, p, 0x10) if n else ()
        qs = sorted(set(i // 4 for i in idx))
        print('%-10s n=%3d quads=%s' % (e['name'], n, qs))


if __name__ == '__main__':
    main()
