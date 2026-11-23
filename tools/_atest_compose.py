"""Komposition aus den besten 16x16-Patches (Ergebnis von _atest_tiles.py)."""
import json
import os
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_imgc_np as D

REF = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 400, 3)
rows = json.load(open('tools/out/atest_tiles.json'))
TS = 16
FAS = {'lt6_b.fa': 'out/romfs/lt6/arc/lt6_b.fa',
       'lt6_uk.fa': 'out/romfs/lt6/arc/lt6_uk.fa',
       'lt6_a.fa': 'out/romfs/lt6/arc/lt6_a.fa'}

cache = {}
comp = np.zeros((240, 400, 3), dtype=np.uint8)
ncc = np.array([r['ncc'] / 768.0 for r in rows])
for r in rows:
    x, y = r['tile']
    fn, off = r['atlas'].split('@')
    off = int(off)
    key = (fn, off)
    if key not in cache:
        cache[key] = D.decode_buf(open(FAS[fn], 'rb').read(), off)
    w, h, rgb, al = cache[key]
    u, v = r['u'], r['v']
    comp[y:y + TS, x:x + TS] = rgb[v:v + TS, u:u + TS]
d = np.abs(comp.astype(np.int16) - REF.astype(np.int16)).sum(2)
print('Komposition (16x16-Bestpatch je Kachel):')
print('  diff>60 : %d/%d' % (int((d > 60).sum()), 240 * 400))
print('  diff>60 sichtbar: %d/%d' % (int((d[:226] > 60).sum()), 226 * 400))
print('  mean abs diff %.2f' % float(np.abs(
    comp.astype(np.int16) - REF.astype(np.int16)).mean()))
print('  NCC(16x16) min/median/max: %.3f %.3f %.3f'
      % (ncc.min(), float(np.median(ncc)), ncc.max()))
open('tools/out/atest_compose_top.bin', 'wb').write(comp.tobytes())
print('  geschrieben: tools/out/atest_compose_top.bin')