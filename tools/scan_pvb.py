"""Scannt alle XPCK-Packs der .fa nach XPVB-Quads mit Bildschirm-Positionen.

  python tools/scan_pvb.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import _imgc
import _atest_xpck as X


def main():
    for fa in ('out/romfs/lt6/arc/lt6_b.fa', 'out/romfs/lt6/arc/lt6_uk.fa'):
        d = open(fa, 'rb').read()
        i = 0
        hits = []
        while True:
            j = d.find(b'XPCK', i)
            if j < 0:
                break
            i = j + 1
            try:
                cnt, ents = X.parse(d, j, 0x200000, _imgc.decomp)
            except Exception:
                continue
            names = [e['name'] for e in ents]
            pvbs = [e for e in ents if e['name'].endswith('.pvb')]
            if not pvbs:
                continue
            for e in pvbs:
                base = j + e['off']
                blob = d[base:base + e['size']]
                if blob[:4] != b'XPVB' or len(blob) < 20:
                    continue
                try:
                    ao, ae, vo, st, vc = struct.unpack_from('<HHHHI', blob, 4)
                    vtx = _imgc.decomp(blob[vo:])
                except Exception:
                    continue
                if st < 20 or st > 64 or vc < 4 or vc > 4000:
                    continue
                if len(vtx) < st * vc:
                    continue
                quads = vc // 4
                # Positionen: erster Vertex je Quad
                scr = 0
                for q in range(quads):
                    px, py, pz = struct.unpack_from('<3f', vtx, q * 4 * st)
                    if -64.0 <= px <= 464.0 and -64.0 <= py <= 304.0:
                        scr += 1
                hits.append((scr, quads, j, e['name'], q if False else 0,
                             os.path.basename(fa)))
        hits.sort(reverse=True)
        print(fa, 'packs mit .pvb:', len(hits))
        for h in hits[:15]:
            print('  %4d/%4d screen-quads  @%08X %-18s %s'
                  % (h[0], h[1], h[2], h[3], h[5]))


main()
