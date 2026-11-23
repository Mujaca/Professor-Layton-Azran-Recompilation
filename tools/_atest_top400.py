"""400x240-Kandidaten mit linearem Tile-Order gegen den Top-Capture."""
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_botscan as BS
import _atest_imgc_np as D

REF = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 400, 3)
rows = []
for fa in BS.FAS:
    d = open(fa, 'rb').read()
    i = 0
    while True:
        o = d.find(b'IMGC00', i)
        if o < 0:
            break
        i = o + 1
        hd = D.parse_header(d, o)
        if not hd or (hd['w'], hd['h']) != (400, 240):
            continue
        if hd['bits'] not in (24, 32):
            continue
        r = BS.decode_linear(d, o) if hd['bits'] == 24 else None
        if r is not None:
            dd = int((np.abs(r.astype(np.int16)
                             - REF.astype(np.int16)).sum(2) > 60).sum())
            rows.append((dd, float(np.abs(r.astype(np.int16)
                                          - REF).mean()), 'lin', fa, o))
        rr = D.decode_buf(d, o)
        if rr is not None:
            a = rr[2]
            dd = int((np.abs(a.astype(np.int16)
                             - REF.astype(np.int16)).sum(2) > 60).sum())
            rows.append((dd, float(np.abs(a.astype(np.int16)
                                          - REF).mean()), 'swiz', fa, o))
rows.sort()
print('kandidaten', len(rows))
for r in rows[:14]:
    print('%6d/96000 mean %6.2f %-5s %-14s off=%d'
          % (r[0], r[1], r[2], r[3].split('/')[-1], r[4]))
