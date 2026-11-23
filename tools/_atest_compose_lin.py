"""Q3 (linear): Komposition aus den SAD-besten 16x16-Patches."""
import json
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_botscan as BS

REF = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 400, 3)
rows = json.load(open('tools/out/atest_tiles_lin.json'))
TS = 16
FAS = {'lt6_b.fa': 'out/romfs/lt6/arc/lt6_b.fa',
       'lt6_uk.fa': 'out/romfs/lt6/arc/lt6_uk.fa',
       'lt6_a.fa': 'out/romfs/lt6/arc/lt6_a.fa'}
cache = {}
comp = np.zeros((240, 400, 3), dtype=np.uint8)
for r in rows:
    x, y = r['tile']
    if not r['atlas']:
        continue
    fn, off = r['atlas'].split('@')
    key = (fn, int(off))
    if key not in cache:
        cache[key] = BS.decode_linear(open(FAS[fn], 'rb').read(), int(off))
    img = cache[key]
    u, v = r['u'], r['v']
    comp[y:y + TS, x:x + TS] = img[v:v + TS, u:u + TS]
d = np.abs(comp.astype(np.int16) - REF.astype(np.int16))
print('Komposition (linear, bester 16x16-Patch je Kachel):')
print('  diff>60 : %d/%d' % (int((d.sum(2) > 60).sum()), 240 * 400))
print('  diff>60 sichtbar: %d/%d'
      % (int((d[:226].sum(2) > 60).sum()), 226 * 400))
print('  mean abs diff %.2f' % float(d.mean()))
open('tools/out/atest_compose_top.bin', 'wb').write(comp.tobytes())
print('  geschrieben tools/out/atest_compose_top.bin')
