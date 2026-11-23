"""Q3: Komposition aus den SAD-besten 16x16-Patches + Rechteck-Report."""
import json
import os
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_imgc_np as D

REF = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 400, 3)
rows = json.load(open('tools/out/atest_tiles_sad.json'))
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
    off = int(off)
    key = (fn, off)
    if key not in cache:
        cache[key] = D.decode_buf(open(FAS[fn], 'rb').read(), off)[2]
    rgb = cache[key]
    u, v = r['u'], r['v']
    comp[y:y + TS, x:x + TS] = rgb[v:v + TS, u:u + TS]
d = np.abs(comp.astype(np.int16) - REF.astype(np.int16))
cnt = int((d.sum(2) > 60).sum())
print('SAD-Komposition (bester 16x16-Patch je Kachel):')
print('  diff>60 : %d/%d' % (cnt, 240 * 400))
print('  diff>60 sichtbar: %d/%d'
      % (int((d[:226].sum(2) > 60).sum()), 226 * 400))
print('  mean abs diff %.2f' % float(d.mean()))
sads = np.array([r['sad'] for r in rows])
print('  Patch-SAD min/median %.2f/%.2f' % (sads.min(), np.median(sads)))
open('tools/out/atest_compose_top.bin', 'wb').write(comp.tobytes())
print('  geschrieben: tools/out/atest_compose_top.bin')

# --- Rechteck-Report pro Atlas ---
agg = {}
for r in rows:
    if not r['atlas']:
        continue
    agg.setdefault(r['atlas'], []).append(r)
print('--- Atlanten mit den meisten Kacheln ---')
for atl, rs in sorted(agg.items(), key=lambda kv: -len(kv[1]))[:6]:
    xs = [r['tile'][0] for r in rs]
    ys = [r['tile'][1] for r in rs]
    print('%-30s %2d/375 Kacheln (%.1f%%) bbox_screen=%d,%d..%d,%d '
          'meanSAD %.2f' % (atl, len(rs), len(rs) / 375 * 100,
                            min(xs), min(ys), max(xs) + TS, max(ys) + TS,
                            float(np.mean([r['sad'] for r in rs]))))
    byrow = {}
    for r in rs:
        byrow.setdefault(r['tile'][1], []).append(r)
    print('  Rechtecke (screen x,y,w,h -> atlas u,v; SAD):')
    for y in sorted(byrow):
        grp = sorted(byrow[y], key=lambda r: r['tile'][0])
        runs = []
        cur = [grp[0]]
        for r in grp[1:]:
            prev = cur[-1]
            if (r['tile'][0] == prev['tile'][0] + TS
                    and r['u'] == prev['u'] + TS and r['v'] == prev['v']):
                cur.append(r)
            else:
                runs.append(cur)
                cur = [r]
        runs.append(cur)
        for run in runs:
            if len(run) >= 2:
                print('    %3d,%3d,%3d,%2d -> %3d,%3d  SAD %.1f'
                      % (run[0]['tile'][0], y, len(run) * TS, TS,
                         run[0]['u'], run[0]['v'],
                         float(np.mean([r['sad'] for r in run]))))
